// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/grpc_support/include/datagram_stream_c.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/containers/circular_deque.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "net/base/http_user_agent_settings.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/request_priority.h"
#include "net/base/schemeful_site.h"
#include "net/http/bidirectional_stream.h"
#include "net/http/bidirectional_stream_request_info.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/http_util.h"
#include "net/socket/next_proto.h"
#include "net/third_party/quiche/src/quiche/common/http/http_header_block.h"
#include "net/third_party/quiche/src/quiche/common/quiche_data_reader.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_getter.h"
#include "url/gurl.h"

namespace {

// The Context ID of the datagrams: a constant of their format, which the
// library adds to what it sends and strips from what it receives.
constexpr uint64_t kContextId = 2;

// The most sends that wait for the network thread, and the most capsules that
// wait for the stream.
constexpr int kMaxPendingSends = 64;
constexpr size_t kMaxQueuedCapsules = 64;

// How long a capsule may wait for the stream to take it. Stale UDP is worse
// than lost UDP: an older capsule is dropped.
constexpr base::TimeDelta kMaxCapsuleWait = base::Milliseconds(100);

// The longest payload: a name of 255 bytes with its type and length, a port
// and 65507 bytes of UDP data. The server resets a stream with a longer
// capsule.
constexpr int kMaxPayloadLength = 1 + 1 + 255 + 2 + 65507;

// The response headers as a bidirectional_stream_header_array, with coalesced
// values split by '\0' as bidirectional_stream_c.cc does.
class HeadersArray : public bidirectional_stream_header_array {
 public:
  explicit HeadersArray(const quiche::HttpHeaderBlock& header_block) {
    for (const auto& [name, value] : header_block) {
      for (std::string_view part : base::SplitStringPiece(
               value, std::string_view("\0", 1), base::KEEP_WHITESPACE,
               base::SPLIT_WANT_ALL)) {
        strings_.emplace_back(name, part);
      }
    }
    entries_.reserve(strings_.size());
    for (const auto& [name, value] : strings_) {
      entries_.push_back({name.c_str(), value.c_str()});
    }
    count = capacity = entries_.size();
    headers = entries_.data();
  }

  HeadersArray(const HeadersArray&) = delete;
  HeadersArray& operator=(const HeadersArray&) = delete;

 private:
  std::vector<std::pair<std::string, std::string>> strings_;
  std::vector<bidirectional_stream_header> entries_;
};

// The C object of a datagram stream: a connect-udp request on a
// net::BidirectionalStream.
//
// Threading: created, started, sent to, canceled and destroyed from any
// thread; everything else happens on the network thread, where the callbacks
// are invoked. The members shared between threads are atomic.
class DatagramStreamAdapter final : public net::BidirectionalStream::Delegate {
 public:
  DatagramStreamAdapter(stream_engine* engine,
                        void* annotation,
                        const datagram_stream_callback* callback);

  DatagramStreamAdapter(const DatagramStreamAdapter&) = delete;
  DatagramStreamAdapter& operator=(const DatagramStreamAdapter&) = delete;

  static DatagramStreamAdapter* FromCStream(datagram_stream* stream) {
    DCHECK(stream);
    return static_cast<DatagramStreamAdapter*>(stream->obj);
  }

  datagram_stream* c_stream() { return &c_stream_; }

  int Start(const char* url, const bidirectional_stream_header_array* headers);
  int Send(const char* data, int length, int flags);
  int max_payload() const {
    return max_payload_.load(std::memory_order_relaxed);
  }
  uint64_t dropped_capsules() const {
    return dropped_capsules_.load(std::memory_order_relaxed);
  }
  void Cancel();
  void Destroy();

 private:
  enum class State {
    // datagram_stream_start() has not run on the network thread yet.
    kNotStarted,
    // The request is on its way; capsules only.
    kStarted,
    // The response was 200: datagrams, and capsules for what does not fit.
    kReady,
    // Closed, failed or canceled.
    kDone,
  };

