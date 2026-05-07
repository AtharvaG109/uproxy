#pragma once

#include "uproxy/config.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uproxy {

struct TLSContextImpl;
struct TLSConnImpl;

class TLSContext {
    std::shared_ptr<TLSContextImpl> impl_;

  public:
    static Result<TLSContext> server(const TLSConfig& cfg);
    static Result<TLSContext> client();
    [[nodiscard]] TLSContextImpl* get() const noexcept {
        return impl_.get();
    }
    explicit TLSContext(std::shared_ptr<TLSContextImpl> impl = {}) : impl_(std::move(impl)) {}
};

enum class TLSHandshakeState { Pending, Done };

class TLSConn {
    std::unique_ptr<TLSConnImpl> impl_;
    bool handshake_done_{false};

  public:
    TLSConn(TLSContextImpl* ctx, bool is_server);
    TLSConn(TLSConn&&) noexcept;
    TLSConn& operator=(TLSConn&&) noexcept;
    ~TLSConn();

    [[nodiscard]] Result<TLSHandshakeState> do_handshake();
    [[nodiscard]] Result<void> feed_encrypted(std::span<const unsigned char> data);
    [[nodiscard]] Result<size_t> take_encrypted(std::span<unsigned char> out);
    [[nodiscard]] Result<size_t> read(std::span<unsigned char> out);
    [[nodiscard]] Result<size_t> write(std::span<const unsigned char> data);
    [[nodiscard]] bool want_read() const noexcept;
    [[nodiscard]] bool want_write() const noexcept;
    [[nodiscard]] std::string_view alpn_protocol() const noexcept;
    [[nodiscard]] Result<void> shutdown();
};

} // namespace uproxy
