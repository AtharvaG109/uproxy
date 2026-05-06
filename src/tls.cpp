#include "uproxy/tls.h"

#include <algorithm>
#include <deque>

#if defined(UPROXY_HAS_BORINGSSL)
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace uproxy {

#if defined(UPROXY_HAS_BORINGSSL)

struct TLSContextImpl {
    bool server{false};
    TLSConfig cfg;
    SSL_CTX* ssl_ctx{nullptr};

    ~TLSContextImpl() {
        if (ssl_ctx) SSL_CTX_free(ssl_ctx);
    }
};

struct TLSConnImpl {
    TLSContextImpl* ctx{nullptr};
    bool server{false};
    SSL* ssl{nullptr};
    BIO* read_bio{nullptr};
    BIO* write_bio{nullptr};
    std::string alpn{"http/1.1"};

    ~TLSConnImpl() {
        if (ssl) SSL_free(ssl);
    }
};

static int alpn_select_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                          const unsigned char* in, unsigned int inlen, void* arg) {
    (void)ssl;
    (void)arg;
    bool has_h2 = false;
    bool has_h1 = false;
    for (unsigned int i = 0; i < inlen;) {
        unsigned int len = in[i];
        i++;
        if (i + len > inlen) break;
        std::string_view proto(reinterpret_cast<const char*>(in + i), len);
        if (proto == "h2") has_h2 = true;
        if (proto == "http/1.1") has_h1 = true;
        i += len;
    }
    if (has_h2) {
        *out = reinterpret_cast<const unsigned char*>("h2");
        *outlen = 2;
        return SSL_TLSEXT_ERR_OK;
    }
    if (has_h1) {
        *out = reinterpret_cast<const unsigned char*>("http/1.1");
        *outlen = 8;
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

Result<TLSContext> TLSContext::server(const TLSConfig& cfg) {
    if (!cfg.enabled) {
        auto impl = std::make_shared<TLSContextImpl>();
        impl->server = true;
        impl->cfg = cfg;
        return Result<TLSContext>::ok(TLSContext(std::move(impl)));
    }

    auto impl = std::make_shared<TLSContextImpl>();
    impl->server = true;
    impl->cfg = cfg;

    impl->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!impl->ssl_ctx) {
        return Result<TLSContext>::err(Error::from_code(ErrCode::SysError, "SSL_CTX_new failed"));
    }

    if (SSL_CTX_use_certificate_chain_file(impl->ssl_ctx, cfg.cert_path.c_str()) != 1) {
        return Result<TLSContext>::err(Error::from_code(ErrCode::ConfigInvalid, "Failed to load cert_path"));
    }
    if (SSL_CTX_use_PrivateKey_file(impl->ssl_ctx, cfg.key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        return Result<TLSContext>::err(Error::from_code(ErrCode::ConfigInvalid, "Failed to load key_path"));
    }
    if (SSL_CTX_check_private_key(impl->ssl_ctx) != 1) {
        return Result<TLSContext>::err(Error::from_code(ErrCode::ConfigInvalid, "Private key does not match certificate"));
    }

    SSL_CTX_set_alpn_select_cb(impl->ssl_ctx, alpn_select_cb, nullptr);

    return Result<TLSContext>::ok(TLSContext(std::move(impl)));
}

Result<TLSContext> TLSContext::client() {
    auto impl = std::make_shared<TLSContextImpl>();
    impl->server = false;
    impl->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!impl->ssl_ctx) {
        return Result<TLSContext>::err(Error::from_code(ErrCode::SysError, "SSL_CTX_new failed"));
    }
    return Result<TLSContext>::ok(TLSContext(std::move(impl)));
}

TLSConn::TLSConn(TLSContextImpl* ctx, bool is_server)
    : impl_(std::make_unique<TLSConnImpl>()), handshake_done_(!is_server) {
    impl_->ctx = ctx;
    impl_->server = is_server;
    
    if (ctx && ctx->ssl_ctx) {
        impl_->ssl = SSL_new(ctx->ssl_ctx);
        impl_->read_bio = BIO_new(BIO_s_mem());
        impl_->write_bio = BIO_new(BIO_s_mem());
        BIO_set_mem_eof_return(impl_->read_bio, -1);
        BIO_set_mem_eof_return(impl_->write_bio, -1);
        SSL_set_bio(impl_->ssl, impl_->read_bio, impl_->write_bio);
        if (is_server) {
            SSL_set_accept_state(impl_->ssl);
        } else {
            SSL_set_connect_state(impl_->ssl);
        }
    }
}

TLSConn::TLSConn(TLSConn&&) noexcept = default;
TLSConn& TLSConn::operator=(TLSConn&&) noexcept = default;
TLSConn::~TLSConn() = default;

Result<TLSHandshakeState> TLSConn::do_handshake() {
    if (!impl_->ssl) {
        handshake_done_ = true;
        return Result<TLSHandshakeState>::ok(TLSHandshakeState::Done);
    }
    if (handshake_done_) {
        return Result<TLSHandshakeState>::ok(TLSHandshakeState::Done);
    }

    int ret = SSL_do_handshake(impl_->ssl);
    if (ret == 1) {
        handshake_done_ = true;
        const unsigned char* alpn_data = nullptr;
        unsigned int alpn_len = 0;
        SSL_get0_alpn_selected(impl_->ssl, &alpn_data, &alpn_len);
        if (alpn_data && alpn_len > 0) {
            impl_->alpn = std::string(reinterpret_cast<const char*>(alpn_data), alpn_len);
        }
        return Result<TLSHandshakeState>::ok(TLSHandshakeState::Done);
    }

    int err = SSL_get_error(impl_->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return Result<TLSHandshakeState>::ok(TLSHandshakeState::Pending);
    }
    
    return Result<TLSHandshakeState>::err(Error::from_code(ErrCode::TlsHandshake, "TLS handshake failed"));
}

Result<void> TLSConn::feed_encrypted(std::span<const unsigned char> data) {
    if (!impl_->ssl) return Result<void>::ok();
    if (data.empty()) return Result<void>::ok();
    int ret = BIO_write(impl_->read_bio, data.data(), static_cast<int>(data.size()));
    if (ret <= 0) {
        return Result<void>::err(Error::from_code(ErrCode::SysError, "BIO_write failed"));
    }
    return Result<void>::ok();
}

Result<size_t> TLSConn::take_encrypted(std::span<unsigned char> out) {
    if (!impl_->ssl) return Result<size_t>::ok(0);
    int ret = BIO_read(impl_->write_bio, out.data(), static_cast<int>(out.size()));
    if (ret > 0) return Result<size_t>::ok(static_cast<size_t>(ret));
    if (BIO_should_retry(impl_->write_bio)) return Result<size_t>::ok(0);
    return Result<size_t>::err(Error::from_code(ErrCode::SysError, "BIO_read failed"));
}

Result<size_t> TLSConn::read(std::span<unsigned char> out) {
    if (!impl_->ssl) return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "no mock data"));
    if (!handshake_done_) return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "handshake pending"));
    
    int ret = SSL_read(impl_->ssl, out.data(), static_cast<int>(out.size()));
    if (ret > 0) {
        return Result<size_t>::ok(static_cast<size_t>(ret));
    }
    int err = SSL_get_error(impl_->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "SSL_read would block"));
    }
    return Result<size_t>::err(Error::from_code(ErrCode::SysError, "SSL_read failed"));
}

