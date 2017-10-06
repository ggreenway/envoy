#include "common/ssl/context_impl.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/exception.h"
#include "envoy/runtime/runtime.h"

#include "common/common/assert.h"
#include "common/common/hex.h"

#include "fmt/format.h"
#include "openssl/rand.h"
#include "openssl/x509v3.h"

namespace Envoy {
namespace Ssl {

int ContextImpl::sslContextIndex() {
  CONSTRUCT_ON_FIRST_USE(int, []() -> int {
    int ssl_context_index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    RELEASE_ASSERT(ssl_context_index >= 0);
    return ssl_context_index;
  }());
}

ContextImpl::ContextImpl(ContextManagerImpl& parent, Stats::Scope& scope, ContextConfig& config)
    : parent_(parent), ctx_(SSL_CTX_new(TLS_method())), scope_(scope),
      stats_(generateStats(scope)) {
  RELEASE_ASSERT(ctx_);

  int rc = SSL_CTX_set_ex_data(ctx_.get(), sslContextIndex(), this);
  RELEASE_ASSERT(rc == 1);

  if (!SSL_CTX_set_strict_cipher_list(ctx_.get(), config.cipherSuites().c_str())) {
    throw EnvoyException(
        fmt::format("Failed to initialize cipher suites {}", config.cipherSuites()));
  }

  if (!SSL_CTX_set1_curves_list(ctx_.get(), config.ecdhCurves().c_str())) {
    throw EnvoyException(fmt::format("Failed to initialize ECDH curves {}", config.ecdhCurves()));
  }

  int verify_mode = SSL_VERIFY_NONE;

  if (!config.caCertFile().empty()) {
    ca_cert_ = loadCert(config.caCertFile());
    ca_file_path_ = config.caCertFile();
    // set CA certificate
    int rc = SSL_CTX_load_verify_locations(ctx_.get(), config.caCertFile().c_str(), nullptr);
    if (0 == rc) {
      throw EnvoyException(
          fmt::format("Failed to load verify locations file {}", config.caCertFile()));
    }
    verify_mode = SSL_VERIFY_PEER;
  }

  if (!config.verifySubjectAltNameList().empty()) {
    verify_subject_alt_name_list_ = config.verifySubjectAltNameList();
    verify_mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }

  if (!config.verifyCertificateHash().empty()) {
    std::string hash = config.verifyCertificateHash();
    // remove ':' delimiters from hex string
    hash.erase(std::remove(hash.begin(), hash.end(), ':'), hash.end());
    verify_certificate_hash_ = Hex::decode(hash);
    verify_mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }

  if (verify_mode != SSL_VERIFY_NONE) {
    SSL_CTX_set_verify(ctx_.get(), verify_mode, nullptr);
    SSL_CTX_set_cert_verify_callback(ctx_.get(), ContextImpl::verifyCallback, this);
  }

  if (!config.certChainFile().empty()) {
    cert_chain_ = loadCert(config.certChainFile());
    cert_chain_file_path_ = config.certChainFile();
    int rc = SSL_CTX_use_certificate_chain_file(ctx_.get(), config.certChainFile().c_str());
    if (0 == rc) {
      throw EnvoyException(
          fmt::format("Failed to load certificate chain file {}", config.certChainFile()));
    }

    rc = SSL_CTX_use_PrivateKey_file(ctx_.get(), config.privateKeyFile().c_str(), SSL_FILETYPE_PEM);
    if (0 == rc) {
      throw EnvoyException(
          fmt::format("Failed to load private key file {}", config.privateKeyFile()));
    }
  }

  SSL_CTX_set_options(ctx_.get(), SSL_OP_NO_SSLv3);

  // use the server's cipher list preferences
  SSL_CTX_set_options(ctx_.get(), SSL_OP_CIPHER_SERVER_PREFERENCE);

  parsed_alpn_protocols_ = parseAlpnProtocols(config.alpnProtocols());
}

int ServerContextImpl::alpnSelectCallback(const unsigned char** out, unsigned char* outlen,
                                          const unsigned char* in, unsigned int inlen) {
  // Currently this uses the standard selection algorithm in priority order.
  const uint8_t* alpn_data = &parsed_alpn_protocols_[0];
  size_t alpn_data_size = parsed_alpn_protocols_.size();
  if (!parsed_alt_alpn_protocols_.empty() &&
      runtime_.snapshot().featureEnabled("ssl.alt_alpn", 0)) {
    alpn_data = &parsed_alt_alpn_protocols_[0];
    alpn_data_size = parsed_alt_alpn_protocols_.size();
  }

  if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, alpn_data, alpn_data_size, in,
                            inlen) != OPENSSL_NPN_NEGOTIATED) {
    return SSL_TLSEXT_ERR_NOACK;
  } else {
    return SSL_TLSEXT_ERR_OK;
  }
}

