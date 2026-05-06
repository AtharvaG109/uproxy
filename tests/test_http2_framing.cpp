#include "uproxy/http2.h"

#include <cassert>

using namespace uproxy;

int main_http2_tests() {
    RingBuffer b(1024);
    H2FrameWriter writer;
    assert(writer.write_ping(b, 0x0102030405060708ULL, false).is_ok());
    H2Frame frame;
    H2FrameReader reader;
    assert(reader.read_frame(b, frame, 16384) == ParseResult::Complete);
    assert(frame.type == H2FrameType::PING);
    assert(frame.length == 8);
    assert(frame.stream_id == 0);
    return 0;
}