Result<size_t> TLSConn::write(std::span<const unsigned char> data) {
    if (!impl_->ssl) return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "no mock data"));
    if (!handshake_done_) return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "handshake pending"));
    
    int ret = SSL_write(impl_->ssl, data.data(), static_cast<int>(data.size()));
    if (ret > 0) {
        return Result<size_t>::ok(static_cast<size_t>(ret));
    }
    int err = SSL_get_error(impl_->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "SSL_write would block"));
    }
    return Result<size_t>::err(Error::from_code(ErrCode::SysError, "SSL_write failed"));
}

bool TLSConn::want_read() const noexcept {
    return true; // We always can read from the network if data is available
}

bool TLSConn::want_write() const noexcept {
    if (!impl_ || !impl_->ssl) return false;
    return BIO_pending(impl_->write_bio) > 0;
}

std::string_view TLSConn::alpn_protocol() const noexcept {
    return impl_ == nullptr ? std::string_view("http/1.1") : std::string_view(impl_->alpn);
}

Result<void> TLSConn::shutdown() {
    if (impl_->ssl) {
        SSL_shutdown(impl_->ssl);
    }
    handshake_done_ = false;
    return Result<void>::ok();
}

#else

struct TLSContextImpl {
    bool server{false};
    TLSConfig cfg;
};