std::vector<uint8_t> ContextImpl::parseAlpnProtocols(const std::string& alpn_protocols) {
  if (alpn_protocols.empty()) {
    return {};
  }

  if (alpn_protocols.size() >= 65535) {
    throw EnvoyException("invalid ALPN protocol string");
  }

  std::vector<uint8_t> out(alpn_protocols.size() + 1);
  size_t start = 0;
  for (size_t i = 0; i <= alpn_protocols.size(); i++) {
    if (i == alpn_protocols.size() || alpn_protocols[i] == ',') {
      if (i - start > 255) {
        throw EnvoyException("invalid ALPN protocol string");
      }

      out[start] = i - start;
      start = i + 1;
    } else {
      out[i + 1] = alpn_protocols[i];
    }
  }

  return out;
}

bssl::UniquePtr<SSL> ContextImpl::newSsl() const {
  return bssl::UniquePtr<SSL>(SSL_new(ctx_.get()));
}

int ContextImpl::verifyCallback(X509_STORE_CTX* store_ctx, void* arg) {
  ContextImpl* impl = reinterpret_cast<ContextImpl*>(arg);

  int ret = X509_verify_cert(store_ctx);
  if (ret <= 0) {
    impl->stats_.fail_verify_error_.inc();
    return ret;
  }

  SSL* ssl = reinterpret_cast<SSL*>(
      X509_STORE_CTX_get_ex_data(store_ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
  bssl::UniquePtr<X509> cert(SSL_get_peer_certificate(ssl));
  return impl->verifyCertificate(cert.get());
}

int ContextImpl::verifyCertificate(X509* cert) {
  if (!verify_subject_alt_name_list_.empty() &&
      !verifySubjectAltName(cert, verify_subject_alt_name_list_)) {
    stats_.fail_verify_san_.inc();
    return 0;
  }

  if (!verify_certificate_hash_.empty() && !verifyCertificateHash(cert, verify_certificate_hash_)) {
    stats_.fail_verify_cert_hash_.inc();
    return 0;
  }

  return 1;
}

void ContextImpl::logHandshake(SSL* ssl) const {
  stats_.handshake_.inc();

  if (SSL_session_reused(ssl)) {
    stats_.session_reused_.inc();
  }

  const char* cipher = SSL_get_cipher_name(ssl);
  scope_.counter(fmt::format("ssl.ciphers.{}", std::string{cipher})).inc();

  bssl::UniquePtr<X509> cert(SSL_get_peer_certificate(ssl));
  if (!cert.get()) {
    stats_.no_certificate_.inc();
  }
}

bool ContextImpl::verifySubjectAltName(X509* cert,
                                       const std::vector<std::string>& subject_alt_names) {
  bool verified = false;

  STACK_OF(GENERAL_NAME)* altnames = static_cast<STACK_OF(GENERAL_NAME)*>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));

  if (altnames) {
    int n = sk_GENERAL_NAME_num(altnames);
    for (int i = 0; i < n && !verified; i++) {
      GENERAL_NAME* altname = sk_GENERAL_NAME_value(altnames, i);

      if (altname->type == GEN_DNS) {
        ASN1_STRING* str = altname->d.dNSName;
        char* dns_name = reinterpret_cast<char*>(ASN1_STRING_data(str));
        for (auto& config_san : subject_alt_names) {
          if (dNSNameMatch(config_san, dns_name)) {
            verified = true;
            break;
          }
        }
      } else if (altname->type == GEN_URI) {
        ASN1_STRING* str = altname->d.uniformResourceIdentifier;
        char* crt_san = reinterpret_cast<char*>(ASN1_STRING_data(str));
        for (auto& config_san : subject_alt_names) {
          if (config_san.compare(crt_san) == 0) {
            verified = true;
            break;
          }
        }
      }
    }

    sk_GENERAL_NAME_pop_free(altnames, GENERAL_NAME_free);
  }

  return verified;
}

