#include "uproxy/http2.h"

#include "test_util.h"

using namespace uproxy;

int main_http2_tests() {
    RingBuffer b(1024);
    H2FrameWriter writer;
    check(writer.write_ping(b, 0x0102030405060708ULL, false).is_ok());
    H2Frame frame;
    H2FrameReader reader;
    check(reader.read_frame(b, frame, 16384) == ParseResult::Complete);
    check(frame.type == H2FrameType::PING);
    check(frame.length == 8);
    check(frame.stream_id == 0);
    return 0;
}
