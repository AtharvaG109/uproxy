#pragma once

#include "uproxy/buffer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uproxy {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
    std::vector<HttpHeader> headers;
    bool keep_alive{true};
    uint64_t content_length{0};
    bool chunked{false};
    size_t header_bytes{0};
};

struct HttpResponse {
    int status{200};
    std::string reason{"OK"};
    std::vector<HttpHeader> headers;
    uint64_t content_length{0};
    bool chunked{false};
    size_t header_bytes{0};
};

constexpr size_t HTTP1_MAX_METHOD_LEN = 16;
constexpr size_t HTTP1_MAX_TARGET_LEN = 8192;
constexpr size_t HTTP1_MAX_HEADER_NAME = 256;
constexpr size_t HTTP1_MAX_HEADER_VALUE = 8192;
constexpr size_t HTTP1_MAX_HEADER_COUNT = 100;
constexpr size_t HTTP1_MAX_HEADER_BYTES = 65536;

enum class ParseResult { Complete, Incomplete, Error };

class Http1Parser {
  public:
    [[nodiscard]] ParseResult parse_request(const RingBuffer& buf, HttpRequest& req);
    [[nodiscard]] ParseResult parse_response(const RingBuffer& buf, HttpResponse& resp);
    [[nodiscard]] int64_t parse_chunk_header(RingBuffer& buf);
};

class Http1Serializer {
  public:
    [[nodiscard]] Result<void> write_request(RingBuffer& buf, const HttpRequest& req,
                                             std::string_view host);
    [[nodiscard]] Result<void> write_response(RingBuffer& buf, const HttpResponse& resp);
    [[nodiscard]] Result<void> write_chunk_header(RingBuffer& buf, size_t chunk_size);
};

} // namespace uproxy