bool ContextImpl::dNSNameMatch(const std::string& dNSName, const char* pattern) {
  if (dNSName == pattern) {
    return true;
  }

  size_t pattern_len = strlen(pattern);
  if (pattern_len > 1 && pattern[0] == '*' && pattern[1] == '.') {
    if (dNSName.length() > pattern_len - 1) {
      size_t off = dNSName.length() - pattern_len + 1;
      return dNSName.compare(off, pattern_len - 1, pattern + 1) == 0;
    }
  }

  return false;
}

bool ContextImpl::verifyCertificateHash(X509* cert, const std::vector<uint8_t>& expected_hash) {
  std::vector<uint8_t> computed_hash(SHA256_DIGEST_LENGTH);
  unsigned int n;
  X509_digest(cert, EVP_sha256(), computed_hash.data(), &n);
  RELEASE_ASSERT(n == computed_hash.size());

  return computed_hash == expected_hash;
}

SslStats ContextImpl::generateStats(Stats::Scope& store) {
  std::string prefix("ssl.");
  return {ALL_SSL_STATS(POOL_COUNTER_PREFIX(store, prefix), POOL_GAUGE_PREFIX(store, prefix),
                        POOL_HISTOGRAM_PREFIX(store, prefix))};
}

size_t ContextImpl::daysUntilFirstCertExpires() {
  int daysUntilExpiration = getDaysUntilExpiration(ca_cert_.get());
  daysUntilExpiration =
      std::min<int>(getDaysUntilExpiration(cert_chain_.get()), daysUntilExpiration);
  if (daysUntilExpiration < 0) { // Ensure that the return value is unsigned
    return 0;
  }
  return daysUntilExpiration;
}

int32_t ContextImpl::getDaysUntilExpiration(const X509* cert) {
  if (cert == nullptr) {
    return std::numeric_limits<int>::max();
  }
  int days, seconds;
  if (ASN1_TIME_diff(&days, &seconds, nullptr, X509_get_notAfter(cert))) {
    return days;
  }
  return 0;
}

std::string ContextImpl::getCaCertInformation() {
  if (ca_cert_ == nullptr) {
    return "";
  }
  return fmt::format("Certificate Path: {}, Serial Number: {}, Days until Expiration: {}",
                     getCaFileName(), getSerialNumber(ca_cert_.get()),
                     getDaysUntilExpiration(ca_cert_.get()));
}

std::string ContextImpl::getCertChainInformation() {
  if (cert_chain_ == nullptr) {
    return "";
  }
  return fmt::format("Certificate Path: {}, Serial Number: {}, Days until Expiration: {}",
                     getCertChainFileName(), getSerialNumber(cert_chain_.get()),
                     getDaysUntilExpiration(cert_chain_.get()));
}

std::string ContextImpl::getSerialNumber(X509* cert) {
  ASSERT(cert);
  ASN1_INTEGER* serial_number = X509_get_serialNumber(cert);
  BIGNUM num_bn;
  BN_init(&num_bn);
  ASN1_INTEGER_to_BN(serial_number, &num_bn);
  char* char_serial_number = BN_bn2hex(&num_bn);
  BN_free(&num_bn);
  if (char_serial_number != nullptr) {
    std::string serial_number(char_serial_number);
    OPENSSL_free(char_serial_number);
    return serial_number;
  }
  return "";
}

bssl::UniquePtr<X509> ContextImpl::loadCert(const std::string& cert_file) {
  X509* cert = nullptr;
  std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(cert_file.c_str(), "r"), &fclose);
  if (!fp.get() || !PEM_read_X509(fp.get(), &cert, nullptr, nullptr)) {
    throw EnvoyException(fmt::format("Failed to load certificate '{}'", cert_file.c_str()));
  }
  return bssl::UniquePtr<X509>(cert);
};

ClientContextImpl::ClientContextImpl(ContextManagerImpl& parent, Stats::Scope& scope,
                                     ClientContextConfig& config)
    : ContextImpl(parent, scope, config) {
  if (!parsed_alpn_protocols_.empty()) {
    int rc = SSL_CTX_set_alpn_protos(ctx_.get(), &parsed_alpn_protocols_[0],
                                     parsed_alpn_protocols_.size());
    RELEASE_ASSERT(rc == 0);
    UNREFERENCED_PARAMETER(rc);
  }

  server_name_indication_ = config.serverNameIndication();
}

