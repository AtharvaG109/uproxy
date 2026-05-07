#include "uproxy/http1.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <limits>
#include <sstream>

namespace uproxy {

namespace {

bool is_token_char(unsigned char c) {
    if (std::isalnum(c) != 0) {
        return true;
    }
    constexpr std::string_view extra = "!#$%&'*+-.^_`|~";
    return extra.find(static_cast<char>(c)) != std::string_view::npos;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim_ows(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

bool valid_header_value(std::string_view value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == '\r' || c == '\n') {
            return false;
        }
        if (c < 0x20 && c != '\t') {
            return false;
        }
    }
    return true;
}

ParseResult parse_headers(std::string_view bytes, HttpRequest* req, HttpResponse* resp) {
    const size_t end = bytes.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        if (bytes.size() > HTTP1_MAX_HEADER_BYTES || bytes.find('\n') != std::string_view::npos) {
            return ParseResult::Error;
        }
        return ParseResult::Incomplete;
    }
    if (end + 4 > HTTP1_MAX_HEADER_BYTES) {
        return ParseResult::Error;
    }
    const std::string_view header_block = bytes.substr(0, end + 2);
    if (header_block.find('\n') != std::string_view::npos) {
        for (size_t i = 0; i < header_block.size(); ++i) {
            if (header_block[i] == '\n' && (i == 0 || header_block[i - 1] != '\r')) {
                return ParseResult::Error;
            }
        }
    }

    size_t pos = 0;
    const size_t line_end = header_block.find("\r\n");
    if (line_end == std::string_view::npos) {
        return ParseResult::Incomplete;
    }
    const std::string_view start = header_block.substr(0, line_end);
    pos = line_end + 2;

    if (req != nullptr) {
        const size_t sp1 = start.find(' ');
        const size_t sp2 =
            sp1 == std::string_view::npos ? std::string_view::npos : start.find(' ', sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos ||
            start.find(' ', sp2 + 1) != std::string_view::npos) {
            return ParseResult::Error;
        }
        const auto method = start.substr(0, sp1);
        const auto target = start.substr(sp1 + 1, sp2 - sp1 - 1);
        const auto version = start.substr(sp2 + 1);
        if (method.empty() || method.size() > HTTP1_MAX_METHOD_LEN ||
            !std::all_of(method.begin(), method.end(),
                         [](char c) { return is_token_char(static_cast<unsigned char>(c)); })) {
            return ParseResult::Error;
        }
        if (target.empty() || target.size() > HTTP1_MAX_TARGET_LEN ||
            (version != "HTTP/1.1" && version != "HTTP/1.0")) {
            return ParseResult::Error;
        }
        req->method = std::string(method);
        req->target = std::string(target);
        req->version = std::string(version);
        req->keep_alive = version == "HTTP/1.1";
        req->headers.clear();
        req->content_length = 0;
        req->chunked = false;
        req->header_bytes = end + 4;
    } else if (resp != nullptr) {
        if (start.rfind("HTTP/", 0) != 0 || start.size() < 12) {
            return ParseResult::Error;
        }
        int status = 0;
        const auto code = start.substr(9, 3);
        const auto [ptr, ec] = std::from_chars(code.data(), code.data() + code.size(), status);
        if (ec != std::errc() || ptr != code.data() + code.size()) {
            return ParseResult::Error;
        }
        resp->status = status;
        resp->reason = start.size() > 13 ? std::string(start.substr(13)) : "";
        resp->headers.clear();
        resp->content_length = 0;
        resp->chunked = false;
        resp->header_bytes = end + 4;
    } else {
        return ParseResult::Error;
    }

    bool saw_content_length = false;
    uint64_t content_length = 0;
    while (pos < header_block.size()) {
        const size_t next = header_block.find("\r\n", pos);
        if (next == std::string_view::npos || next == pos) {
            break;
        }
        const std::string_view line = header_block.substr(pos, next - pos);
        pos = next + 2;
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            return ParseResult::Error;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return ParseResult::Error;
        }
        if (line[colon - 1] == ' ' || line[colon - 1] == '\t') {
            return ParseResult::Error;
        }
        const std::string_view name = line.substr(0, colon);
        if (name.size() > HTTP1_MAX_HEADER_NAME ||
            !std::all_of(name.begin(), name.end(),
                         [](char c) { return is_token_char(static_cast<unsigned char>(c)); })) {
            return ParseResult::Error;
        }
        const std::string value = trim_ows(line.substr(colon + 1));
        if (value.size() > HTTP1_MAX_HEADER_VALUE || !valid_header_value(value)) {
            return ParseResult::Error;
        }
        std::vector<HttpHeader>& headers = req != nullptr ? req->headers : resp->headers;
        if (headers.size() >= HTTP1_MAX_HEADER_COUNT) {
            return ParseResult::Error;
        }
        const std::string lname = lower(name);
        if (lname == "content-length") {
            uint64_t parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec != std::errc() || ptr != value.data() + value.size()) {
                return ParseResult::Error;
            }
            if (saw_content_length && parsed != content_length) {
                return ParseResult::Error;
            }
            saw_content_length = true;
            content_length = parsed;
        } else if (lname == "transfer-encoding" &&
                   lower(value).find("chunked") != std::string::npos) {
            if (req != nullptr) {
                req->chunked = true;
            } else {
                resp->chunked = true;
            }
        } else if (lname == "connection") {
            const auto lv = lower(value);
            if (req != nullptr && lv.find("close") != std::string::npos) {
                req->keep_alive = false;
            }
        }
        headers.push_back({std::string(name), value});
    }

    if (req != nullptr) {
        if (req->chunked && saw_content_length) {
            return ParseResult::Error;
        }
        req->content_length = content_length;
    } else if (resp != nullptr) {
        if (resp->chunked && saw_content_length) {
            return ParseResult::Error;
        }
        resp->content_length = content_length;
    }
    return ParseResult::Complete;
}

} // namespace

