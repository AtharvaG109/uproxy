#include "uproxy/hpack.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, unsigned long size) {
    uproxy::HpackDecoder decoder;
    std::vector<uproxy::HpackHeader> out;
    (void)decoder.decode(std::span<const unsigned char>(data, size), out);
    return 0;
}
