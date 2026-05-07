#pragma once

#include <cassert>
#include <cerrno>
#include <concepts>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace uproxy {

enum class ErrCode : int {
    Ok = 0,
    SysError,
    WouldBlock,
    EOF_,
    HttpMalformed,
    Http2Protocol,
    HpackError,
    ConfigSyntax,
    ConfigInvalid,
    TlsHandshake,
    TlsIO,
    NoUpstream,
    Timeout,
};

struct Error {
    ErrCode code{ErrCode::Ok};
    int sys_errno{0};
    std::string msg;

    static Error from_errno(std::string_view ctx);
    static Error from_code(ErrCode c, std::string_view msg = {});
    [[nodiscard]] std::string to_string() const;
};

template <typename T> class [[nodiscard]] Result {
    std::optional<T> value_;
    std::optional<Error> error_;

    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Error error) : error_(std::move(error)) {}

  public:
    using value_type = T;

    static Result ok(T value) {
        return Result(std::move(value));
    }
    static Result err(Error error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return value_.has_value();
    }
    [[nodiscard]] bool is_err() const noexcept {
        return error_.has_value();
    }

    T& value() & {
        assert(value_.has_value());
        return *value_;
    }
    const T& value() const& {
        assert(value_.has_value());
        return *value_;
    }
    T value() && {
        assert(value_.has_value());
        return std::move(*value_);
    }
    Error& error() & {
        assert(error_.has_value());
        return *error_;
    }
    const Error& error() const& {
        assert(error_.has_value());
        return *error_;
    }
    Error error() && {
        assert(error_.has_value());
        return std::move(*error_);
    }

    template <typename F>
    auto and_then(F&& f) & -> decltype(std::invoke(std::forward<F>(f), value())) {
        using Ret = decltype(std::invoke(std::forward<F>(f), value()));
        if (is_err()) {
            return Ret::err(error());
        }
        return std::invoke(std::forward<F>(f), value());
    }

    template <typename F>
    auto and_then(F&& f) && -> decltype(std::invoke(std::forward<F>(f), std::move(*value_))) {
        using Ret = decltype(std::invoke(std::forward<F>(f), std::move(*value_)));
        if (is_err()) {
            return Ret::err(std::move(*error_));
        }
        return std::invoke(std::forward<F>(f), std::move(*value_));
    }

    template <typename F> auto map(F&& f) & {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T&>>;
        if (is_err()) {
            return Result<U>::err(error());
        }
        return Result<U>::ok(std::invoke(std::forward<F>(f), value()));
    }

    template <typename F> auto map(F&& f) && {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T&&>>;
        if (is_err()) {
            return Result<U>::err(std::move(*error_));
        }
        return Result<U>::ok(std::invoke(std::forward<F>(f), std::move(*value_)));
    }

    template <typename F> Result or_else(F&& f) & {
        if (is_ok()) {
            return Result::ok(value());
        }
        return std::invoke(std::forward<F>(f), error());
    }
};

template <> class [[nodiscard]] Result<void> {
    std::optional<Error> error_;

    explicit Result(Error error) : error_(std::move(error)) {}

  public:
    using value_type = void;

    static Result ok() {
        return Result();
    }
    static Result err(Error error) {
        return Result(std::move(error));
    }

    Result() = default;

    [[nodiscard]] bool is_ok() const noexcept {
        return !error_.has_value();
    }
    [[nodiscard]] bool is_err() const noexcept {
        return error_.has_value();
    }

    void value() const {
        assert(!error_.has_value());
    }
    Error& error() & {
        assert(error_.has_value());
        return *error_;
    }
    const Error& error() const& {
        assert(error_.has_value());
        return *error_;
    }
    Error error() && {
        assert(error_.has_value());
        return std::move(*error_);
    }

    template <typename F> auto and_then(F&& f) {
        using Ret = decltype(std::invoke(std::forward<F>(f)));
        if (is_err()) {
            return Ret::err(*error_);
        }
        return std::invoke(std::forward<F>(f));
    }

    template <typename F> auto map(F&& f) {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        if (is_err()) {
            return Result<U>::err(*error_);
        }
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f));
            return Result<void>::ok();
        } else {
            return Result<U>::ok(std::invoke(std::forward<F>(f)));
        }
    }

    template <typename F> Result or_else(F&& f) {
        if (is_ok()) {
            return Result::ok();
        }
        return std::invoke(std::forward<F>(f), *error_);
    }
};

template <typename T> Result<std::remove_cvref_t<T>> ok(T&& value) {
    return Result<std::remove_cvref_t<T>>::ok(std::forward<T>(value));
}

inline Result<void> ok() {
    return Result<void>::ok();
}
inline Result<void> err(Error error) {
    return Result<void>::err(std::move(error));
}

} // namespace uproxy

#define UPROXY_TRY(expr)                                                                           \
    ({                                                                                             \
        auto _uproxy_result = (expr);                                                              \
        if (_uproxy_result.is_err()) {                                                             \
            return decltype(_uproxy_result)::err(_uproxy_result.error());                          \
        }                                                                                          \
        std::move(_uproxy_result).value();                                                         \
    })

#define UPROXY_TRY_VOID(expr)                                                                      \
    do {                                                                                           \
        auto _uproxy_result = (expr);                                                              \
        if (_uproxy_result.is_err()) {                                                             \
            return uproxy::Result<void>::err(_uproxy_result.error());                              \
        }                                                                                          \
    } while (false)
