// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/bidirectional_stream_quic_impl.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "net/base/net_errors.h"
#include "net/http/bidirectional_stream_request_info.h"
#include "net/http/http_util.h"
#include "net/socket/next_proto.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_connection.h"
#include "quic_http_stream.h"

namespace net {
namespace {
// The most HTTP Datagrams, and bytes of them, kept while the initial headers
// that came with them wait to be delivered.
constexpr size_t kMaxEarlyDatagrams = 16;
constexpr size_t kMaxEarlyDatagramBytes = 64 * 1024;

// Sets a boolean to a value, and restores it to the previous value once
// the saver goes out of scope.
class ScopedBoolSaver {
 public:
  ScopedBoolSaver(bool* var, bool new_val) : var_(var), old_val_(*var) {
    *var_ = new_val;
  }

  ~ScopedBoolSaver() { *var_ = old_val_; }

 private:
  raw_ptr<bool> var_;
  bool old_val_;
};
}  // namespace

BidirectionalStreamQuicImpl::BidirectionalStreamQuicImpl(
    std::unique_ptr<QuicChromiumClientSession::Handle> session)
    : session_(std::move(session)) {}

BidirectionalStreamQuicImpl::~BidirectionalStreamQuicImpl() {
  if (stream_) {
    delegate_ = nullptr;
    stream_->Reset(quic::QUIC_STREAM_CANCELLED);
  }
}

void BidirectionalStreamQuicImpl::Start(
    const BidirectionalStreamRequestInfo* request_info,
    const NetLogWithSource& net_log,
    bool send_request_headers_automatically,
    BidirectionalStreamImpl::Delegate* delegate,
    std::unique_ptr<base::OneShotTimer> timer,
    const NetworkTrafficAnnotationTag& traffic_annotation) {
  ScopedBoolSaver saver(&may_invoke_callbacks_, false);
  DCHECK(!stream_);
  CHECK(delegate);
  DLOG_IF(WARNING, !session_->IsConnected())
      << "Trying to start request headers after session has been closed.";

  net_log.AddEventReferencingSource(
      NetLogEventType::BIDIRECTIONAL_STREAM_BOUND_TO_QUIC_SESSION,
      session_->net_log().source());

  send_request_headers_automatically_ = send_request_headers_automatically;
  delegate_ = delegate;
  request_info_ = request_info;

  if (request_info_->method == "CONNECT") {
    connect_protocol_ =
        request_info_->extra_headers.GetHeader("-connect-protocol");
  }
  http_datagrams_ = connect_protocol_ == "connect-udp";
  if (http_datagrams_) {
    // connect-udp needs HTTP/3 datagrams and extended CONNECT, which the
    // SETTINGS of the server announce: a server without them gets no request.
    int rv = session_->WaitForSettings(
        base::BindOnce(&BidirectionalStreamQuicImpl::OnSettingsReceived,
                       weak_factory_.GetWeakPtr(), traffic_annotation));
    if (rv != ERR_IO_PENDING) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&BidirectionalStreamQuicImpl::OnSettingsReceived,
                         weak_factory_.GetWeakPtr(), traffic_annotation, rv));
    }
    return;
  }

  RequestStream(traffic_annotation);
}

void BidirectionalStreamQuicImpl::OnSettingsReceived(
    const NetworkTrafficAnnotationTag& traffic_annotation,
    int rv) {
  if (rv == OK && !session_->IsConnected()) {
    rv = ERR_CONNECTION_CLOSED;
  }
  if (rv == OK && !session_->SupportsH3DatagramAndExtendedConnect()) {
    rv = ERR_METHOD_NOT_SUPPORTED;
  }
  if (rv != OK) {
    NotifyError(rv);
    return;
  }

  ScopedBoolSaver saver(&may_invoke_callbacks_, false);
  RequestStream(traffic_annotation);
}

void BidirectionalStreamQuicImpl::RequestStream(
    const NetworkTrafficAnnotationTag& traffic_annotation) {
  // Only allow SAFE methods to use early data, unless overridden by the caller.
  bool use_early_data = HttpUtil::IsMethodSafe(request_info_->method);
  use_early_data |= request_info_->allow_early_data_override;

  int rv = session_->RequestStream(
      !use_early_data,
      base::BindOnce(&BidirectionalStreamQuicImpl::OnStreamReady,
                     weak_factory_.GetWeakPtr()),
      traffic_annotation);
  if (rv == ERR_IO_PENDING)
    return;

  if (rv != OK) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            &BidirectionalStreamQuicImpl::NotifyError,
            weak_factory_.GetWeakPtr(),
            session_->OneRttKeysAvailable() ? rv : ERR_QUIC_HANDSHAKE_FAILED));
    return;
  }

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&BidirectionalStreamQuicImpl::OnStreamReady,
                                weak_factory_.GetWeakPtr(), rv));
}