struct TLSConnImpl {
    TLSContextImpl* ctx{nullptr};
    bool server{false};
    std::deque<unsigned char> inbound;
    std::deque<unsigned char> outbound;
    std::string alpn{"http/1.1"};
};

Result<TLSContext> TLSContext::server(const TLSConfig& cfg) {
    if (cfg.enabled) {
        return Result<TLSContext>::err(Error::from_code(
            ErrCode::TlsHandshake,
            "BoringSSL submodule is required for TLS; run with --no-tls in fallback builds"));
    }
    auto impl = std::make_shared<TLSContextImpl>();
    impl->server = true;
    impl->cfg = cfg;
    return Result<TLSContext>::ok(TLSContext(std::move(impl)));
}

Result<TLSContext> TLSContext::client() {
    auto impl = std::make_shared<TLSContextImpl>();
    impl->server = false;
    return Result<TLSContext>::ok(TLSContext(std::move(impl)));
}

TLSConn::TLSConn(TLSContextImpl* ctx, bool is_server)
    : impl_(std::make_unique<TLSConnImpl>()), handshake_done_(!is_server) {
    impl_->ctx = ctx;
    impl_->server = is_server;
}

TLSConn::TLSConn(TLSConn&&) noexcept = default;
TLSConn& TLSConn::operator=(TLSConn&&) noexcept = default;
TLSConn::~TLSConn() = default;

Result<TLSHandshakeState> TLSConn::do_handshake() {
    if (impl_ != nullptr && impl_->ctx != nullptr && impl_->ctx->cfg.enabled) {
        return Result<TLSHandshakeState>::err(
            Error::from_code(ErrCode::TlsHandshake, "TLS unavailable in fallback build"));
    }
    handshake_done_ = true;
    return Result<TLSHandshakeState>::ok(TLSHandshakeState::Done);
}

Result<void> TLSConn::feed_encrypted(std::span<const unsigned char> data) {
    impl_->inbound.insert(impl_->inbound.end(), data.begin(), data.end());
    return Result<void>::ok();
}

Result<size_t> TLSConn::take_encrypted(std::span<unsigned char> out) {
    const size_t n = std::min(out.size(), impl_->outbound.size());
    for (size_t i = 0; i < n; ++i) {
        out[i] = impl_->outbound.front();
        impl_->outbound.pop_front();
    }
    return Result<size_t>::ok(n);
}

Result<size_t> TLSConn::read(std::span<unsigned char> out) {
    if (!handshake_done_) {
        return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "handshake pending"));
    }
    const size_t n = std::min(out.size(), impl_->inbound.size());
    if (n == 0) {
        return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "no TLS data"));
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = impl_->inbound.front();
        impl_->inbound.pop_front();
    }
    return Result<size_t>::ok(n);
}

Result<size_t> TLSConn::write(std::span<const unsigned char> data) {
    if (!handshake_done_) {
        return Result<size_t>::err(Error::from_code(ErrCode::WouldBlock, "handshake pending"));
    }
    impl_->outbound.insert(impl_->outbound.end(), data.begin(), data.end());
    return Result<size_t>::ok(data.size());
}

bool TLSConn::want_read() const noexcept {
    return impl_ != nullptr && impl_->inbound.empty();
}

bool TLSConn::want_write() const noexcept {
    return impl_ != nullptr && !impl_->outbound.empty();
}

std::string_view TLSConn::alpn_protocol() const noexcept {
    return impl_ == nullptr ? std::string_view("http/1.1") : std::string_view(impl_->alpn);
}

Result<void> TLSConn::shutdown() {
    handshake_done_ = false;
    return Result<void>::ok();
}

#endif

} // namespace uproxy
