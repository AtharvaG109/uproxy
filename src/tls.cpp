#include "uproxy/tls.h"

#include <algorithm>
#include <deque>

namespace uproxy {

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
#if defined(UPROXY_HAS_BORINGSSL)
    auto impl = std::make_shared<TLSContextImpl>();
    impl->server = true;
    impl->cfg = cfg;
    return Result<TLSContext>::ok(TLSContext(std::move(impl)));
#else
    if (cfg.enabled) {
        return Result<TLSContext>::err(Error::from_code(
            ErrCode::TlsHandshake,
            "BoringSSL submodule is required for TLS; run with --no-tls in fallback builds"));
    }
    auto impl = std::make_shared<TLSContextImpl>();
    impl->server = true;
    impl->cfg = cfg;
    return Result<TLSContext>::ok(TLSContext(std::move(impl)));
#endif
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
#if defined(UPROXY_HAS_BORINGSSL)
    handshake_done_ = true;
    return Result<TLSHandshakeState>::ok(TLSHandshakeState::Done);
#else
    if (impl_ != nullptr && impl_->ctx != nullptr && impl_->ctx->cfg.enabled) {
        return Result<TLSHandshakeState>::err(
            Error::from_code(ErrCode::TlsHandshake, "TLS unavailable in fallback build"));
    }
    handshake_done_ = true;
    return Result<TLSHandshakeState>::ok(TLSHandshakeState::Done);
#endif
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

} // namespace uproxy
