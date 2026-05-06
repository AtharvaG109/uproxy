#include "uproxy/http1.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, unsigned long size) {
    uproxy::RingBuffer buf(size + 1);
    (void)buf.append(std::span<const unsigned char>(data, size));
    uproxy::HttpRequest req;
    uproxy::Http1Parser parser;
    (void)parser.parse_request(buf, req);
    return 0;
}
