#include "uproxy/http2.h"

#include <algorithm>
#include <cstring>

namespace uproxy {

namespace {

constexpr std::string_view H2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

unsigned char byte(uint32_t v, int shift) {
    return static_cast<unsigned char>((v >> shift) & 0xffU);
}

Result<void> write_frame_header(RingBuffer& buf, uint32_t length, H2FrameType type, uint8_t flags,
                                uint32_t stream_id) {
    if (length > 0x00ffffffU || (stream_id & 0x80000000U) != 0U) {
        return Result<void>::err(Error::from_code(ErrCode::Http2Protocol, "invalid frame header"));
    }
    unsigned char header[9] = {byte(length, 16),
                               byte(length, 8),
                               byte(length, 0),
                               static_cast<unsigned char>(type),
                               flags,
                               byte(stream_id, 24),
                               byte(stream_id, 16),
                               byte(stream_id, 8),
                               byte(stream_id, 0)};
    return buf.append(std::span<const unsigned char>(header, sizeof(header)));
}

Result<void> append_u32(RingBuffer& buf, uint32_t v) {
    unsigned char b[4] = {byte(v, 24), byte(v, 16), byte(v, 8), byte(v, 0)};
    return buf.append(std::span<const unsigned char>(b, sizeof(b)));
}

} // namespace

ParseResult H2FrameReader::read_frame(RingBuffer& buf, H2Frame& frame, uint32_t max_frame_size) {
    if (buf.readable() < FRAME_HEADER_SIZE) {
        return ParseResult::Incomplete;
    }
    auto h = buf.peek(FRAME_HEADER_SIZE);
    const uint32_t len = (static_cast<uint32_t>(h[0]) << 16U) |
                         (static_cast<uint32_t>(h[1]) << 8U) | static_cast<uint32_t>(h[2]);
    if (len > max_frame_size) {
        return ParseResult::Error;
    }
    if (buf.readable() < FRAME_HEADER_SIZE + len) {
        return ParseResult::Incomplete;
    }
    frame.length = len;
    frame.type = static_cast<H2FrameType>(h[3]);
    frame.flags = h[4];
    frame.stream_id = ((static_cast<uint32_t>(h[5]) << 24U) | (static_cast<uint32_t>(h[6]) << 16U) |
                       (static_cast<uint32_t>(h[7]) << 8U) | static_cast<uint32_t>(h[8])) &
                      0x7fffffffU;
    buf.commit_read(FRAME_HEADER_SIZE);
    frame.payload = buf.peek(len);
    buf.commit_read(len);
    return ParseResult::Complete;
}

Result<void>
H2FrameWriter::write_settings(RingBuffer& buf,
                              const std::vector<std::pair<H2Setting, uint32_t>>& settings) {
    auto r = write_frame_header(buf, static_cast<uint32_t>(settings.size() * 6U),
                                H2FrameType::SETTINGS, 0, 0);
    if (r.is_err()) {
        return r;
    }
    for (const auto& [setting, value] : settings) {
        unsigned char pair[6] = {byte(static_cast<uint32_t>(setting), 8),
                                 byte(static_cast<uint32_t>(setting), 0),
                                 byte(value, 24),
                                 byte(value, 16),
                                 byte(value, 8),
                                 byte(value, 0)};
        r = buf.append(std::span<const unsigned char>(pair, sizeof(pair)));
        if (r.is_err()) {
            return r;
        }
    }
    return Result<void>::ok();
}

Result<void> H2FrameWriter::write_settings_ack(RingBuffer& buf) {
    return write_frame_header(buf, 0, H2FrameType::SETTINGS, H2Flags::ACK, 0);
}

Result<void> H2FrameWriter::write_ping(RingBuffer& buf, uint64_t data, bool ack) {
    auto r = write_frame_header(buf, 8, H2FrameType::PING, ack ? H2Flags::ACK : 0, 0);
    if (r.is_err()) {
        return r;
    }
    unsigned char payload[8];
    for (int i = 7; i >= 0; --i) {
        payload[7 - i] = static_cast<unsigned char>((data >> (i * 8)) & 0xffU);
    }
    return buf.append(std::span<const unsigned char>(payload, sizeof(payload)));
}

Result<void> H2FrameWriter::write_window_update(RingBuffer& buf, uint32_t stream_id,
                                                uint32_t increment) {
    if (increment == 0 || increment > 0x7fffffffU) {
        return Result<void>::err(
            Error::from_code(ErrCode::Http2Protocol, "invalid window increment"));
    }
    auto r = write_frame_header(buf, 4, H2FrameType::WINDOW_UPDATE, 0, stream_id);
    if (r.is_err()) {
        return r;
    }
    return append_u32(buf, increment);
}

Result<void> H2FrameWriter::write_goaway(RingBuffer& buf, uint32_t last_stream_id, H2Error error,
                                         std::string_view debug_data) {
    auto r = write_frame_header(buf, static_cast<uint32_t>(8 + debug_data.size()),
                                H2FrameType::GOAWAY, 0, 0);
    if (r.is_err()) {
        return r;
    }
    r = append_u32(buf, last_stream_id & 0x7fffffffU);
    if (r.is_err()) {
        return r;
    }
    r = append_u32(buf, static_cast<uint32_t>(error));
    if (r.is_err()) {
        return r;
    }
    return buf.append(debug_data);
}

Result<void> H2FrameWriter::write_rst_stream(RingBuffer& buf, uint32_t stream_id, H2Error error) {
    if (stream_id == 0) {
        return Result<void>::err(
            Error::from_code(ErrCode::Http2Protocol, "RST_STREAM on stream 0"));
    }
    auto r = write_frame_header(buf, 4, H2FrameType::RST_STREAM, 0, stream_id);
    if (r.is_err()) {
        return r;
    }
    return append_u32(buf, static_cast<uint32_t>(error));
}

Result<void> H2FrameWriter::write_headers(RingBuffer& buf, uint32_t stream_id,
                                          std::span<const unsigned char> header_block,
                                          bool end_stream, uint32_t max_frame_size) {
    if (stream_id == 0 || header_block.size() > max_frame_size) {
        return Result<void>::err(Error::from_code(ErrCode::Http2Protocol, "invalid HEADERS frame"));
    }
    const uint8_t flags = H2Flags::END_HEADERS | (end_stream ? H2Flags::END_STREAM : 0);
    auto r = write_frame_header(buf, static_cast<uint32_t>(header_block.size()),
                                H2FrameType::HEADERS, flags, stream_id);
    if (r.is_err()) {
        return r;
    }
    return buf.append(header_block);
}

Result<void> H2FrameWriter::write_data(RingBuffer& buf, uint32_t stream_id,
                                       std::span<const unsigned char> data, bool end_stream,
                                       uint32_t max_frame_size) {
    if (stream_id == 0 || max_frame_size == 0) {
        return Result<void>::err(Error::from_code(ErrCode::Http2Protocol, "invalid DATA frame"));
    }
    size_t offset = 0;
    while (offset < data.size()) {
        const size_t chunk = std::min<size_t>(max_frame_size, data.size() - offset);
        const bool final_chunk = end_stream && offset + chunk == data.size();
        auto r = write_frame_header(buf, static_cast<uint32_t>(chunk), H2FrameType::DATA,
                                    final_chunk ? H2Flags::END_STREAM : 0, stream_id);
        if (r.is_err()) {
            return r;
        }
        r = buf.append(data.subspan(offset, chunk));
        if (r.is_err()) {
            return r;
        }
        offset += chunk;
    }
    return Result<void>::ok();
}

H2Conn::H2Conn(bool is_server) : is_server_(is_server) {
    if (!is_server_) {
        (void)send_buf_.append(H2_PREFACE);
    }
}

Result<void> H2Conn::feed(std::span<const unsigned char> data) {
    return recv_buf_.append(data);
}

Result<std::vector<uint32_t>> H2Conn::process() {
    std::vector<uint32_t> ready;
    if (is_server_ && !saw_preface_) {
        if (recv_buf_.readable() < H2_PREFACE.size()) {
            return Result<std::vector<uint32_t>>::ok(ready);
        }
        auto bytes = recv_buf_.peek(H2_PREFACE.size());
        std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (sv != H2_PREFACE) {
            done_ = true;
            return Result<std::vector<uint32_t>>::err(
                Error::from_code(ErrCode::Http2Protocol, "invalid HTTP/2 preface"));
        }
        recv_buf_.commit_read(H2_PREFACE.size());
        saw_preface_ = true;
        auto r = writer_.write_settings(send_buf_, {});
        if (r.is_err()) {
            return Result<std::vector<uint32_t>>::err(r.error());
        }
    }
    for (;;) {
        H2Frame frame;
        const ParseResult parsed = reader_.read_frame(recv_buf_, frame, max_frame_size_);
        if (parsed == ParseResult::Incomplete) {
            break;
        }
        if (parsed == ParseResult::Error) {
            done_ = true;
            return Result<std::vector<uint32_t>>::err(
                Error::from_code(ErrCode::Http2Protocol, "invalid HTTP/2 frame"));
        }
        auto handled = handle_frame(frame, ready);
        if (handled.is_err()) {
            return Result<std::vector<uint32_t>>::err(handled.error());
        }
    }
    return Result<std::vector<uint32_t>>::ok(std::move(ready));
}

const std::vector<HpackHeader>& H2Conn::stream_headers(uint32_t stream_id) const {
    return streams_.at(stream_id).request_headers;
}

std::vector<unsigned char> H2Conn::stream_body(uint32_t stream_id) const {
    const auto& stream = streams_.at(stream_id);
    return stream.recv_buf.peek(stream.recv_buf.readable());
}

void H2Conn::consume_body(uint32_t stream_id, size_t n) {
    streams_.at(stream_id).recv_buf.commit_read(n);
}

Result<void> H2Conn::send_response(uint32_t stream_id, std::vector<HpackHeader> headers,
                                   std::span<const unsigned char> body, bool end_stream) {
    RingBuffer encoded(4096);
    auto r = hpack_enc_.encode(headers, encoded);
    if (r.is_err()) {
        return r;
    }
    auto block = encoded.peek(encoded.readable());
    r = writer_.write_headers(send_buf_, stream_id, block, body.empty() && end_stream,
                              max_frame_size_);
    if (r.is_err()) {
        return r;
    }
    if (!body.empty()) {
        return writer_.write_data(send_buf_, stream_id, body, end_stream, max_frame_size_);
    }
    return Result<void>::ok();
}

Result<void> H2Conn::send_window_update(uint32_t stream_id, uint32_t increment) {
    return writer_.write_window_update(send_buf_, stream_id, increment);
}

std::vector<unsigned char> H2Conn::pending_output() const {
    return send_buf_.peek(send_buf_.readable());
}

void H2Conn::consume_output(size_t n) {
    send_buf_.commit_read(n);
}

Result<void> H2Conn::goaway(H2Error error, std::string_view debug_data) {
    done_ = true;
    return writer_.write_goaway(send_buf_, last_peer_stream_, error, debug_data);
}

bool H2Conn::is_done() const noexcept {
    return done_;
}

Result<void> H2Conn::handle_frame(const H2Frame& frame, std::vector<uint32_t>& ready) {
    if (frame.type == H2FrameType::SETTINGS) {
        if (frame.stream_id != 0 || frame.length % 6U != 0U) {
            return Result<void>::err(Error::from_code(ErrCode::Http2Protocol, "bad SETTINGS"));
        }
        if ((frame.flags & H2Flags::ACK) == 0U) {
            return writer_.write_settings_ack(send_buf_);
        }
        return Result<void>::ok();
    }
    if (frame.type == H2FrameType::PING) {
        if (frame.stream_id != 0 || frame.length != 8) {
            return Result<void>::err(Error::from_code(ErrCode::Http2Protocol, "bad PING"));
        }
        uint64_t data = 0;
        for (unsigned char b : frame.payload) {
            data = (data << 8U) | b;
        }
        if ((frame.flags & H2Flags::ACK) == 0U) {
            return writer_.write_ping(send_buf_, data, true);
        }
        return Result<void>::ok();
    }
    if (frame.type == H2FrameType::GOAWAY) {
        done_ = true;
        return Result<void>::ok();
    }
    if (frame.stream_id == 0) {
        return Result<void>::err(
            Error::from_code(ErrCode::Http2Protocol, "stream frame on stream 0"));
    }
    last_peer_stream_ = std::max(last_peer_stream_, frame.stream_id);
    auto& stream = get_or_create_stream(frame.stream_id);
    if (frame.type == H2FrameType::HEADERS) {
        if (stream.state != H2StreamState::Idle && stream.state != H2StreamState::Open &&
            stream.state != H2StreamState::HalfClosedLocal) {
            return writer_.write_rst_stream(send_buf_, frame.stream_id, H2Error::STREAM_CLOSED);
        }
        if ((frame.flags & H2Flags::END_HEADERS) == 0U) {
            return Result<void>::err(Error::from_code(ErrCode::Http2Protocol,
                                                      "CONTINUATION unsupported in simple block"));
        }
        auto decoded = hpack_dec_.decode(frame.payload, stream.request_headers);
        if (decoded.is_err()) {
            return decoded;
        }
        if (stream.state == H2StreamState::Idle) {
            stream.state = (frame.flags & H2Flags::END_STREAM) != 0U
                               ? H2StreamState::HalfClosedRemote
                               : H2StreamState::Open;
        } else if (stream.state == H2StreamState::Open &&
                   (frame.flags & H2Flags::END_STREAM) != 0U) {
            stream.state = H2StreamState::HalfClosedRemote;
        } else if (stream.state == H2StreamState::HalfClosedLocal &&
                   (frame.flags & H2Flags::END_STREAM) != 0U) {
            stream.state = H2StreamState::Closed;
        }
        if ((frame.flags & H2Flags::END_STREAM) != 0U) {
            ready.push_back(frame.stream_id);
        }
        return Result<void>::ok();
    }
    if (frame.type == H2FrameType::DATA) {
        if (stream.state == H2StreamState::Idle) {
            return Result<void>::err(
                Error::from_code(ErrCode::Http2Protocol, "DATA on idle stream"));
        }
        if (stream.state == H2StreamState::HalfClosedRemote ||
            stream.state == H2StreamState::Closed) {
            return writer_.write_rst_stream(send_buf_, frame.stream_id, H2Error::STREAM_CLOSED);
        }
        if (stream.state != H2StreamState::Open && stream.state != H2StreamState::HalfClosedLocal) {
            return Result<void>::err(
                Error::from_code(ErrCode::Http2Protocol, "DATA on closed stream"));
        }
        auto r = stream.recv_buf.append(frame.payload);
        if (r.is_err()) {
            return r;
        }
        if ((frame.flags & H2Flags::END_STREAM) != 0U) {
            if (stream.state == H2StreamState::Open) {
                stream.state = H2StreamState::HalfClosedRemote;
            } else if (stream.state == H2StreamState::HalfClosedLocal) {
                stream.state = H2StreamState::Closed;
            }
            ready.push_back(frame.stream_id);
        }
        return Result<void>::ok();
    }
    if (frame.type == H2FrameType::WINDOW_UPDATE) {
        if (frame.length != 4 || frame.payload.size() != 4) {
            return Result<void>::err(Error::from_code(ErrCode::Http2Protocol, "bad WINDOW_UPDATE"));
        }
        const uint32_t inc = ((static_cast<uint32_t>(frame.payload[0]) << 24U) |
                              (static_cast<uint32_t>(frame.payload[1]) << 16U) |
                              (static_cast<uint32_t>(frame.payload[2]) << 8U) | frame.payload[3]) &
                             0x7fffffffU;
        if (inc == 0) {
            return Result<void>::err(
                Error::from_code(ErrCode::Http2Protocol, "zero WINDOW_UPDATE"));
        }
        stream.send_window += static_cast<int32_t>(inc);
    }
    return Result<void>::ok();
}

H2Stream& H2Conn::get_or_create_stream(uint32_t id) {
    auto [it, _] = streams_.try_emplace(id);
    it->second.id = id;
    return it->second;
}

} // namespace uproxy
