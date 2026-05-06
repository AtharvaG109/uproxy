#include "uproxy/result.h"

#include <array>

namespace uproxy {

namespace {

std::string_view code_name(ErrCode code) {
    switch (code) {
    case ErrCode::Ok:
        return "ok";
    case ErrCode::SysError:
        return "sys_error";
    case ErrCode::WouldBlock:
        return "would_block";
    case ErrCode::EOF_:
        return "eof";
    case ErrCode::HttpMalformed:
        return "http_malformed";
    case ErrCode::Http2Protocol:
        return "http2_protocol";
    case ErrCode::HpackError:
        return "hpack_error";
    case ErrCode::ConfigSyntax:
        return "config_syntax";
    case ErrCode::ConfigInvalid:
        return "config_invalid";
    case ErrCode::TlsHandshake:
        return "tls_handshake";
    case ErrCode::TlsIO:
        return "tls_io";
    case ErrCode::NoUpstream:
        return "no_upstream";
    case ErrCode::Timeout:
        return "timeout";
    }
    return "unknown";
}

} // namespace

Error Error::from_errno(std::string_view ctx) {
    Error e;
    e.code = ErrCode::SysError;
    e.sys_errno = errno;
    e.msg = std::string(ctx);
    return e;
}

Error Error::from_code(ErrCode c, std::string_view msg) {
    Error e;
    e.code = c;
    e.msg = std::string(msg);
    return e;
}

std::string Error::to_string() const {
    std::string out(code_name(code));
    if (!msg.empty()) {
        out += ": ";
        out += msg;
    }
    if (code == ErrCode::SysError) {
        out += ": ";
        out += std::strerror(sys_errno);
    }
    return out;
}

} // namespace uproxy
