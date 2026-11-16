// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/cronet/native/exact_server_cert_verifier.h"

#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/task/single_thread_task_runner.h"
#include "net/base/net_errors.h"
#include "net/cert/asn1_util.h"
#include "net/cert/cert_status_flags.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "net/log/net_log_with_source.h"

namespace cronet {

ExactServerCertVerifier::TrustedCertificate::TrustedCertificate(
    scoped_refptr<net::X509Certificate> certificate,
    const net::SHA256HashValue& public_key_hash)
    : certificate(std::move(certificate)), public_key_hash(public_key_hash) {}

ExactServerCertVerifier::TrustedCertificate::TrustedCertificate(
    const TrustedCertificate& other) = default;

ExactServerCertVerifier::TrustedCertificate::TrustedCertificate(
    TrustedCertificate&& other) = default;

ExactServerCertVerifier::TrustedCertificate&
ExactServerCertVerifier::TrustedCertificate::operator=(
    const TrustedCertificate& other) = default;

ExactServerCertVerifier::TrustedCertificate&
ExactServerCertVerifier::TrustedCertificate::operator=(
    TrustedCertificate&& other) = default;

ExactServerCertVerifier::TrustedCertificate::~TrustedCertificate() = default;

// static
std::unique_ptr<ExactServerCertVerifier> ExactServerCertVerifier::Create(
    const std::vector<std::string>& der_certificates) {
  if (der_certificates.empty()) {
    return nullptr;
  }
  std::vector<std::pair<std::string, TrustedCertificate>> entries;
  entries.reserve(der_certificates.size());
  for (const std::string& der : der_certificates) {
    scoped_refptr<net::X509Certificate> certificate =
        net::X509Certificate::CreateFromBytes(base::as_byte_span(der));
    if (!certificate) {
      return nullptr;
    }
    // Checked here, so that hashing the key cannot fail later. Of a
    // certificate that X509Certificate parses, it does not.
    std::string_view spki;
    if (!net::asn1::ExtractSPKIFromDERCert(der, &spki)) {
      return nullptr;
    }
    const net::SHA256HashValue public_key_hash =
        net::x509_util::CalculateSha256SpkiHash(certificate->cert_buffer());
    entries.emplace_back(der, TrustedCertificate(std::move(certificate),
                                                 public_key_hash));
  }
  // A certificate given twice is kept once.
  return base::WrapUnique(
      new ExactServerCertVerifier(TrustedCertificates(std::move(entries))));
}

ExactServerCertVerifier::ExactServerCertVerifier(
    TrustedCertificates trusted_certificates)
    : trusted_certificates_(std::move(trusted_certificates)) {}

ExactServerCertVerifier::~ExactServerCertVerifier() = default;

int ExactServerCertVerifier::Verify(const RequestParams& params,
                                    net::CertVerifyResult* verify_result,
                                    net::CompletionOnceCallback callback,
                                    std::unique_ptr<Request>* out_req,
                                    const net::NetLogWithSource& net_log) {
  verify_result->Reset();
  const scoped_refptr<net::X509Certificate>& presented = params.certificate();
  const auto trusted = trusted_certificates_.find(
      net::x509_util::CryptoBufferAsStringPiece(presented->cert_buffer()));
  if (trusted == trusted_certificates_.end()) {
    verify_result->verified_cert = presented;
    verify_result->cert_status = net::CERT_STATUS_AUTHORITY_INVALID;
    return net::ERR_CERT_AUTHORITY_INVALID;
  }
  verify_result->verified_cert = trusted->second.certificate;
  verify_result->public_key_hashes.push_back(trusted->second.public_key_hash);
  // Reset() leaves cert_status 0 and is_issued_by_known_root false, and CT
  // policy compliance not evaluated, which no requirement asks for when the
  // root is not a known one.
  return net::OK;
}

void ExactServerCertVerifier::Verify2QwacBinding(
    const std::string& binding,
    const std::string& hostname,
    const scoped_refptr<net::X509Certificate>& tls_cert,
    base::OnceCallback<void(const scoped_refptr<net::X509Certificate>&)>
        callback,
    const net::NetLogWithSource& net_log) {
  // No 2-QWAC is verified: that needs the trust of a root store.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), nullptr));
}

}  // namespace cronet
