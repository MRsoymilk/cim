#include "config/ServerOptions.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <iostream>
#include <memory>
#include <string>

namespace cim {
namespace {

std::string LastOpenSslError() {
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return "unknown OpenSSL error";
    }
    char message[256];
    ERR_error_string_n(code, message, sizeof(message));
    return message;
}

} // namespace

bool ParseServerOptions(int argc, char** argv, ServerOptions& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "--tls-cert" || argument == "--tls-key") && index + 1 >= argc) {
            std::cerr << argument << " requires a file path" << std::endl;
            return false;
        }
        if (argument == "--tls-cert") {
            if (!options.certificate.empty()) {
                std::cerr << "--tls-cert may only be specified once" << std::endl;
                return false;
            }
            options.certificate = argv[++index];
        } else if (argument == "--tls-key") {
            if (!options.private_key.empty()) {
                std::cerr << "--tls-key may only be specified once" << std::endl;
                return false;
            }
            options.private_key = argv[++index];
        } else {
            std::cerr << "Unknown option: " << argument << std::endl;
            return false;
        }
    }

    if (options.certificate.empty() || options.private_key.empty()) {
        std::cerr << "TLS is required. Start the server with --tls-cert <PEM> "
                     "--tls-key <PEM>."
                  << std::endl;
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(options.certificate, error) || error) {
        std::cerr << "TLS certificate is not a readable regular file: "
                  << options.certificate << std::endl;
        return false;
    }
    error.clear();
    if (!std::filesystem::is_regular_file(options.private_key, error) || error) {
        std::cerr << "TLS private key is not a readable regular file: "
                  << options.private_key << std::endl;
        return false;
    }
#ifndef _WIN32
    error.clear();
    const auto permissions = std::filesystem::status(options.private_key, error).permissions();
    constexpr auto unsafe_permissions =
        std::filesystem::perms::group_read | std::filesystem::perms::group_write |
        std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
        std::filesystem::perms::others_write | std::filesystem::perms::others_exec;
    if (error || (permissions & unsafe_permissions) != std::filesystem::perms::none) {
        std::cerr << "TLS private key must not be accessible by group or other users: "
                  << options.private_key << std::endl;
        return false;
    }
#endif
    return true;
}

bool ValidateServerCredentials(const ServerOptions& options) {
    ERR_clear_error();
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(
        SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context) {
        std::cerr << "Unable to initialize TLS: " << LastOpenSslError() << std::endl;
        return false;
    }
    if (SSL_CTX_use_certificate_chain_file(
            context.get(), options.certificate.string().c_str()) != 1) {
        std::cerr << "Unable to load TLS certificate chain: " << LastOpenSslError()
                  << std::endl;
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(
            context.get(), options.private_key.string().c_str(), SSL_FILETYPE_PEM) != 1) {
        std::cerr << "Unable to load TLS private key: " << LastOpenSslError() << std::endl;
        return false;
    }
    if (SSL_CTX_check_private_key(context.get()) != 1) {
        std::cerr << "TLS certificate and private key do not match: "
                  << LastOpenSslError() << std::endl;
        return false;
    }

    const X509* certificate = SSL_CTX_get0_certificate(context.get());
    if (certificate == nullptr || X509_cmp_current_time(X509_get0_notBefore(certificate)) >= 0) {
        std::cerr << "TLS certificate is not yet valid or has an invalid start time" << std::endl;
        return false;
    }
    if (X509_cmp_current_time(X509_get0_notAfter(certificate)) <= 0) {
        std::cerr << "TLS certificate has expired or has an invalid expiration time" << std::endl;
        return false;
    }
    if (X509_check_ca(const_cast<X509*>(certificate)) != 0 ||
        X509_check_purpose(const_cast<X509*>(certificate), X509_PURPOSE_SSL_SERVER, 0) != 1) {
        std::cerr << "TLS leaf certificate is not valid for server authentication" << std::endl;
        return false;
    }

    STACK_OF(X509)* chain = nullptr;
    SSL_CTX_get_extra_chain_certs(context.get(), &chain);
    if (chain == nullptr || sk_X509_num(chain) == 0) {
        std::cerr << "TLS certificate file must contain the complete chain through its root CA"
                  << std::endl;
        return false;
    }
    const X509* subject = certificate;
    for (int index = 0; index < sk_X509_num(chain); ++index) {
        const X509* issuer = sk_X509_value(chain, index);
        if (X509_cmp_current_time(X509_get0_notBefore(issuer)) >= 0 ||
            X509_cmp_current_time(X509_get0_notAfter(issuer)) <= 0 ||
            X509_check_ca(const_cast<X509*>(issuer)) == 0 ||
            X509_check_issued(const_cast<X509*>(issuer), const_cast<X509*>(subject)) != X509_V_OK) {
            std::cerr << "TLS certificate chain is invalid" << std::endl;
            return false;
        }
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> issuer_key(
            X509_get_pubkey(const_cast<X509*>(issuer)), EVP_PKEY_free);
        if (!issuer_key || X509_verify(const_cast<X509*>(subject), issuer_key.get()) != 1) {
            std::cerr << "TLS certificate chain signature verification failed" << std::endl;
            return false;
        }
        subject = issuer;
    }
    if (X509_check_issued(const_cast<X509*>(subject), const_cast<X509*>(subject)) != X509_V_OK) {
        std::cerr << "TLS certificate chain must end with a self-signed root CA" << std::endl;
        return false;
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> root_key(
        X509_get_pubkey(const_cast<X509*>(subject)), EVP_PKEY_free);
    if (!root_key || X509_verify(const_cast<X509*>(subject), root_key.get()) != 1) {
        std::cerr << "TLS root CA self-signature verification failed" << std::endl;
        return false;
    }
    return true;
}

} // namespace cim
