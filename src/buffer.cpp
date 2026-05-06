#include "uproxy/buffer.h"

#include <algorithm>
#include <cstring>

namespace uproxy {

RingBuffer::RingBuffer(size_t capacity) : data_(capacity) {}

size_t RingBuffer::readable() const noexcept {
    return size_;
}

size_t RingBuffer::writable() const noexcept {
    return data_.size() - size_;
}

size_t RingBuffer::capacity() const noexcept {
    return data_.size();
}

int RingBuffer::read_iovecs(struct iovec out[2]) const noexcept {
    if (size_ == 0 || data_.empty()) {
        out[0] = {nullptr, 0};
        out[1] = {nullptr, 0};
        return 0;
    }
    const size_t first = std::min(size_, data_.size() - read_pos_);
    out[0] = {const_cast<unsigned char*>(data_.data() + read_pos_), first};
    const size_t second = size_ - first;
    out[1] = {const_cast<unsigned char*>(data_.data()), second};
    return second == 0 ? 1 : 2;
}

int RingBuffer::write_iovecs(struct iovec out[2]) const noexcept {
    const size_t avail = writable();
    if (avail == 0 || data_.empty()) {
        out[0] = {nullptr, 0};
        out[1] = {nullptr, 0};
        return 0;
    }
    const size_t first = std::min(avail, data_.size() - write_pos_);
    out[0] = {const_cast<unsigned char*>(data_.data() + write_pos_), first};
    const size_t second = avail - first;
    out[1] = {const_cast<unsigned char*>(data_.data()), second};
    return second == 0 ? 1 : 2;
}

void RingBuffer::commit_read(size_t n) noexcept {
    n = std::min(n, size_);
    read_pos_ = data_.empty() ? 0 : (read_pos_ + n) % data_.size();
    size_ -= n;
    if (size_ == 0) {
        read_pos_ = write_pos_;
    }
}

void RingBuffer::commit_write(size_t n) noexcept {
    n = std::min(n, writable());
    write_pos_ = data_.empty() ? 0 : (write_pos_ + n) % data_.size();
    size_ += n;
}

std::vector<unsigned char> RingBuffer::peek(size_t n) const {
    n = std::min(n, size_);
    std::vector<unsigned char> out(n);
    if (n == 0) {
        return out;
    }
    const size_t first = std::min(n, data_.size() - read_pos_);
    std::memcpy(out.data(), data_.data() + read_pos_, first);
    if (n > first) {
        std::memcpy(out.data() + first, data_.data(), n - first);
    }
    return out;
}

std::vector<unsigned char> RingBuffer::read_bytes(size_t n) const {
    return peek(n);
}

Result<void> RingBuffer::ensure_writable(size_t n) {
    if (writable() >= n) {
        return Result<void>::ok();
    }
    size_t new_cap = std::max<size_t>(data_.size() * 2U, data_.size() + n);
    if (new_cap == 0) {
        new_cap = n;
    }
    if (new_cap < size_) {
        return Result<void>::err(Error::from_code(ErrCode::ConfigInvalid, "ring buffer overflow"));
    }
    auto existing = peek(size_);
    std::vector<unsigned char> replacement(new_cap);
    std::copy(existing.begin(), existing.end(), replacement.begin());
    data_ = std::move(replacement);
    read_pos_ = 0;
    size_ = existing.size();
    write_pos_ = size_ == data_.size() ? 0 : size_;
    return Result<void>::ok();
}

Result<void> RingBuffer::append(std::span<const unsigned char> data) {
    auto ensured = ensure_writable(data.size());
    if (ensured.is_err()) {
        return ensured;
    }
    size_t copied = 0;
    while (copied < data.size()) {
        struct iovec iov[2]{};
        const int count = write_iovecs(iov);
        if (count == 0) {
            return Result<void>::err(Error::from_code(ErrCode::ConfigInvalid, "ring buffer full"));
        }
        for (int i = 0; i < count && copied < data.size(); ++i) {
            const size_t n = std::min(iov[i].iov_len, data.size() - copied);
            std::memcpy(iov[i].iov_base, data.data() + copied, n);
            commit_write(n);
            copied += n;
        }
    }
    return Result<void>::ok();
}

Result<void> RingBuffer::append(std::string_view s) {
    const auto* ptr = reinterpret_cast<const unsigned char*>(s.data());
    return append(std::span<const unsigned char>(ptr, s.size()));
}

} // namespace uproxy
