#include "uproxy/hpack.h"

#include "test_util.h"

using namespace uproxy;

int main_hpack_tests() {
    RingBuffer b(1024);
    HpackEncoder enc;
    std::vector<HpackHeader> headers{{":method", "GET"}, {":path", "/"}};
    check(enc.encode(headers, b).is_ok());
    HpackDecoder dec;
    std::vector<HpackHeader> out;
    auto data = b.peek(b.readable());
    check(dec.decode(data, out).is_ok());
    check(out.size() == 2);
    check(out[0].name == ":method");
    return 0;
}
