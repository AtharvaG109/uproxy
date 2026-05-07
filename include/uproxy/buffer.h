#pragma once

#include "uproxy/result.h"

#include <span>
#include <string_view>
#include <sys/uio.h>
#include <vector>

namespace uproxy {

class RingBuffer {
    std::vector<unsigned char> data_;
    size_t read_pos_{0};
    size_t write_pos_{0};
    size_t size_{0};

  public:
    explicit RingBuffer(size_t capacity);

    [[nodiscard]] size_t readable() const noexcept;
    [[nodiscard]] size_t writable() const noexcept;
    [[nodiscard]] size_t capacity() const noexcept;

    [[nodiscard]] int read_iovecs(struct iovec out[2]) const noexcept;
    [[nodiscard]] int write_iovecs(struct iovec out[2]) const noexcept;

    void commit_read(size_t n) noexcept;
    void commit_write(size_t n) noexcept;

    [[nodiscard]] std::vector<unsigned char> peek(size_t n) const;
    [[nodiscard]] std::vector<unsigned char> read_bytes(size_t n) const;

    [[nodiscard]] Result<void> ensure_writable(size_t n);
    [[nodiscard]] Result<void> append(std::span<const unsigned char> data);
    [[nodiscard]] Result<void> append(std::string_view s);
};

} // namespace uproxy
