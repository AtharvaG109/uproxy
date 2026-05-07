#pragma once

#include "uproxy/hpack.h"
#include "uproxy/http1.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uproxy {

enum class H2FrameType : uint8_t {
    DATA = 0x0,
    HEADERS = 0x1,
    PRIORITY = 0x2,
    RST_STREAM = 0x3,
    SETTINGS = 0x4,
    PUSH_PROMISE = 0x5,
    PING = 0x6,
    GOAWAY = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION = 0x9,
};

namespace H2Flags {
constexpr uint8_t END_STREAM = 0x1;
constexpr uint8_t END_HEADERS = 0x4;
constexpr uint8_t PADDED = 0x8;
constexpr uint8_t PRIORITY = 0x20;
constexpr uint8_t ACK = 0x1;
} // namespace H2Flags

enum class H2Error : uint32_t {
    NO_ERROR = 0x0,
    PROTOCOL_ERROR = 0x1,
    INTERNAL_ERROR = 0x2,
    FLOW_CONTROL_ERROR = 0x3,
    SETTINGS_TIMEOUT = 0x4,
    STREAM_CLOSED = 0x5,
    FRAME_SIZE_ERROR = 0x6,
    REFUSED_STREAM = 0x7,
    CANCEL = 0x8,
    COMPRESSION_ERROR = 0x9,
    CONNECT_ERROR = 0xa,
    ENHANCE_YOUR_CALM = 0xb,
    INADEQUATE_SECURITY = 0xc,
    HTTP_1_1_REQUIRED = 0xd,
};

enum class H2Setting : uint16_t {
    HEADER_TABLE_SIZE = 0x1,
    ENABLE_PUSH = 0x2,
    MAX_CONCURRENT_STREAMS = 0x3,
    INITIAL_WINDOW_SIZE = 0x4,
    MAX_FRAME_SIZE = 0x5,
    MAX_HEADER_LIST_SIZE = 0x6,
};

struct H2Frame {
    uint32_t length{0};
    H2FrameType type{H2FrameType::DATA};
    uint8_t flags{0};
    uint32_t stream_id{0};
    std::vector<unsigned char> payload;
};

class H2FrameReader {
    static constexpr size_t FRAME_HEADER_SIZE = 9;

  public:
    [[nodiscard]] ParseResult read_frame(RingBuffer& buf, H2Frame& frame, uint32_t max_frame_size);
};

class H2FrameWriter {
  public:
    [[nodiscard]] Result<void>
    write_settings(RingBuffer& buf, const std::vector<std::pair<H2Setting, uint32_t>>& settings);
    [[nodiscard]] Result<void> write_settings_ack(RingBuffer& buf);
    [[nodiscard]] Result<void> write_ping(RingBuffer& buf, uint64_t data, bool ack);
    [[nodiscard]] Result<void> write_window_update(RingBuffer& buf, uint32_t stream_id,
                                                   uint32_t increment);
    [[nodiscard]] Result<void> write_goaway(RingBuffer& buf, uint32_t last_stream_id, H2Error error,
                                            std::string_view debug_data);
    [[nodiscard]] Result<void> write_rst_stream(RingBuffer& buf, uint32_t stream_id, H2Error error);
    [[nodiscard]] Result<void> write_headers(RingBuffer& buf, uint32_t stream_id,
                                             std::span<const unsigned char> header_block,
                                             bool end_stream, uint32_t max_frame_size);
    [[nodiscard]] Result<void> write_data(RingBuffer& buf, uint32_t stream_id,
                                          std::span<const unsigned char> data, bool end_stream,
                                          uint32_t max_frame_size);
};

enum class H2StreamState { Idle, Open, HalfClosedLocal, HalfClosedRemote, Closed };

struct H2Stream {
    uint32_t id{0};
    H2StreamState state{H2StreamState::Idle};
    int32_t recv_window{65535};
    int32_t send_window{65535};
    RingBuffer recv_buf{65536};
    RingBuffer send_buf{65536};
    std::vector<HpackHeader> request_headers;
    std::vector<HpackHeader> response_headers;
    bool end_stream_recv{false};
    bool end_stream_sent{false};
};

class H2Conn {
    bool is_server_;
    H2FrameReader reader_;
    H2FrameWriter writer_;
    HpackEncoder hpack_enc_;
    HpackDecoder hpack_dec_;
    std::unordered_map<uint32_t, H2Stream> streams_;
    uint32_t last_peer_stream_{0};
    uint32_t max_frame_size_{16384};
    RingBuffer recv_buf_{65536};
    RingBuffer send_buf_{65536};
    bool done_{false};
    bool saw_preface_{false};

  public:
    explicit H2Conn(bool is_server);
    [[nodiscard]] Result<void> feed(std::span<const unsigned char> data);
    [[nodiscard]] Result<std::vector<uint32_t>> process();
    [[nodiscard]] const std::vector<HpackHeader>& stream_headers(uint32_t stream_id) const;
    [[nodiscard]] std::vector<unsigned char> stream_body(uint32_t stream_id) const;
    void consume_body(uint32_t stream_id, size_t n);
    [[nodiscard]] Result<void> send_response(uint32_t stream_id, std::vector<HpackHeader> headers,
                                             std::span<const unsigned char> body, bool end_stream);
    [[nodiscard]] Result<void> send_window_update(uint32_t stream_id, uint32_t increment);
    [[nodiscard]] std::vector<unsigned char> pending_output() const;
    void consume_output(size_t n);
    [[nodiscard]] Result<void> goaway(H2Error error, std::string_view debug_data);
    [[nodiscard]] bool is_done() const noexcept;

  private:
    [[nodiscard]] Result<void> handle_frame(const H2Frame& frame, std::vector<uint32_t>& ready);
    [[nodiscard]] H2Stream& get_or_create_stream(uint32_t id);
};

} // namespace uproxy
