// Copyright 2026 klzgrad <kizdiv@gmail.com>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/ssl/ssl_config_service_trust_anchor_ids.h"

#include <stdint.h>

#include <utility>
#include <vector>

#include "base/containers/extend.h"
#include "base/feature_list.h"
#include "net/base/features.h"
#include "net/cert/x509_util.h"
#include "net/net_buildflags.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
#include "net/cert/internal/trust_store_chrome.h"
#endif

namespace net {

SSLConfigServiceTrustAnchorIDs::SSLConfigServiceTrustAnchorIDs(
    bool no_post_quantum,
    std::unique_ptr<EchModeGetter> ech_mode_getter)
    : ech_mode_getter_(std::move(ech_mode_getter)) {
#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
  // Copied from chrome/browser/ssl/ssl_config_service_manager.cc
  if (base::FeatureList::IsEnabled(features::kTLSTrustAnchorIDs)) {
    std::vector<std::vector<uint8_t>> trust_anchor_ids;
    if (base::FeatureList::IsEnabled(features::kNonMtcTrustAnchorIDs)) {
      base::Extend(trust_anchor_ids,
                   TrustStoreChrome::GetTrustAnchorIDsFromCompiledInRootStore());
    }
    if (base::FeatureList::IsEnabled(features::kVerifyMTCs)) {
      base::Extend(
          trust_anchor_ids,
          TrustStoreChrome::GetTrustedMtcCaIDsFromCompiledInRootStore());
    }
    config_.trust_anchor_ids = x509_util::EncodeTlsRequestedTrustAnchorIDList(
        std::move(trust_anchor_ids));
  }
#endif
  if (no_post_quantum) {
    std::erase_if(config_.supported_named_groups,
                  [](const SSLNamedGroupInfo& g) {
                    return g.group_id == SSL_GROUP_X25519_MLKEM768 ||
                           g.group_id == SSL_GROUP_MLKEM1024;
                  });
  }
}

SSLConfigServiceTrustAnchorIDs::~SSLConfigServiceTrustAnchorIDs() = default;

SSLContextConfig SSLConfigServiceTrustAnchorIDs::GetSSLContextConfig() {
  return config_;
}

EchMode SSLConfigServiceTrustAnchorIDs::GetEchMode(
    std::string_view hostname) const {
  if (ech_mode_getter_) {
    return ech_mode_getter_->GetEchMode(hostname);
  }
  return EchMode::kOpportunistic;
}

bool SSLConfigServiceTrustAnchorIDs::CanShareConnectionWithClientCerts(
    std::string_view hostname) const {
  return false;
}

}  // namespace net
