#include "uproxy/buffer.h"

#include "test_util.h"

#include <string>

using namespace uproxy;

int main_buffer_tests() {
    RingBuffer b(4);
    check(b.append("abcd").is_ok());
    check(b.readable() == 4);
    check(std::string(reinterpret_cast<const char*>(b.peek(4).data()), 4) == "abcd");
    b.commit_read(2);
    check(b.append("ef").is_ok());
    auto wrapped = b.peek(4);
    check(std::string(reinterpret_cast<const char*>(wrapped.data()), wrapped.size()) == "cdef");
    check(b.ensure_writable(8).is_ok());
    check(b.append("ghij").is_ok());
    auto all = b.peek(b.readable());
    check(std::string(reinterpret_cast<const char*>(all.data()), all.size()) == "cdefghij");
    return 0;
}
