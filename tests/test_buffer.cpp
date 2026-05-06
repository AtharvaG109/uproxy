#include "uproxy/buffer.h"

#include <cassert>
#include <string>

using namespace uproxy;

int main_buffer_tests() {
    RingBuffer b(4);
    assert(b.append("abcd").is_ok());
    assert(b.readable() == 4);
    assert(std::string(reinterpret_cast<const char*>(b.peek(4).data()), 4) == "abcd");
    b.commit_read(2);
    assert(b.append("ef").is_ok());
    auto wrapped = b.peek(4);
    assert(std::string(reinterpret_cast<const char*>(wrapped.data()), wrapped.size()) == "cdef");
    assert(b.ensure_writable(8).is_ok());
    assert(b.append("ghij").is_ok());
    auto all = b.peek(b.readable());
    assert(std::string(reinterpret_cast<const char*>(all.data()), all.size()) == "cdefghij");
    return 0;
}
