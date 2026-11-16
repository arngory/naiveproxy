// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_CRONET_NATIVE_EXACT_SERVER_CERT_VERIFIER_H_
#define COMPONENTS_CRONET_NATIVE_EXACT_SERVER_CERT_VERIFIER_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/memory/scoped_refptr.h"
#include "net/base/hash_value.h"
#include "net/cert/cert_verifier.h"

namespace net {
class X509Certificate;
}  // namespace net

namespace cronet {

// The certificate verifier of an engine whose EngineParams name exact server
// certificates (Cronet_EngineParams_exact_server_certificates_add()).
//
// It accepts a server certificate if and only if the leaf the server presents
// is, byte for byte, the DER of one of those certificates. That comparison is
// the whole decision: no chain is built, and neither the host name or IP
// address, nor the validity period, key usage, the certificate's own
// signature, revocation, a root store or Certificate Transparency is looked
// at. (The TLS handshake still proves that the server holds the private key of
// the certificate it presents.) It is meant for servers that are reached by
// address with a self-signed certificate that the embedder distributes itself.
//
// A match verifies with cert_status 0; |verified_cert| is the leaf alone, its
// own trust anchor (intermediates the server sent play no part, so they are
// not in the verified chain); |public_key_hashes| holds the SHA-256 hash of the
// leaf's SubjectPublicKeyInfo; and |is_issued_by_known_root| is false, as for a
// locally trusted root. Being no known root, it needs no Certificate
// Transparency (RequireCTDelegate::CheckCTRequirements) and is not refused by
// the known-root check of QUIC as long as QuicParams::allow_unknown_cert_roots
// is set, which Cronet does for every verifier that replaces its own.
//
// Any other certificate fails with ERR_CERT_AUTHORITY_INVALID, which is what
// Chromium reports for a certificate that leads to no trusted anchor: the set
// is the only anchor there is. The certificate is not malformed, which
// ERR_CERT_INVALID would say.
//
// Verification is synchronous, so the verifier needs no caching.
class ExactServerCertVerifier : public net::CertVerifier {
 public:
  // Returns a verifier that trusts exactly |der_certificates|, or null if
  // |der_certificates| is empty or one of its entries is not a DER X.509
  // certificate that net::X509Certificate parses.
  static std::unique_ptr<ExactServerCertVerifier> Create(
      const std::vector<std::string>& der_certificates);

  ExactServerCertVerifier(const ExactServerCertVerifier&) = delete;
  ExactServerCertVerifier& operator=(const ExactServerCertVerifier&) = delete;

  ~ExactServerCertVerifier() override;

  // net::CertVerifier implementation:
  int Verify(const RequestParams& params,
             net::CertVerifyResult* verify_result,
             net::CompletionOnceCallback callback,
             std::unique_ptr<Request>* out_req,
             const net::NetLogWithSource& net_log) override;
  void Verify2QwacBinding(
      const std::string& binding,
      const std::string& hostname,
      const scoped_refptr<net::X509Certificate>& tls_cert,
      base::OnceCallback<void(const scoped_refptr<net::X509Certificate>&)>
          callback,
      const net::NetLogWithSource& net_log) override;
  // Nothing to configure: the set is fixed and revocation is never checked.
  void SetConfig(const Config& config) override {}
  // The set never changes, so observers are never notified.
  void AddObserver(Observer* observer) override {}
  void RemoveObserver(Observer* observer) override {}

 private:
  // A certificate of the set, as a successful verification reports it.
  struct TrustedCertificate {
    TrustedCertificate(scoped_refptr<net::X509Certificate> certificate,
                       const net::SHA256HashValue& public_key_hash);
    TrustedCertificate(const TrustedCertificate& other);
    TrustedCertificate(TrustedCertificate&& other);
    TrustedCertificate& operator=(const TrustedCertificate& other);
    TrustedCertificate& operator=(TrustedCertificate&& other);
    ~TrustedCertificate();

    // The certificate alone, without intermediates.
    scoped_refptr<net::X509Certificate> certificate;
    // SHA-256 of its SubjectPublicKeyInfo.
    net::SHA256HashValue public_key_hash;
  };

  using TrustedCertificates =
      base::flat_map<std::string, TrustedCertificate, std::less<>>;

  explicit ExactServerCertVerifier(TrustedCertificates trusted_certificates);

  // Keyed by DER.
  const TrustedCertificates trusted_certificates_;
};

}  // namespace cronet

#endif  // COMPONENTS_CRONET_NATIVE_EXACT_SERVER_CERT_VERIFIER_H_
