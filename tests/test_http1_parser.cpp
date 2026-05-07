#include "uproxy/http1.h"

#include "test_util.h"

#include <string>

using namespace uproxy;

int main_http1_tests() {
    RingBuffer b(1024);
    check(
        b.append("GET /x?q=1 HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n").is_ok());
    HttpRequest req;
    Http1Parser parser;
    check(parser.parse_request(b, req) == ParseResult::Complete);
    check(req.method == "GET");
    check(req.target == "/x?q=1");
    check(!req.keep_alive);

    RingBuffer smuggle(1024);
    check(smuggle
              .append("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: "
                      "chunked\r\n\r\n")
              .is_ok());
    check(parser.parse_request(smuggle, req) == ParseResult::Error);

    HttpRequest h2_translated;
    h2_translated.method = "POST";
    h2_translated.target = "/submit";
    h2_translated.version = "HTTP/1.1";
    h2_translated.content_length = 5;
    h2_translated.headers.push_back({"user-agent", "uproxy-test"});
    RingBuffer serialized(1024);
    Http1Serializer serializer;
    check(serializer.write_request(serialized, h2_translated, "127.0.0.1:3000").is_ok());
    const auto serialized_bytes = serialized.peek(serialized.readable());
    const std::string serialized_text(reinterpret_cast<const char*>(serialized_bytes.data()),
                                      serialized_bytes.size());
    check(serialized_text.find("Content-Length: 5\r\n") != std::string::npos);

    RingBuffer bare_lf(1024);
    check(bare_lf.append("GET / HTTP/1.1\nHost: x\n\n").is_ok());
    check(parser.parse_request(bare_lf, req) == ParseResult::Error);
    return 0;
}