void BidirectionalStreamQuicImpl::SendRequestHeaders() {
  ScopedBoolSaver saver(&may_invoke_callbacks_, false);
  int rv = WriteHeaders();
  if (rv < 0) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&BidirectionalStreamQuicImpl::NotifyError,
                                  weak_factory_.GetWeakPtr(), rv));
  }
}

int BidirectionalStreamQuicImpl::WriteHeaders() {
  DCHECK(!has_sent_headers_);

  quiche::HttpHeaderBlock headers;
  HttpRequestInfo http_request_info;
  http_request_info.url = request_info_->url;
  http_request_info.method = request_info_->method;
  http_request_info.extra_headers = request_info_->extra_headers;

  if (connect_protocol_) {
    CreateSpdyHeadersFromHttpRequestForExtendedConnect(
        http_request_info, std::nullopt, *connect_protocol_,
        http_request_info.extra_headers, &headers);
  } else {
    CreateSpdyHeadersFromHttpRequest(http_request_info, std::nullopt,
                                     http_request_info.extra_headers, &headers);
  }
  int rv = stream_->WriteHeaders(std::move(headers),
                                 request_info_->end_stream_on_headers, nullptr);
  if (rv >= 0) {
    headers_bytes_sent_ += rv;
    has_sent_headers_ = true;
  }
  return rv;
}

int BidirectionalStreamQuicImpl::ReadData(IOBuffer* buffer, int buffer_len) {
  ScopedBoolSaver saver(&may_invoke_callbacks_, false);
  DCHECK(buffer);
  DCHECK(buffer_len);

  int rv = stream_->ReadBody(
      buffer, buffer_len,
      base::BindOnce(&BidirectionalStreamQuicImpl::OnReadDataComplete,
                     weak_factory_.GetWeakPtr()));
  if (rv == ERR_IO_PENDING) {
    read_buffer_ = buffer;
    read_buffer_len_ = buffer_len;
    return ERR_IO_PENDING;
  }

  if (rv < 0)
    return rv;

  // If the write side is closed, OnFinRead() will call
  // BidirectionalStreamQuicImpl::OnClose().
  if (stream_->IsDoneReading())
    stream_->OnFinRead();

  return rv;
}

void BidirectionalStreamQuicImpl::SendvData(
    const std::vector<scoped_refptr<IOBuffer>>& buffers,
    const std::vector<int>& lengths,
    bool end_stream) {
  ScopedBoolSaver saver(&may_invoke_callbacks_, false);
  DCHECK_EQ(buffers.size(), lengths.size());

  if (!stream_->IsOpen()) {
    LOG(ERROR) << "Trying to send data after stream has been closed.";
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&BidirectionalStreamQuicImpl::NotifyError,
                                  weak_factory_.GetWeakPtr(), ERR_UNEXPECTED));
    return;
  }

  std::unique_ptr<quic::QuicConnection::ScopedPacketFlusher> bundler(
      session_->CreatePacketBundler());
  if (!has_sent_headers_) {
    DCHECK(!send_request_headers_automatically_);
    int rv = WriteHeaders();
    if (rv < 0) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&BidirectionalStreamQuicImpl::NotifyError,
                                    weak_factory_.GetWeakPtr(), rv));
      return;
    }
  }

  int rv = stream_->WritevStreamData(
      buffers, lengths, end_stream,
      base::BindOnce(&BidirectionalStreamQuicImpl::OnSendDataComplete,
                     weak_factory_.GetWeakPtr()));

  if (rv != ERR_IO_PENDING) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&BidirectionalStreamQuicImpl::OnSendDataComplete,
                       weak_factory_.GetWeakPtr(), rv));
  }
}

NextProto BidirectionalStreamQuicImpl::GetProtocol() const {
  return negotiated_protocol_;
}

int64_t BidirectionalStreamQuicImpl::GetTotalReceivedBytes() const {
  if (stream_) {
    DCHECK_LE(stream_->NumBytesConsumed(), stream_->stream_bytes_read());
    // Only count the uniquely received bytes.
    return stream_->NumBytesConsumed();
  }
  return closed_stream_received_bytes_;
}

int64_t BidirectionalStreamQuicImpl::GetTotalSentBytes() const {
  if (stream_) {
    return stream_->stream_bytes_written();
  }
  return closed_stream_sent_bytes_;
}

bool BidirectionalStreamQuicImpl::GetLoadTimingInfo(
    LoadTimingInfo* load_timing_info) const {
  bool is_first_stream = closed_is_first_stream_;
  if (stream_)
    is_first_stream = stream_->IsFirstStream();
  if (is_first_stream) {
    load_timing_info->socket_reused = false;
    load_timing_info->connect_timing = connect_timing_;
  } else {
    load_timing_info->socket_reused = true;
  }
  return true;
}

