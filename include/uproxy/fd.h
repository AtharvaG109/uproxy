#pragma once

#include "uproxy/result.h"

namespace uproxy {

class Uniquefd {
    int fd_{-1};

  public:
    explicit Uniquefd(int fd) noexcept;
    Uniquefd() noexcept = default;
    ~Uniquefd() noexcept;

    Uniquefd(Uniquefd&& other) noexcept;
    Uniquefd& operator=(Uniquefd&& other) noexcept;
    Uniquefd(const Uniquefd&) = delete;
    Uniquefd& operator=(const Uniquefd&) = delete;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] int release() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    void reset(int fd = -1) noexcept;

    [[nodiscard]] Result<void> set_nonblocking() noexcept;
    [[nodiscard]] Result<void> set_reuse() noexcept;
    [[nodiscard]] Result<void> set_nodelay() noexcept;
};

} // namespace uproxy