  struct QueuedCapsule {
    // The Context ID and the payload.
    std::string payload;
    // When the stream could have taken it: null until the request headers
    // are sent.
    base::TimeTicks since;
  };

  ~DatagramStreamAdapter() override;

  // net::BidirectionalStream::Delegate implementation:
  void OnStreamReady(bool request_headers_sent) override;
  void OnHeadersReceived(const quiche::HttpHeaderBlock& response_headers,
                         const net::ProxyInfo& used_proxy_info) override;
  void OnDataRead(int bytes_read) override;
  void OnDataSent() override;
  void OnTrailersReceived(const quiche::HttpHeaderBlock& trailers) override;
  void OnFailed(int error) override;
  void OnHttp3Datagram(std::string_view payload) override;

  void StartOnNetworkThread(
      std::unique_ptr<net::BidirectionalStreamRequestInfo> request_info);
  void SendOnNetworkThread(std::string payload, bool allow_capsule);
  void CancelOnNetworkThread();
  void DestroyOnNetworkThread();

  // Reads the stream, whose body is parsed as capsules, for its end.
  void ReadEndOfStream();
  void OnEndOfStream(int rv);

  void QueueCapsule(std::string payload);
  // Drops the queued capsules that waited too long, and writes the others
  // while the stream takes them at once.
  void WriteCapsules();
  void OnStaleCapsuleTimer();
  void OnCapsuleWritten(int rv);
  // The write side of the stream is closed (reset after a STOP_SENDING):
  // nothing more is sent, and the queued capsules are dropped.
  void CloseWrites();
  // Publishes the queue to Send(). Its head is the capsule in the send buffer
  // of the stream, if there is one, or else the oldest queued capsule.
  void UpdateCapsuleQueue();

  // The payload that fits in a datagram of |max_datagram_size|.
  static int MaxPayloadFor(size_t max_datagram_size);
  // Updates max_payload_ and tells the callback if it changed.
  void UpdateMaxPayload(size_t max_datagram_size);

  // Enters kDone, where no callback is made and nothing is sent.
  void Finish();
  // Destroys the stream from a task, as it may be up the stack now.
  void ReleaseStreamLater();
  void DropStream();

  bool IsOnNetworkThread() const;
  void PostToNetworkThread(const base::Location& from_here,
                           base::OnceClosure task);

  datagram_stream c_stream_;
  const raw_ptr<const datagram_stream_callback> callback_;
  const raw_ptr<net::URLRequestContextGetter> request_context_getter_;

  // Shared between threads.
  std::atomic<bool> started_{false};
  std::atomic<bool> canceled_{false};
  // Started, and neither ended nor canceled: Send() takes datagrams.
  std::atomic<bool> open_{false};
  std::atomic<int> pending_sends_{0};
  std::atomic<size_t> queued_capsules_{0};
  // When the oldest queued capsule could have been written, in microseconds
  // since the origin of base::TimeTicks; 0 if none waits.
  std::atomic<int64_t> capsule_head_since_us_{0};
  std::atomic<int> max_payload_{0};
  std::atomic<uint64_t> dropped_capsules_{0};

  // Network thread only.
  State state_ = State::kNotStarted;
  std::unique_ptr<net::BidirectionalStream> bidi_stream_;
  // The request headers are sent: capsules may follow.
  bool stream_takes_capsules_ = false;
  // A capsule waits in the send buffer of the stream, since that time.
  bool capsule_in_stream_ = false;
  base::TimeTicks capsule_in_stream_since_;
  base::circular_deque<QueuedCapsule> capsules_;
  // Drops the oldest queued capsule when it has waited too long; armed for
  // the capsule queued since that time.
  base::OneShotTimer stale_capsule_timer_;
  base::TimeTicks stale_capsule_timer_since_;
  // See CloseWrites().
  bool writes_closed_ = false;
  scoped_refptr<net::IOBufferWithSize> read_buffer_;

