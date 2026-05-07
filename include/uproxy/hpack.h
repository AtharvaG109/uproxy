#pragma once

#include "uproxy/buffer.h"

#include <array>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace uproxy {

struct HpackHeader {
    std::string name;
    std::string value;
    bool sensitive{false};
};

extern const std::array<HpackHeader, 62> HPACK_STATIC_TABLE;

class HpackEncoder {
    std::deque<HpackHeader> dynamic_table_;
    size_t table_size_{0};
    size_t max_table_size_{4096};

  public:
    [[nodiscard]] Result<void> encode(const std::vector<HpackHeader>& headers, RingBuffer& out_buf);
    void set_max_table_size(size_t n);
};

class HpackDecoder {
    std::deque<HpackHeader> dynamic_table_;
    size_t table_size_{0};
    size_t max_table_size_{4096};

  public:
    [[nodiscard]] Result<void> decode(std::span<const unsigned char> data,
                                      std::vector<HpackHeader>& out);
    void set_max_table_size(size_t n);

  private:
    [[nodiscard]] Result<uint64_t> decode_int(std::span<const unsigned char>& data,
                                              int prefix_bits);
    [[nodiscard]] Result<std::string> decode_string(std::span<const unsigned char>& data);
    [[nodiscard]] const HpackHeader* lookup(size_t index) const;
    void add_to_dynamic(HpackHeader h);
};

} // namespace uproxy
