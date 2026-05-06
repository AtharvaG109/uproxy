#include "uproxy/hpack.h"
#include "uproxy/hpack_huffman_table.h"

#include <algorithm>
#include <cctype>

namespace uproxy {

const std::array<HpackHeader, 62> HPACK_STATIC_TABLE = {{
    {"", ""},
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
}};

namespace {

bool has_uppercase(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isupper(c) != 0; });
}

size_t entry_size(const HpackHeader& h) {
    return h.name.size() + h.value.size() + 32U;
}

Result<void> encode_int(uint64_t value, int prefix_bits, unsigned char prefix, RingBuffer& out) {
    const uint64_t max_prefix = (1ULL << prefix_bits) - 1ULL;
    if (value < max_prefix) {
        const unsigned char byte =
            static_cast<unsigned char>(prefix | static_cast<unsigned char>(value));
        return out.append(std::span<const unsigned char>(&byte, 1));
    }
    unsigned char first =
        static_cast<unsigned char>(prefix | static_cast<unsigned char>(max_prefix));
    auto r = out.append(std::span<const unsigned char>(&first, 1));
    if (r.is_err()) {
        return r;
    }
    value -= max_prefix;
    while (value >= 128) {
        unsigned char byte = static_cast<unsigned char>((value % 128U) + 128U);
        r = out.append(std::span<const unsigned char>(&byte, 1));
        if (r.is_err()) {
            return r;
        }
        value /= 128U;
    }
    const unsigned char last = static_cast<unsigned char>(value);
    return out.append(std::span<const unsigned char>(&last, 1));
}

Result<void> encode_string(std::string_view s, RingBuffer& out) {
    size_t huffman_len = 0;
    for (unsigned char c : s) {
        huffman_len += HPACK_HUFFMAN_SYM_TABLE[c].nbits;
    }
    const size_t huffman_bytes = (huffman_len + 7) / 8;
    
    if (huffman_bytes < s.size()) {
        auto r = encode_int(huffman_bytes, 7, 0x80, out);
        if (r.is_err()) return r;
        
        uint64_t bit_buf = 0;
        int bit_len = 0;
        std::vector<unsigned char> enc;
        enc.reserve(huffman_bytes);
        
        for (unsigned char c : s) {
            const auto& sym = HPACK_HUFFMAN_SYM_TABLE[c];
            uint32_t code = sym.code >> (32 - sym.nbits);
            bit_buf = (bit_buf << sym.nbits) | code;
            bit_len += sym.nbits;
            while (bit_len >= 8) {
                bit_len -= 8;
                enc.push_back(static_cast<unsigned char>((bit_buf >> bit_len) & 0xff));
            }
        }
        if (bit_len > 0) {
            bit_buf <<= (8 - bit_len);
            bit_buf |= (1 << (8 - bit_len)) - 1;
            enc.push_back(static_cast<unsigned char>(bit_buf & 0xff));
        }
        return out.append(std::span<const unsigned char>(enc));
    } else {
        auto r = encode_int(s.size(), 7, 0x00, out);
        if (r.is_err()) return r;
        return out.append(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(s.data()), s.size()));
    }
}

} // namespace

Result<void> HpackEncoder::encode(const std::vector<HpackHeader>& headers, RingBuffer& out_buf) {
    for (const auto& h : headers) {
        if (has_uppercase(h.name)) {
            return Result<void>::err(
                Error::from_code(ErrCode::HpackError, "uppercase HTTP/2 header name"));
        }
        const bool sensitive = h.sensitive || h.name == "authorization" || h.name == "cookie" ||
                               h.name == "proxy-authorization";
        const unsigned char literal_prefix = sensitive ? 0x10 : 0x40;
        auto r = encode_int(0, sensitive ? 4 : 6, literal_prefix, out_buf);
        if (r.is_err()) {
            return r;
        }
        r = encode_string(h.name, out_buf);
        if (r.is_err()) {
            return r;
        }
        r = encode_string(h.value, out_buf);
        if (r.is_err()) {
            return r;
        }
        if (!sensitive) {
            dynamic_table_.push_front(h);
            table_size_ += entry_size(h);
            while (table_size_ > max_table_size_ && !dynamic_table_.empty()) {
                table_size_ -= entry_size(dynamic_table_.back());
                dynamic_table_.pop_back();
            }
        }
    }
    return Result<void>::ok();
}