void BidirectionalStreamQuicImpl::PopulateNetErrorDetails(
    NetErrorDetails* details) {
  DCHECK(details);
  details->connection_info =
      QuicHttpStream::ConnectionInfoFromQuicVersion(session_->GetQuicVersion());
  session_->PopulateNetErrorDetails(details);
  if (session_->OneRttKeysAvailable() && stream_)
    details->quic_connection_error = stream_->connection_error();
}

int BidirectionalStreamQuicImpl::SendHttp3Datagram(std::string_view payload) {
  if (!http_datagrams_) {
    return ERR_NOT_IMPLEMENTED;
  }
  if (!stream_) {
    return ERR_UNEXPECTED;
  }
  return stream_->SendHttp3Datagram(payload);
}

int BidirectionalStreamQuicImpl::WriteDatagramCapsule(
    std::string_view payload,
    CompletionOnceCallback callback) {
  if (!http_datagrams_) {
    return ERR_NOT_IMPLEMENTED;
  }
  if (!stream_ || !has_sent_headers_) {
    return ERR_UNEXPECTED;
  }
  return stream_->WriteDatagramCapsule(payload, std::move(callback));
}

base::ScopedClosureRunner BidirectionalStreamQuicImpl::BundleWrites() {
  return base::ScopedClosureRunner(base::BindOnce(
      [](std::unique_ptr<quic::QuicConnection::ScopedPacketFlusher> bundler) {},
      session_->CreatePacketBundler()));
}

size_t BidirectionalStreamQuicImpl::GetMaxDatagramPayload() const {
  if (!http_datagrams_ || !stream_) {
    return 0;
  }
  return stream_->GetMaxDatagramSize();
}

void BidirectionalStreamQuicImpl::OnHttp3Datagram(quic::QuicStreamId stream_id,
                                                  std::string_view payload) {
  if (!delegate_) {
    return;
  }
  if (!initial_headers_delivered_) {
    // The initial headers reach the delegate from a posted task, and the
    // capsules that came with them are parsed at once.
    if (early_datagrams_.size() < kMaxEarlyDatagrams &&
        early_datagram_bytes_ + payload.size() <= kMaxEarlyDatagramBytes) {
      early_datagram_bytes_ += payload.size();
      early_datagrams_.emplace_back(payload);
    }
    return;
  }
  delegate_->OnHttp3Datagram(payload);
}

void BidirectionalStreamQuicImpl::OnUnknownCapsule(
    quic::QuicStreamId stream_id,
    const quiche::UnknownCapsule& capsule) {
  // Capsules of an unknown type are skipped (RFC 9297, section 3.2).
}

void BidirectionalStreamQuicImpl::OnStreamReady(int rv) {
  DCHECK_NE(ERR_IO_PENDING, rv);
  DCHECK(!stream_);
  if (rv != OK) {
    NotifyError(rv);
    return;
  }

  stream_ = session_->ReleaseStream();
  DCHECK(stream_);

  if (!stream_->IsOpen()) {
    NotifyError(ERR_CONNECTION_CLOSED);
    return;
  }

  if (http_datagrams_) {
    // Before anything is read: the body of the response is read as capsules.
    stream_->RegisterHttp3DatagramVisitor(this);
  }

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&BidirectionalStreamQuicImpl::ReadInitialHeaders,
                     weak_factory_.GetWeakPtr()));

  NotifyStreamReady();
}

void BidirectionalStreamQuicImpl::OnSendDataComplete(int rv) {
  CHECK(may_invoke_callbacks_);
  DCHECK_NE(ERR_IO_PENDING, rv);
  if (rv < 0) {
    NotifyError(rv);
    return;
  }

  if (delegate_)
    delegate_->OnDataSent();
}

void BidirectionalStreamQuicImpl::OnReadInitialHeadersComplete(int rv) {
  CHECK(may_invoke_callbacks_);
  DCHECK_NE(ERR_IO_PENDING, rv);
  if (rv < 0) {
    NotifyError(rv);
    return;
  }

  headers_bytes_received_ += rv;
  negotiated_protocol_ = NextProto::kProtoQUIC;
  connect_timing_ = session_->GetConnectTiming();
  // With HTTP Datagrams the body is capsules up to the end of the stream, and
  // the delegate reads it to learn of that end: no trailers are read, which
  // could fail on a stream already closed before the end is read.
  if (!http_datagrams_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&BidirectionalStreamQuicImpl::ReadTrailingHeaders,
                       weak_factory_.GetWeakPtr()));
  }
  initial_headers_delivered_ = true;
  if (!delegate_) {
    return;
  }
  base::WeakPtr<BidirectionalStreamQuicImpl> weak_this =
      weak_factory_.GetWeakPtr();
  delegate_->OnHeadersReceived(initial_headers_);
  if (!weak_this) {
    return;
  }
  // The HTTP Datagrams that came with the headers follow them.
  std::vector<std::string> early_datagrams;
  early_datagrams.swap(early_datagrams_);
  early_datagram_bytes_ = 0;
  for (const std::string& datagram : early_datagrams) {
    if (!weak_this || !delegate_) {
      return;
    }
    delegate_->OnHttp3Datagram(datagram);
  }
}