bssl::UniquePtr<SSL> ClientContextImpl::newSsl() const {
  bssl::UniquePtr<SSL> ssl_con(ContextImpl::newSsl());

  if (!server_name_indication_.empty()) {
    int rc = SSL_set_tlsext_host_name(ssl_con.get(), server_name_indication_.c_str());
    RELEASE_ASSERT(rc);
    UNREFERENCED_PARAMETER(rc);
  }

  return ssl_con;
}

ServerContextImpl::ServerContextImpl(ContextManagerImpl& parent, Stats::Scope& scope,
                                     ServerContextConfig& config, Runtime::Loader& runtime)
    : ContextImpl(parent, scope, config), runtime_(runtime) {
  if (!config.caCertFile().empty()) {
    bssl::UniquePtr<STACK_OF(X509_NAME)> list(SSL_load_client_CA_file(config.caCertFile().c_str()));
    if (nullptr == list) {
      throw EnvoyException(fmt::format("Failed to load client CA file {}", config.caCertFile()));
    }
    SSL_CTX_set_client_CA_list(ctx_.get(), list.release());

    // SSL_VERIFY_PEER or stronger mode was already set in ContextImpl::ContextImpl().
    if (config.requireClientCertificate()) {
      SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }
  }

  parsed_alt_alpn_protocols_ = parseAlpnProtocols(config.altAlpnProtocols());

  if (!parsed_alpn_protocols_.empty()) {
    SSL_CTX_set_alpn_select_cb(ctx_.get(),
                               [](SSL*, const unsigned char** out, unsigned char* outlen,
                                  const unsigned char* in, unsigned int inlen, void* arg) -> int {
                                 return static_cast<ServerContextImpl*>(arg)->alpnSelectCallback(
                                     out, outlen, in, inlen);
                               },
                               this);
  }

  if (!config.sessionTicketKeys().empty()) {
    session_ticket_keys_.resize(config.sessionTicketKeys().size());
    for (unsigned i = 0; i < config.sessionTicketKeys().size(); ++i) {
      // If this changes, need to figure out how to deal with key files
      // that previously worked.  For now, just assert so we'll notice that
      // it changed if it does.
      static_assert(sizeof(SessionTicketKey) == 80, "Input is expected to be this size");

      const std::vector<uint8_t>& src_key = config.sessionTicketKeys().at(i);
      SessionTicketKey& dst_key = session_ticket_keys_.at(i);

      if (src_key.size() != sizeof(SessionTicketKey)) {
        throw EnvoyException(fmt::format("Incorrect TLS session ticket key length.  "
                                         "Index {}, length {}, expected length {}.",
                                         i, src_key.size(), sizeof(SessionTicketKey)));
      }

      std::copy_n(src_key.begin(), dst_key.name.size(), dst_key.name.begin());
      size_t pos = dst_key.name.size();
      std::copy_n(src_key.begin() + pos, dst_key.hmac_key.size(), dst_key.hmac_key.begin());
      pos += dst_key.hmac_key.size();
      std::copy_n(src_key.begin() + pos, dst_key.aes_key.size(), dst_key.aes_key.begin());
      pos += dst_key.aes_key.size();
      ASSERT(src_key.begin() + pos == src_key.end());
    }
    SSL_CTX_set_tlsext_ticket_key_cb(
        ctx_.get(),
        [](SSL* ssl, uint8_t* key_name, uint8_t* iv, EVP_CIPHER_CTX* ctx, HMAC_CTX* hmac_ctx,
           int encrypt) -> int {
          ContextImpl* context_impl = static_cast<ContextImpl*>(
              SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), sslContextIndex()));
          return dynamic_cast<ServerContextImpl*>(context_impl)
              ->sessionTicketProcess(ssl, key_name, iv, ctx, hmac_ctx, encrypt);
        });
  }

  // Hash all the settings that affect whether the server will allow/accept
  // the client connection.  This ensures that the client is always validated against
  // the correct settings, even if session resumption across different listeners
  // is enabled.
  uint8_t session_context_buf[EVP_MAX_MD_SIZE] = {};
  unsigned session_context_len = 0;
  EVP_MD_CTX md;
  int rc = EVP_DigestInit(&md, EVP_sha256());
  RELEASE_ASSERT(rc == 1);

  // Include "envoy" so that if all the other verify-settings are unset
  // we have a deterministic value.
  const char* initial_context = "envoy";
  rc = EVP_DigestUpdate(&md, initial_context, strlen(initial_context));
  RELEASE_ASSERT(rc == 1);

  if (ca_cert_ != nullptr) {
    rc = X509_digest(ca_cert_.get(), EVP_sha256(), session_context_buf, &session_context_len);
    RELEASE_ASSERT(rc == 1 && session_context_len == SHA256_DIGEST_LENGTH);
    rc = EVP_DigestUpdate(&md, session_context_buf, session_context_len);
    RELEASE_ASSERT(rc == 1);

    // verify_subject_alt_name_list_ can only be set with a ca_cert
    for (const std::string& name : verify_subject_alt_name_list_) {
      rc = EVP_DigestUpdate(&md, name.data(), name.size());
      RELEASE_ASSERT(rc == 1);
    }

    // verify_certificate_hash_ can only be set with a ca_cert
    rc = EVP_DigestUpdate(&md, verify_certificate_hash_.data(),
                          verify_certificate_hash_.size() *
                              sizeof(decltype(verify_certificate_hash_)::value_type));
    RELEASE_ASSERT(rc == 1);
  }

  rc = EVP_DigestFinal(&md, session_context_buf, &session_context_len);
  RELEASE_ASSERT(rc == 1);
  rc = SSL_CTX_set_session_id_context(ctx_.get(), session_context_buf, session_context_len);
  RELEASE_ASSERT(rc == 1);
}

