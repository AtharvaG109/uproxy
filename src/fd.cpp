#include "uproxy/fd.h"

#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace uproxy {

Uniquefd::Uniquefd(int fd) noexcept : fd_(fd) {}

Uniquefd::~Uniquefd() noexcept {
    reset();
}

Uniquefd::Uniquefd(Uniquefd&& other) noexcept : fd_(other.release()) {}

Uniquefd& Uniquefd::operator=(Uniquefd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int Uniquefd::get() const noexcept {
    return fd_;
}

int Uniquefd::release() noexcept {
    const int out = fd_;
    fd_ = -1;
    return out;
}

bool Uniquefd::valid() const noexcept {
    return fd_ >= 0;
}

void Uniquefd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        while (::close(fd_) < 0 && errno == EINTR) {
        }
    }
    fd_ = fd;
}

Result<void> Uniquefd::set_nonblocking() noexcept {
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        return Result<void>::err(Error::from_errno("fcntl(F_GETFL)"));
    }
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        return Result<void>::err(Error::from_errno("fcntl(F_SETFL O_NONBLOCK)"));
    }
    return Result<void>::ok();
}

Result<void> Uniquefd::set_reuse() noexcept {
    int one = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        return Result<void>::err(Error::from_errno("setsockopt(SO_REUSEADDR)"));
    }
#ifdef SO_REUSEPORT
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
        return Result<void>::err(Error::from_errno("setsockopt(SO_REUSEPORT)"));
    }
#endif
    return Result<void>::ok();
}

Result<void> Uniquefd::set_nodelay() noexcept {
    int one = 1;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        return Result<void>::err(Error::from_errno("setsockopt(TCP_NODELAY)"));
    }
    return Result<void>::ok();
}

} // namespace uproxy
