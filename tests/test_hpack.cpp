#include "uproxy/hpack.h"

#include <cassert>

using namespace uproxy;

int main_hpack_tests() {
    RingBuffer b(1024);
    HpackEncoder enc;
    std::vector<HpackHeader> headers{{":method", "GET"}, {":path", "/"}};
    assert(enc.encode(headers, b).is_ok());
    HpackDecoder dec;
    std::vector<HpackHeader> out;
    auto data = b.peek(b.readable());
    assert(dec.decode(data, out).is_ok());
    assert(out.size() == 2);
    assert(out[0].name == ":method");
    return 0;
}