void HpackEncoder::set_max_table_size(size_t n) {
    max_table_size_ = n;
    while (table_size_ > max_table_size_ && !dynamic_table_.empty()) {
        table_size_ -= entry_size(dynamic_table_.back());
        dynamic_table_.pop_back();
    }
}

Result<void> HpackDecoder::decode(std::span<const unsigned char> data,
                                  std::vector<HpackHeader>& out) {
    while (!data.empty()) {
        const unsigned char b = data.front();
        if ((b & 0x80U) != 0U) {
            auto idx = decode_int(data, 7);
            if (idx.is_err()) {
                return Result<void>::err(idx.error());
            }
            const HpackHeader* h = lookup(static_cast<size_t>(idx.value()));
            if (h == nullptr) {
                return Result<void>::err(
                    Error::from_code(ErrCode::HpackError, "invalid HPACK index"));
            }
            out.push_back(*h);
        } else if ((b & 0x20U) != 0U) {
            auto size = decode_int(data, 5);
            if (size.is_err()) {
                return Result<void>::err(size.error());
            }
            if (size.value() > max_table_size_) {
                return Result<void>::err(
                    Error::from_code(ErrCode::HpackError, "dynamic table size update too large"));
            }
            set_max_table_size(static_cast<size_t>(size.value()));
        } else {
            const bool indexed = (b & 0x40U) != 0U;
            const bool never_indexed = (b & 0x10U) != 0U;
            const int prefix = indexed ? 6 : 4;
            auto name_idx = decode_int(data, prefix);
            if (name_idx.is_err()) {
                return Result<void>::err(name_idx.error());
            }
            std::string name;
            if (name_idx.value() == 0) {
                auto decoded_name = decode_string(data);
                if (decoded_name.is_err()) {
                    return Result<void>::err(decoded_name.error());
                }
                name = std::move(decoded_name).value();
            } else {
                const HpackHeader* h = lookup(static_cast<size_t>(name_idx.value()));
                if (h == nullptr) {
                    return Result<void>::err(
                        Error::from_code(ErrCode::HpackError, "invalid HPACK name index"));
                }
                name = h->name;
            }
            auto value = decode_string(data);
            if (value.is_err()) {
                return Result<void>::err(value.error());
            }
            if (has_uppercase(name)) {
                return Result<void>::err(
                    Error::from_code(ErrCode::HpackError, "uppercase HTTP/2 header name"));
            }
            HpackHeader header{name, std::move(value).value(), never_indexed};
            out.push_back(header);
            if (indexed) {
                add_to_dynamic(std::move(header));
            }
        }
    }
    return Result<void>::ok();
}

void HpackDecoder::set_max_table_size(size_t n) {
    max_table_size_ = n;
    while (table_size_ > max_table_size_ && !dynamic_table_.empty()) {
        table_size_ -= entry_size(dynamic_table_.back());
        dynamic_table_.pop_back();
    }
}

Result<uint64_t> HpackDecoder::decode_int(std::span<const unsigned char>& data, int prefix_bits) {
    if (data.empty() || prefix_bits <= 0 || prefix_bits > 8) {
        return Result<uint64_t>::err(
            Error::from_code(ErrCode::HpackError, "truncated HPACK integer"));
    }
    const uint64_t mask = (1ULL << prefix_bits) - 1ULL;
    uint64_t value = data.front() & mask;
    data = data.subspan(1);
    if (value < mask) {
        if (value == 0 && prefix_bits == 7) {
            return Result<uint64_t>::err(Error::from_code(ErrCode::HpackError, "invalid index 0"));
        }
        return Result<uint64_t>::ok(value);
    }
    uint64_t m = 0;
    while (!data.empty()) {
        const unsigned char b = data.front();
        data = data.subspan(1);
        value += static_cast<uint64_t>(b & 0x7FU) << m;
        if ((b & 0x80U) == 0U) {
            return Result<uint64_t>::ok(value);
        }
        m += 7U;
        if (m > 56U) {
            return Result<uint64_t>::err(
                Error::from_code(ErrCode::HpackError, "HPACK integer overflow"));
        }
    }
    return Result<uint64_t>::err(Error::from_code(ErrCode::HpackError, "truncated HPACK integer"));
}

