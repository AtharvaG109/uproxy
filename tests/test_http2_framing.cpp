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

    RingBuffer data_buf(1024);
    const unsigned char payload[5] = {'h', 'e', 'l', 'l', 'o'};
    check(writer.write_data(data_buf, 1, payload, true, 3).is_ok());
    check(reader.read_frame(data_buf, frame, 16384) == ParseResult::Complete);
    check(frame.type == H2FrameType::DATA);
    check(frame.length == 3);
    check((frame.flags & H2Flags::END_STREAM) == 0);
    check(reader.read_frame(data_buf, frame, 16384) == ParseResult::Complete);
    check(frame.type == H2FrameType::DATA);
    check(frame.length == 2);
    check((frame.flags & H2Flags::END_STREAM) != 0);
    return 0;
}