void BidirectionalStreamQuicImpl::ReadInitialHeaders() {
  int rv = stream_->ReadInitialHeaders(
      &initial_headers_,
      base::BindOnce(&BidirectionalStreamQuicImpl::OnReadInitialHeadersComplete,
                     weak_factory_.GetWeakPtr()));

  if (rv != ERR_IO_PENDING)
    OnReadInitialHeadersComplete(rv);
}

void BidirectionalStreamQuicImpl::ReadTrailingHeaders() {
  int rv = stream_->ReadTrailingHeaders(
      &trailing_headers_,
      base::BindOnce(
          &BidirectionalStreamQuicImpl::OnReadTrailingHeadersComplete,
          weak_factory_.GetWeakPtr()));

  if (rv != ERR_IO_PENDING)
    OnReadTrailingHeadersComplete(rv);
}

void BidirectionalStreamQuicImpl::OnReadTrailingHeadersComplete(int rv) {
  CHECK(may_invoke_callbacks_);
  DCHECK_NE(ERR_IO_PENDING, rv);
  if (rv < 0) {
    NotifyError(rv);
    return;
  }

  headers_bytes_received_ += rv;

  if (delegate_)
    delegate_->OnTrailersReceived(trailing_headers_);
}

void BidirectionalStreamQuicImpl::OnReadDataComplete(int rv) {
  CHECK(may_invoke_callbacks_);

  read_buffer_ = nullptr;
  read_buffer_len_ = 0;

  // If the write side is closed, OnFinRead() will call
  // BidirectionalStreamQuicImpl::OnClose().
  if (stream_->IsDoneReading())
    stream_->OnFinRead();

  if (!delegate_)
    return;

  if (rv < 0)
    NotifyError(rv);
  else
    delegate_->OnDataRead(rv);
}

void BidirectionalStreamQuicImpl::NotifyError(int error) {
  NotifyErrorImpl(error, /*notify_delegate_later*/ false);
}

void BidirectionalStreamQuicImpl::NotifyErrorImpl(int error,
                                                  bool notify_delegate_later) {
  DCHECK_NE(OK, error);
  DCHECK_NE(ERR_IO_PENDING, error);

  ResetStream();
  if (delegate_) {
    response_status_ = error;
    BidirectionalStreamImpl::Delegate* delegate = delegate_;
    delegate_ = nullptr;
    // Cancel any pending callback.
    weak_factory_.InvalidateWeakPtrs();
    if (notify_delegate_later) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&BidirectionalStreamQuicImpl::NotifyFailure,
                         weak_factory_.GetWeakPtr(), delegate, error));
    } else {
      NotifyFailure(delegate, error);
      // |this| might be destroyed at this point.
    }
  }
}

void BidirectionalStreamQuicImpl::NotifyFailure(
    BidirectionalStreamImpl::Delegate* delegate,
    int error) {
  CHECK(may_invoke_callbacks_);
  delegate->OnFailed(error);
  // |this| might be destroyed at this point.
}

void BidirectionalStreamQuicImpl::NotifyStreamReady() {
  CHECK(may_invoke_callbacks_);
  // With HTTP Datagrams, the capsules sent before the stream was there go with
  // the request headers.
  std::unique_ptr<quic::QuicConnection::ScopedPacketFlusher> bundler;
  if (http_datagrams_) {
    bundler = session_->CreatePacketBundler();
  }
  if (send_request_headers_automatically_) {
    int rv = WriteHeaders();
    if (rv < 0) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&BidirectionalStreamQuicImpl::NotifyError,
                                    weak_factory_.GetWeakPtr(), rv));
      return;
    }
  }

  if (delegate_)
    delegate_->OnStreamReady(has_sent_headers_);
}

void BidirectionalStreamQuicImpl::ResetStream() {
  if (!stream_)
    return;
  closed_stream_received_bytes_ = stream_->stream_bytes_read();
  closed_stream_sent_bytes_ = stream_->stream_bytes_written();
  closed_is_first_stream_ = stream_->IsFirstStream();
}

}  // namespace net