Result<std::string> HpackDecoder::decode_string(std::span<const unsigned char>& data) {
    if (data.empty()) {
        return Result<std::string>::err(
            Error::from_code(ErrCode::HpackError, "truncated HPACK string"));
    }
    const bool huffman = (data.front() & 0x80U) != 0U;
    auto len = decode_int(data, 7);
    if (len.is_err()) {
        return Result<std::string>::err(len.error());
    }
    if (len.value() > 65535 || len.value() > data.size()) {
        return Result<std::string>::err(
            Error::from_code(ErrCode::HpackError, "invalid HPACK string length"));
    }
    if (huffman) {
        struct HuffNode {
            int left{-1};
            int right{-1};
            int sym{-1};
        };
        static std::vector<HuffNode> tree;
        if (tree.empty()) {
            tree.emplace_back();
            for (int i = 0; i < 257; ++i) {
                const auto& sym = HPACK_HUFFMAN_SYM_TABLE[i];
                int node = 0;
                uint32_t code = sym.code >> (32 - sym.nbits);
                for (int b = sym.nbits - 1; b >= 0; --b) {
                    int bit = (code >> b) & 1;
                    if (bit == 0) {
                        if (tree[node].left == -1) {
                            tree[node].left = static_cast<int>(tree.size());
                            tree.emplace_back();
                        }
                        node = tree[node].left;
                    } else {
                        if (tree[node].right == -1) {
                            tree[node].right = static_cast<int>(tree.size());
                            tree.emplace_back();
                        }
                        node = tree[node].right;
                    }
                }
                tree[node].sym = i;
            }
        }
        
        std::string out;
        int node = 0;
        for (size_t i = 0; i < len.value(); ++i) {
            unsigned char b = data[i];
            for (int bit = 7; bit >= 0; --bit) {
                int b_val = (b >> bit) & 1;
                node = (b_val == 0) ? tree[node].left : tree[node].right;
                if (node == -1) {
                    return Result<std::string>::err(Error::from_code(ErrCode::HpackError, "invalid Huffman code"));
                }
                if (tree[node].sym != -1) {
                    if (tree[node].sym == 256) {
                        return Result<std::string>::err(Error::from_code(ErrCode::HpackError, "EOS symbol in Huffman"));
                    }
                    out.push_back(static_cast<char>(tree[node].sym));
                    node = 0;
                }
            }
        }
        data = data.subspan(static_cast<size_t>(len.value()));
        return Result<std::string>::ok(std::move(out));
    }
    std::string out(reinterpret_cast<const char*>(data.data()), static_cast<size_t>(len.value()));
    data = data.subspan(static_cast<size_t>(len.value()));
    return Result<std::string>::ok(std::move(out));
}

const HpackHeader* HpackDecoder::lookup(size_t index) const {
    if (index == 0) {
        return nullptr;
    }
    if (index < HPACK_STATIC_TABLE.size()) {
        return &HPACK_STATIC_TABLE[index];
    }
    const size_t dyn = index - HPACK_STATIC_TABLE.size();
    if (dyn == 0 || dyn > dynamic_table_.size()) {
        return nullptr;
    }
    return &dynamic_table_[dyn - 1];
}

void HpackDecoder::add_to_dynamic(HpackHeader h) {
    table_size_ += entry_size(h);
    dynamic_table_.push_front(std::move(h));
    while (table_size_ > max_table_size_ && !dynamic_table_.empty()) {
        table_size_ -= entry_size(dynamic_table_.back());
        dynamic_table_.pop_back();
    }
}

} // namespace uproxy