ParseResult Http1Parser::parse_request(const RingBuffer& buf, HttpRequest& req) {
    auto bytes = buf.peek(std::min(buf.readable(), HTTP1_MAX_HEADER_BYTES + 1));
    return parse_headers(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &req, nullptr);
}

ParseResult Http1Parser::parse_response(const RingBuffer& buf, HttpResponse& resp) {
    auto bytes = buf.peek(std::min(buf.readable(), HTTP1_MAX_HEADER_BYTES + 1));
    return parse_headers(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), nullptr,
        &resp);
}

int64_t Http1Parser::parse_chunk_header(RingBuffer& buf) {
    auto bytes = buf.peek(std::min<size_t>(buf.readable(), 128));
    std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const size_t crlf = sv.find("\r\n");
    if (crlf == std::string_view::npos) {
        return -1;
    }
    std::string_view hex = sv.substr(0, crlf);
    const size_t semi = hex.find(';');
    if (semi != std::string_view::npos) {
        hex = hex.substr(0, semi);
    }
    uint64_t n = 0;
    const auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), n, 16);
    if (ec != std::errc() || ptr != hex.data() + hex.size() ||
        n > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return -1;
    }
    buf.commit_read(crlf + 2);
    return static_cast<int64_t>(n);
}

Result<void> Http1Serializer::write_request(RingBuffer& buf, const HttpRequest& req,
                                            std::string_view host) {
    std::ostringstream os;
    os << req.method << ' ' << req.target << ' ' << req.version << "\r\n";
    os << "Host: " << host << "\r\n";
    bool saw_content_length = false;
    for (const auto& h : req.headers) {
        const std::string name = lower(h.name);
        if (name == "host" || name == "connection" || name == "keep-alive" ||
            name == "proxy-authenticate" || name == "proxy-authorization" || name == "te" ||
            name == "trailers" || name == "transfer-encoding" || name == "upgrade") {
            continue;
        }
        if (name == "content-length") {
            saw_content_length = true;
        }
        if (!valid_header_value(h.value)) {
            return Result<void>::err(
                Error::from_code(ErrCode::HttpMalformed, "invalid header value"));
        }
        os << h.name << ": " << h.value << "\r\n";
    }
    if (req.content_length > 0 && !saw_content_length) {
        os << "Content-Length: " << req.content_length << "\r\n";
    }
    os << "Via: 1.1 uproxy\r\n\r\n";
    return buf.append(os.str());
}

Result<void> Http1Serializer::write_response(RingBuffer& buf, const HttpResponse& resp) {
    std::ostringstream os;
    os << "HTTP/1.1 " << resp.status << ' ' << resp.reason << "\r\n";
    for (const auto& h : resp.headers) {
        const std::string name = lower(h.name);
        if (name == "connection" || name == "keep-alive" || name == "proxy-authenticate" ||
            name == "proxy-authorization" || name == "te" || name == "trailers" ||
            name == "transfer-encoding" || name == "upgrade") {
            continue;
        }
        if (!valid_header_value(h.value)) {
            return Result<void>::err(
                Error::from_code(ErrCode::HttpMalformed, "invalid header value"));
        }
        os << h.name << ": " << h.value << "\r\n";
    }
    os << "Via: 1.1 uproxy\r\n\r\n";
    return buf.append(os.str());
}

Result<void> Http1Serializer::write_chunk_header(RingBuffer& buf, size_t chunk_size) {
    char tmp[32];
    const int n = std::snprintf(tmp, sizeof(tmp), "%zx\r\n", chunk_size);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) {
        return Result<void>::err(Error::from_code(ErrCode::HttpMalformed, "chunk too large"));
    }
    return buf.append(std::string_view(tmp, static_cast<size_t>(n)));
}

} // namespace uproxy