int ServerContextImpl::sessionTicketProcess(SSL*, uint8_t* key_name, uint8_t* iv,
                                            EVP_CIPHER_CTX* ctx, HMAC_CTX* hmac_ctx, int encrypt) {
  const EVP_MD* hmac = EVP_sha256();
  const EVP_CIPHER* cipher = EVP_aes_256_cbc();

  if (encrypt == 1) {
    // Encrypt
    if (session_ticket_keys_.empty()) {
      return -1;
    }

    const SessionTicketKey& key = session_ticket_keys_.front();

    static_assert(std::tuple_size<decltype(key.name)>::value == SSL_TICKET_KEY_NAME_LEN,
                  "Expected key.name length");
    std::copy_n(key.name.begin(), SSL_TICKET_KEY_NAME_LEN, key_name);

    int rc = RAND_bytes(iv, EVP_CIPHER_iv_length(cipher));
    ASSERT(rc);
    UNREFERENCED_PARAMETER(rc);

    // This RELEASE_ASSERT is logically a static_assert, but we can't actually get
    // EVP_CIPHER_key_length(cipher) at compile-time
    RELEASE_ASSERT(key.aes_key.size() == EVP_CIPHER_key_length(cipher));
    if (!EVP_EncryptInit_ex(ctx, cipher, nullptr, key.aes_key.data(), iv)) {
      return -1;
    }

    if (!HMAC_Init_ex(hmac_ctx, key.hmac_key.data(), key.hmac_key.size(), hmac, nullptr)) {
      return -1;
    }

    return 1; // success
  } else {
    // Decrypt
    bool is_enc_key = true; // first element is the encryption key
    for (const SessionTicketKey& key : session_ticket_keys_) {
      static_assert(std::tuple_size<decltype(key.name)>::value == SSL_TICKET_KEY_NAME_LEN,
                    "Expected key.name length");
      if (std::equal(key.name.begin(), key.name.end(), key_name)) {
        if (!HMAC_Init_ex(hmac_ctx, key.hmac_key.data(), key.hmac_key.size(), hmac, nullptr)) {
          return -1;
        }

        RELEASE_ASSERT(key.aes_key.size() == EVP_CIPHER_key_length(cipher));
        if (!EVP_DecryptInit_ex(ctx, cipher, nullptr, key.aes_key.data(), iv)) {
          return -1;
        }

        // If our current encryption was not the decryption key, renew
        return is_enc_key ? 1  // success; do not renew
                          : 2; // success: renew key
      }
      is_enc_key = false;
    }

    return 0; // decryption failed
  }
}

} // namespace Ssl
} // namespace Envoy
