#include "uproxy/http2.h"

extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, unsigned long size) {
    uproxy::RingBuffer buf(size + 1);
    (void)buf.append(std::span<const unsigned char>(data, size));
    uproxy::H2Frame frame;
    uproxy::H2FrameReader reader;
    (void)reader.read_frame(buf, frame, 16384);
    return 0;
}
