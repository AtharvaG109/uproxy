#include "uproxy/proxy.h"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace uproxy {

std::atomic<bool> ProxyServer::g_shutdown{false};

ProxyServer::ProxyServer(ProxyConfig cfg)
    : cfg_(std::move(cfg)), loop_(EventLoop::create()), tls_ctx_(), lb_(cfg_.upstreams),
      pools_(cfg_.pool) {}

Result<void> ProxyServer::run() {
    if (cfg_.tls.enabled) {
        auto tls = TLSContext::server(cfg_.tls);
        if (tls.is_err()) {
            return Result<void>::err(tls.error());
        }
        tls_ctx_ = std::move(tls).value();
    }
    auto setup = setup_listeners();
    if (setup.is_err()) {
        return setup;
    }
    auto timer = loop_->add_timer(1000, [this] { on_tick(); });
    if (timer.is_err()) {
        return Result<void>::err(timer.error());
    }
    while (!g_shutdown.load()) {
        auto polled = loop_->poll(1000, [this](const FiredEvent& ev) {
            if (ev.fd == listen_fd_.get()) {
                on_accept(ev.fd, false);
            } else if (ev.fd == tls_listen_fd_.get()) {
                on_accept(ev.fd, true);
            } else {
                on_client_event(ev.fd, ev.events);
            }
        });
        if (polled.is_err()) {
            return polled;
        }
    }
    for (auto it = conns_.begin(); it != conns_.end();) {
        close_conn(it++->first, "shutdown");
    }
    return Result<void>::ok();
}

Result<void> ProxyServer::setup_listeners() {
    listen_fd_.reset(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listen_fd_.valid()) {
        return Result<void>::err(Error::from_errno("socket listen"));
    }
    auto reuse = listen_fd_.set_reuse();
    if (reuse.is_err()) {
        return reuse;
    }
    auto nb = listen_fd_.set_nonblocking();
    if (nb.is_err()) {
        return nb;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.listen_port);
    if (::inet_pton(AF_INET, cfg_.listen_addr.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (::bind(listen_fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return Result<void>::err(Error::from_errno("bind listen"));
    }
    if (::listen(listen_fd_.get(), SOMAXCONN) < 0) {
        return Result<void>::err(Error::from_errno("listen"));
    }
    return loop_->add(listen_fd_.get(), Event::Read, nullptr);
}

void ProxyServer::on_accept(int listen_fd, bool tls) {
    for (;;) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        Uniquefd fd(::accept(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len));
        if (!fd.valid()) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_WARN("accept failed", "error", std::strerror(errno));
            break;
        }
        (void)fd.set_nonblocking();
        auto conn = std::make_unique<ClientConn>();
        conn->fd = std::move(fd);
        conn->state = tls ? ConnState::TLSHandshake : ConnState::ReadRequest;
        conn->connected_at_ms = now_ms();
        char remote[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &addr.sin_addr, remote, sizeof(remote)) != nullptr) {
            conn->remote_addr = remote;
        }
        const int client_fd = conn->fd.get();
        auto added = loop_->add(client_fd, Event::Read, conn.get());
        if (added.is_err()) {
            LOG_WARN("event add failed", "error", added.error().to_string());
            continue;
        }
        conns_.emplace(client_fd, std::move(conn));
    }
}

void ProxyServer::on_client_event(int fd, Event events) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    if (has(events, Event::HangUp) || has(events, Event::Error)) {
        close_conn(fd, "peer closed");
        return;
    }
    auto& conn = *it->second;
    if (has(events, Event::Read)) {
        unsigned char tmp[8192];
        for (;;) {
            const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                auto appended =
                    conn.rbuf.append(std::span<const unsigned char>(tmp, static_cast<size_t>(n)));
                if (appended.is_err()) {
                    close_conn(fd, "read buffer overflow");
                    return;
                }
                continue;
            }
            if (n == 0) {
                close_conn(fd, "eof");
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            close_conn(fd, "recv error");
            return;
        }
        HttpRequest req;
        const ParseResult parsed = conn.h1parser.parse_request(conn.rbuf, req);
        if (parsed == ParseResult::Error) {
            close_conn(fd, "bad request");
            return;
        }
        if (parsed == ParseResult::Complete) {
            conn.rbuf.commit_read(req.header_bytes);
            auto upstream = lb_.next();
            if (upstream == nullptr) {
                static constexpr std::string_view unavailable =
                    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: "
                    "close\r\n\r\n";
                (void)conn.wbuf.append(unavailable);
                conn.keep_alive = false;
            } else {
                conn.upstream = upstream;
                std::string body =
                    "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                (void)conn.wbuf.append(body);
                conn.keep_alive = false;
            }
            (void)loop_->modify(fd, Event::Read | Event::Write, &conn);
        }
    }
    if (has(events, Event::Write)) {
        while (conn.wbuf.readable() > 0) {
            const auto bytes = conn.wbuf.peek(conn.wbuf.readable());
            const ssize_t n = ::send(fd, bytes.data(), bytes.size(), 0);
            if (n > 0) {
                conn.wbuf.commit_read(static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            close_conn(fd, "send error");
            return;
        }
        if (!conn.keep_alive) {
            close_conn(fd, "response complete");
        } else {
            (void)loop_->modify(fd, Event::Read, &conn);
        }
    }
}

void ProxyServer::close_conn(int fd, std::string_view reason) {
    (void)loop_->remove(fd);
    auto it = conns_.find(fd);
    if (it != conns_.end()) {
        LOG_INFO("connection closed", "fd", fd, "reason", reason);
        conns_.erase(it);
    }
}

uint64_t ProxyServer::now_ms() const noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void ProxyServer::on_tick() {
    pools_.tick(now_ms());
    if (!g_shutdown.load()) {
        (void)loop_->add_timer(1000, [this] { on_tick(); });
    }
}

} // namespace uproxy
