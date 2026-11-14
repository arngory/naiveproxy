// Copyright 2026 klzgrad <kizdiv@gmail.com>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SSL_SSL_CONFIG_SERVICE_TRUST_ANCHOR_IDS_H_
#define NET_SSL_SSL_CONFIG_SERVICE_TRUST_ANCHOR_IDS_H_

#include <memory>
#include <string_view>

#include "net/base/net_export.h"
#include "net/ssl/ech_mode_getter.h"
#include "net/ssl/ssl_config_service.h"

namespace net {

// An SSLConfigService for embedders outside of Chrome that want the same
// ClientHello as Chrome. It is SSLConfigServiceDefaults plus the TLS Trust
// Anchor IDs of the compiled-in Chrome Root Store, which Chrome's
// SSLConfigServiceManager puts into the SSLContextConfig at startup
// (chrome/browser/ssl/ssl_config_service_manager.cc). Without them the
// trust_anchors extension is not sent at all.
class NET_EXPORT SSLConfigServiceTrustAnchorIDs : public SSLConfigService {
 public:
  // If `no_post_quantum` is true, the post-quantum key agreement groups are
  // removed from the supported groups.
  // If `ech_mode_getter` is provided, it will be used to query the ECH policy.
  // Otherwise, GetEchMode will default to kOpportunistic, as Chrome does
  // unless Encrypted ClientHello is turned off by policy.
  explicit SSLConfigServiceTrustAnchorIDs(
      bool no_post_quantum = false,
      std::unique_ptr<EchModeGetter> ech_mode_getter = nullptr);

  SSLConfigServiceTrustAnchorIDs(const SSLConfigServiceTrustAnchorIDs&) =
      delete;
  SSLConfigServiceTrustAnchorIDs& operator=(
      const SSLConfigServiceTrustAnchorIDs&) = delete;

  ~SSLConfigServiceTrustAnchorIDs() override;

  // SSLConfigService implementation:
  SSLContextConfig GetSSLContextConfig() override;
  EchMode GetEchMode(std::string_view hostname) const override;
  bool CanShareConnectionWithClientCerts(
      std::string_view hostname) const override;

 private:
  SSLContextConfig config_;

  std::unique_ptr<EchModeGetter> ech_mode_getter_;
};

}  // namespace net

#endif  // NET_SSL_SSL_CONFIG_SERVICE_TRUST_ANCHOR_IDS_H_