  base::WeakPtr<DatagramStreamAdapter> weak_this_;
  base::WeakPtrFactory<DatagramStreamAdapter> weak_factory_{this};
};

DatagramStreamAdapter::DatagramStreamAdapter(
    stream_engine* engine,
    void* annotation,
    const datagram_stream_callback* callback)
    : callback_(callback),
      request_context_getter_(
          reinterpret_cast<net::URLRequestContextGetter*>(engine->obj)) {
  DCHECK(request_context_getter_);
  c_stream_.obj = this;
  c_stream_.annotation = annotation;
  weak_this_ = weak_factory_.GetWeakPtr();
}

DatagramStreamAdapter::~DatagramStreamAdapter() {
  DCHECK(IsOnNetworkThread());
  // Destroy() is a task of its own: the stream is not up the stack. It
  // forgets its delegate before it resets the QUIC stream.
  bidi_stream_.reset();
}

int DatagramStreamAdapter::Start(
    const char* url,
    const bidirectional_stream_header_array* headers) {
  auto request_info = std::make_unique<net::BidirectionalStreamRequestInfo>();
  request_info->url = GURL(url ? url : "");
  if (!request_info->url.is_valid()) {
    return -1;
  }
  request_info->method = "CONNECT";
  request_info->priority = net::DEFAULT_PRIORITY;
  if (headers) {
    for (size_t i = 0; i < headers->count; ++i) {
      const bidirectional_stream_header& header =
          UNSAFE_TODO(headers->headers[i]);
      std::string name(header.key ? header.key : "");
      std::string value(header.value ? header.value : "");
      if (!net::HttpUtil::IsValidHeaderName(name) ||
          !net::HttpUtil::IsValidHeaderValue(value)) {
        return static_cast<int>(i + 1);
      }
      request_info->extra_headers.SetHeader(name, value);
    }
  }
  if (auto network_isolation_key_header =
          request_info->extra_headers.GetHeader("-network-isolation-key")) {
    if (net::SchemefulSite(GURL(*network_isolation_key_header)).opaque()) {
      return -1;
    }
  }
  // connect-udp (RFC 9298), which needs HTTP/3 datagrams: over QUIC only.
  request_info->extra_headers.SetHeader("-connect-protocol", "connect-udp");
  request_info->extra_headers.SetHeaderIfMissing("capsule-protocol", "?1");
  request_info->extra_headers.SetHeader("-force-quic", "true");
  request_info->end_stream_on_headers = false;

  if (canceled_.load(std::memory_order_acquire) || started_.exchange(true)) {
    return -1;
  }
  open_.store(true, std::memory_order_release);
  PostToNetworkThread(
      FROM_HERE, base::BindOnce(&DatagramStreamAdapter::StartOnNetworkThread,
                                weak_this_, std::move(request_info)));
  return 0;
}

int DatagramStreamAdapter::Send(const char* data, int length, int flags) {
  if (length < 0 || length > kMaxPayloadLength || (length > 0 && !data)) {
    return DATAGRAM_STREAM_TOO_LARGE;
  }
  if (!open_.load(std::memory_order_acquire)) {
    return DATAGRAM_STREAM_CLOSED;
  }
  const bool allow_capsule = (flags & DATAGRAM_STREAM_SEND_ALLOW_CAPSULE) != 0;
  const int max_payload = max_payload_.load(std::memory_order_relaxed);
  if (max_payload == 0 || length > max_payload) {
    // A capsule.
    if (!allow_capsule) {
      return DATAGRAM_STREAM_TOO_LARGE;
    }
    const int64_t head_since_us =
        capsule_head_since_us_.load(std::memory_order_relaxed);
    if (head_since_us != 0 &&
        base::TimeTicks::Now().since_origin().InMicroseconds() - head_since_us >
            kMaxCapsuleWait.InMicroseconds()) {
      return DATAGRAM_STREAM_CAPSULE_TIMEOUT;
    }
    if (queued_capsules_.load(std::memory_order_relaxed) >=
        kMaxQueuedCapsules) {
      return DATAGRAM_STREAM_QUEUE_FULL;
    }
  }
  if (pending_sends_.fetch_add(1, std::memory_order_relaxed) >=
      kMaxPendingSends) {
    pending_sends_.fetch_sub(1, std::memory_order_relaxed);
    return DATAGRAM_STREAM_QUEUE_FULL;
  }

  // The one copy of the payload, behind its Context ID.
  std::string payload;
  payload.reserve(static_cast<size_t>(length) + 1);
  payload.push_back(static_cast<char>(kContextId));
  if (length > 0) {
    const base::span<const char> bytes =
        UNSAFE_TODO(base::span(data, static_cast<size_t>(length)));
    payload.append(bytes.begin(), bytes.end());
  }
  PostToNetworkThread(
      FROM_HERE, base::BindOnce(&DatagramStreamAdapter::SendOnNetworkThread,
                                weak_this_, std::move(payload), allow_capsule));
  return DATAGRAM_STREAM_OK;
}

void DatagramStreamAdapter::Cancel() {
  canceled_.store(true, std::memory_order_release);
  open_.store(false, std::memory_order_release);
  PostToNetworkThread(
      FROM_HERE, base::BindOnce(&DatagramStreamAdapter::CancelOnNetworkThread,
                                weak_this_));
}

void DatagramStreamAdapter::Destroy() {
  open_.store(false, std::memory_order_release);
  PostToNetworkThread(
      FROM_HERE, base::BindOnce(&DatagramStreamAdapter::DestroyOnNetworkThread,
                                base::Unretained(this)));
}

void DatagramStreamAdapter::StartOnNetworkThread(
    std::unique_ptr<net::BidirectionalStreamRequestInfo> request_info) {
  DCHECK(IsOnNetworkThread());
  DCHECK(state_ == State::kNotStarted);
  if (canceled_.load(std::memory_order_acquire)) {
    // Canceled while the start was on its way: the request never goes.
    Finish();
    callback_->on_canceled(c_stream());
    return;
  }
  net::URLRequestContext* request_context =
      request_context_getter_->GetURLRequestContext();
  request_info->extra_headers.SetHeaderIfMissing(
      net::HttpRequestHeaders::kUserAgent,
      request_context->http_user_agent_settings()->GetUserAgent());
  state_ = State::kStarted;
  bidi_stream_ = std::make_unique<net::BidirectionalStream>(
      std::move(request_info),
      request_context->http_transaction_factory()->GetSession(),
      /*send_request_headers_automatically=*/true, this);
}

void DatagramStreamAdapter::SendOnNetworkThread(std::string payload,
                                                bool allow_capsule) {
  DCHECK(IsOnNetworkThread());
  pending_sends_.fetch_sub(1, std::memory_order_relaxed);
  if (state_ == State::kDone) {
    return;
  }
  if (state_ == State::kReady) {
    const size_t max_datagram_size = bidi_stream_->GetMaxDatagramPayload();
    UpdateMaxPayload(max_datagram_size);
    if (payload.size() <= max_datagram_size) {
      const int rv = bidi_stream_->SendHttp3Datagram(payload);
      if (rv == net::OK) {
        return;
      }
      if (rv == net::ERR_CONNECTION_CLOSED) {
        CloseWrites();
        dropped_capsules_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    // It does not fit after all, as the size changed since Send(), or the
    // connection takes no datagram: dropped, unless it may go in a capsule.
    if (!allow_capsule) {
      dropped_capsules_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  QueueCapsule(std::move(payload));
}

void DatagramStreamAdapter::CancelOnNetworkThread() {
  DCHECK(IsOnNetworkThread());
  // Not started yet: StartOnNetworkThread() sees the cancel.
  if (state_ == State::kNotStarted || state_ == State::kDone) {
    return;
  }
  Finish();
  // A task of its own: the stream is not up the stack.
  bidi_stream_.reset();
  callback_->on_canceled(c_stream());
}

void DatagramStreamAdapter::DestroyOnNetworkThread() {
  DCHECK(IsOnNetworkThread());
  delete this;
}

void DatagramStreamAdapter::OnStreamReady(bool request_headers_sent) {
  DCHECK(IsOnNetworkThread());
  DCHECK(request_headers_sent);
  if (state_ != State::kStarted) {
    return;
  }
  // The capsules sent before the stream was there start waiting now.
  stream_takes_capsules_ = true;
  const base::TimeTicks now = base::TimeTicks::Now();
  for (QueuedCapsule& capsule : capsules_) {
    capsule.since = now;
  }
  WriteCapsules();
}

void DatagramStreamAdapter::OnHeadersReceived(
    const quiche::HttpHeaderBlock& response_headers,
    const net::ProxyInfo& used_proxy_info) {
  DCHECK(IsOnNetworkThread());
  if (state_ != State::kStarted) {
    return;
  }
  if (bidi_stream_->GetProtocol() != net::NextProto::kProtoQUIC) {
    OnFailed(net::ERR_METHOD_NOT_SUPPORTED);
    return;
  }
  int status = 0;
  if (auto it = response_headers.find(":status");
      it != response_headers.end()) {
    base::StringToInt(it->second, &status);
  }
  HeadersArray headers(response_headers);
  if (status != 200) {
    // An answer of the server all the same: the stream ends cleanly.
    Finish();
    ReleaseStreamLater();
    callback_->on_ready(c_stream(), status, &headers);
    callback_->on_closed(c_stream());
    return;
  }

  state_ = State::kReady;
  max_payload_.store(MaxPayloadFor(bidi_stream_->GetMaxDatagramPayload()),
                     std::memory_order_relaxed);
  callback_->on_ready(c_stream(), status, &headers);
  if (const int max_payload = max_payload_.load(std::memory_order_relaxed)) {
    callback_->on_max_payload_changed(c_stream(), max_payload);
  }
  // From a task: the datagrams that came with the headers are delivered
  // after them, and before an end that came with them too.
  read_buffer_ = base::MakeRefCounted<net::IOBufferWithSize>(1);
  PostToNetworkThread(
      FROM_HERE,
      base::BindOnce(&DatagramStreamAdapter::ReadEndOfStream, weak_this_));
}

void DatagramStreamAdapter::OnDataRead(int bytes_read) {
  DCHECK(IsOnNetworkThread());
  if (state_ != State::kReady) {
    return;
  }
  if (bytes_read > 0) {
    ReadEndOfStream();
    return;
  }
  OnEndOfStream(bytes_read);
}

void DatagramStreamAdapter::OnDataSent() {}

void DatagramStreamAdapter::OnTrailersReceived(
    const quiche::HttpHeaderBlock& trailers) {}

void DatagramStreamAdapter::OnFailed(int error) {
  DCHECK(IsOnNetworkThread());
  if (state_ == State::kDone) {
    return;
  }
  Finish();
  ReleaseStreamLater();
  callback_->on_failed(c_stream(), error);
}

void DatagramStreamAdapter::OnHttp3Datagram(std::string_view payload) {
  DCHECK(IsOnNetworkThread());
  if (state_ != State::kReady) {
    return;
  }
  quiche::QuicheDataReader reader(payload);
  uint64_t context_id = 0;
  if (!reader.ReadVarInt62(&context_id) || context_id != kContextId) {
    // Not of this format: dropped (RFC 9298, section 5).
    return;
  }
  const std::string_view data = reader.ReadRemainingPayload();
  callback_->on_datagram_received(c_stream(), data.data(),
                                  static_cast<int>(data.size()));
}

void DatagramStreamAdapter::ReadEndOfStream() {
  if (state_ != State::kReady) {
    return;
  }
  int rv;
  do {
    rv = bidi_stream_->ReadData(read_buffer_.get(), read_buffer_->size());
  } while (rv > 0);
  if (rv == net::ERR_IO_PENDING) {
    return;
  }
  OnEndOfStream(rv);
}

void DatagramStreamAdapter::OnEndOfStream(int rv) {
  if (rv < 0) {
    OnFailed(rv);
    return;
  }
  // The server closed the stream.
  Finish();
  ReleaseStreamLater();
  callback_->on_closed(c_stream());
}

void DatagramStreamAdapter::QueueCapsule(std::string payload) {
  if (writes_closed_ || capsules_.size() >= kMaxQueuedCapsules) {
    dropped_capsules_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  capsules_.push_back({std::move(payload), stream_takes_capsules_
                                               ? base::TimeTicks::Now()
                                               : base::TimeTicks()});
  WriteCapsules();
}

void DatagramStreamAdapter::WriteCapsules() {
  if (stream_takes_capsules_ && state_ != State::kDone && !writes_closed_) {
    const base::TimeTicks now = base::TimeTicks::Now();
    while (!capsules_.empty() &&
           now - capsules_.front().since >= kMaxCapsuleWait) {
      capsules_.pop_front();
      dropped_capsules_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!capsule_in_stream_ && !capsules_.empty()) {
      // The capsules written at once share packets.
      base::ScopedClosureRunner bundle = bidi_stream_->BundleWrites();
      while (!capsule_in_stream_ && !capsules_.empty()) {
        QueuedCapsule capsule = std::move(capsules_.front());
        capsules_.pop_front();
        const int rv = bidi_stream_->WriteDatagramCapsule(
            capsule.payload,
            base::BindOnce(&DatagramStreamAdapter::OnCapsuleWritten,
                           weak_this_));
        if (rv == net::ERR_IO_PENDING) {
          capsule_in_stream_ = true;
          capsule_in_stream_since_ = capsule.since;
        } else if (rv != net::OK) {
          // The write side is closed, or the stream is closing.
          dropped_capsules_.fetch_add(1, std::memory_order_relaxed);
          CloseWrites();
          break;
        }
      }
    }
  }
  // The timer drops the oldest queued capsule when it goes stale.
  if (capsules_.empty()) {
    stale_capsule_timer_.Stop();
    stale_capsule_timer_since_ = base::TimeTicks();
  } else if (capsules_.front().since != stale_capsule_timer_since_ &&
             !capsules_.front().since.is_null()) {
    stale_capsule_timer_since_ = capsules_.front().since;
    stale_capsule_timer_.Start(
        FROM_HERE,
        stale_capsule_timer_since_ + kMaxCapsuleWait - base::TimeTicks::Now(),
        base::BindOnce(&DatagramStreamAdapter::OnStaleCapsuleTimer,
                       base::Unretained(this)));
  }
  UpdateCapsuleQueue();
}

void DatagramStreamAdapter::OnStaleCapsuleTimer() {
  stale_capsule_timer_since_ = base::TimeTicks();
  WriteCapsules();
}

void DatagramStreamAdapter::OnCapsuleWritten(int rv) {
  DCHECK(IsOnNetworkThread());
  capsule_in_stream_ = false;
  if (rv == net::OK) {
    WriteCapsules();
    return;
  }
  if (state_ != State::kDone) {
    CloseWrites();
  }
  UpdateCapsuleQueue();
}

void DatagramStreamAdapter::CloseWrites() {
  writes_closed_ = true;
  open_.store(false, std::memory_order_release);
  dropped_capsules_.fetch_add(capsules_.size(), std::memory_order_relaxed);
  capsules_.clear();
  stale_capsule_timer_.Stop();
  stale_capsule_timer_since_ = base::TimeTicks();
  UpdateCapsuleQueue();
}

void DatagramStreamAdapter::UpdateCapsuleQueue() {
  queued_capsules_.store(capsules_.size(), std::memory_order_relaxed);
  base::TimeTicks head_since;
  if (capsule_in_stream_) {
    head_since = capsule_in_stream_since_;
  } else if (!capsules_.empty()) {
    head_since = capsules_.front().since;
  }
  capsule_head_since_us_.store(
      head_since.is_null() ? 0 : head_since.since_origin().InMicroseconds(),
      std::memory_order_relaxed);
}

// static
int DatagramStreamAdapter::MaxPayloadFor(size_t max_datagram_size) {
  // The Context ID takes one byte of it.
  if (max_datagram_size <= 1) {
    return 0;
  }
  return static_cast<int>(
      std::min(max_datagram_size - 1, static_cast<size_t>(kMaxPayloadLength)));
}

void DatagramStreamAdapter::UpdateMaxPayload(size_t max_datagram_size) {
  const int max_payload = MaxPayloadFor(max_datagram_size);
  if (max_payload_.exchange(max_payload, std::memory_order_relaxed) !=
      max_payload) {
    callback_->on_max_payload_changed(c_stream(), max_payload);
  }
}

void DatagramStreamAdapter::Finish() {
  state_ = State::kDone;
  open_.store(false, std::memory_order_release);
  max_payload_.store(0, std::memory_order_relaxed);
  capsules_.clear();
  capsule_in_stream_ = false;
  stale_capsule_timer_.Stop();
  stale_capsule_timer_since_ = base::TimeTicks();
  UpdateCapsuleQueue();
}

void DatagramStreamAdapter::ReleaseStreamLater() {
  // The stream may be up the stack. It stays owned here: if this goes first,
  // its destructor destroys the stream, from a task of its own.
  PostToNetworkThread(
      FROM_HERE,
      base::BindOnce(&DatagramStreamAdapter::DropStream, weak_this_));
}

void DatagramStreamAdapter::DropStream() {
  DCHECK(IsOnNetworkThread());
  bidi_stream_.reset();
}

bool DatagramStreamAdapter::IsOnNetworkThread() const {
  return request_context_getter_->GetNetworkTaskRunner()
      ->BelongsToCurrentThread();
}

void DatagramStreamAdapter::PostToNetworkThread(const base::Location& from_here,
                                                base::OnceClosure task) {
  request_context_getter_->GetNetworkTaskRunner()->PostTask(from_here,
                                                            std::move(task));
}

}  // namespace

datagram_stream* datagram_stream_create(
    stream_engine* engine,
    void* annotation,
    const datagram_stream_callback* callback) {
  // Owned by the caller until datagram_stream_destroy().
  auto* adapter = new DatagramStreamAdapter(engine, annotation, callback);
  return adapter->c_stream();
}

int datagram_stream_start(datagram_stream* stream,
                          const char* url,
                          const bidirectional_stream_header_array* headers) {
  return DatagramStreamAdapter::FromCStream(stream)->Start(url, headers);
}

int datagram_stream_send(datagram_stream* stream,
                         const char* data,
                         int length,
                         int flags) {
  return DatagramStreamAdapter::FromCStream(stream)->Send(data, length, flags);
}

int datagram_stream_max_payload(datagram_stream* stream) {
  return DatagramStreamAdapter::FromCStream(stream)->max_payload();
}

uint64_t datagram_stream_dropped_capsules(datagram_stream* stream) {
  return DatagramStreamAdapter::FromCStream(stream)->dropped_capsules();
}

void datagram_stream_cancel(datagram_stream* stream) {
  DatagramStreamAdapter::FromCStream(stream)->Cancel();
}

int datagram_stream_destroy(datagram_stream* stream) {
  DatagramStreamAdapter::FromCStream(stream)->Destroy();
  return 0;
}
