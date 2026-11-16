// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/bidirectional_stream_impl.h"

#include "net/base/net_errors.h"

namespace net {

BidirectionalStreamImpl::Delegate::Delegate() = default;

BidirectionalStreamImpl::Delegate::~Delegate() = default;

void BidirectionalStreamImpl::Delegate::OnHttp3Datagram(
    std::string_view payload) {}

BidirectionalStreamImpl::BidirectionalStreamImpl() = default;

BidirectionalStreamImpl::~BidirectionalStreamImpl() = default;

int BidirectionalStreamImpl::SendHttp3Datagram(std::string_view payload) {
  return ERR_NOT_IMPLEMENTED;
}

int BidirectionalStreamImpl::WriteDatagramCapsule(
    std::string_view payload,
    CompletionOnceCallback callback) {
  return ERR_NOT_IMPLEMENTED;
}

size_t BidirectionalStreamImpl::GetMaxDatagramPayload() const {
  return 0;
}

}  // namespace net
