#include "uproxy/proxy.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace uproxy {

namespace {

constexpr size_t MAX_PROXY_RESPONSE_BYTES = 64U * 1024U * 1024U;

bool header_name_eq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

Result<void> wait_fd(int fd, short event, int timeout_ms, std::string_view op) {
    pollfd pfd{fd, event, 0};
    for (;;) {
        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (pfd.revents & event) == 0) {
                return Result<void>::err(Error::from_code(ErrCode::SysError, op));
            }
            return Result<void>::ok();
        }
        if (rc == 0) {
            return Result<void>::err(Error::from_code(ErrCode::Timeout, op));
        }
        if (errno != EINTR) {
            return Result<void>::err(Error::from_errno(op));
        }
    }
}

Result<Uniquefd> connect_upstream(const UpstreamEndpoint& upstream, uint32_t timeout_ms) {
    Uniquefd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid()) {
        return Result<Uniquefd>::err(Error::from_errno("socket upstream"));
    }
    auto nb = fd.set_nonblocking();
    if (nb.is_err()) {
        return Result<Uniquefd>::err(nb.error());
    }
    (void)fd.set_nodelay();

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(upstream.port);
    if (::inet_pton(AF_INET, upstream.addr.c_str(), &addr.sin_addr) != 1) {
        return Result<Uniquefd>::err(
            Error::from_code(ErrCode::ConfigInvalid, "upstream address must be IPv4"));
    }

    const int rc = ::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        return Result<Uniquefd>::ok(std::move(fd));
    }
    if (errno != EINPROGRESS) {
        return Result<Uniquefd>::err(Error::from_errno("connect upstream"));
    }
    auto waited = wait_fd(fd.get(), POLLOUT, static_cast<int>(timeout_ms), "connect upstream");
    if (waited.is_err()) {
        return Result<Uniquefd>::err(waited.error());
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
        return Result<Uniquefd>::err(Error::from_errno("getsockopt(SO_ERROR)"));
    }
    if (so_error != 0) {
        Error e;
        e.code = ErrCode::SysError;
        e.sys_errno = so_error;
        e.msg = "connect upstream";
        return Result<Uniquefd>::err(std::move(e));
    }
    return Result<Uniquefd>::ok(std::move(fd));
}

Result<void> write_all(int fd, std::span<const unsigned char> bytes, uint32_t timeout_ms) {
    size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, 0);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            auto waited = wait_fd(fd, POLLOUT, static_cast<int>(timeout_ms), "write upstream");
            if (waited.is_err()) {
                return waited;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return Result<void>::err(Error::from_errno("write upstream"));
    }
    return Result<void>::ok();
}

bool response_complete(const RingBuffer& response, HttpResponse& parsed) {
    Http1Parser parser;
    const ParseResult status = parser.parse_response(response, parsed);
    if (status != ParseResult::Complete) {
        return false;
    }
    if (parsed.chunked) {
        const auto bytes = response.peek(response.readable());
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return text.find("\r\n0\r\n\r\n", parsed.header_bytes) != std::string_view::npos ||
               text.find("\r\n0;") != std::string_view::npos;
    }
    const bool has_content_length = std::ranges::any_of(parsed.headers, [](const HttpHeader& h) {
        return header_name_eq(h.name, "content-length");
    });
    if (has_content_length) {
        return response.readable() >= parsed.header_bytes + parsed.content_length;
    }
    return parsed.status == 204 || parsed.status == 304;
}

Result<std::vector<unsigned char>> read_response(int fd, uint32_t timeout_ms) {
    RingBuffer response(65536);
    HttpResponse parsed;
    for (;;) {
        if (response_complete(response, parsed)) {
            return Result<std::vector<unsigned char>>::ok(response.peek(response.readable()));
        }
        if (response.readable() >= MAX_PROXY_RESPONSE_BYTES) {
            return Result<std::vector<unsigned char>>::err(
                Error::from_code(ErrCode::HttpMalformed, "upstream response too large"));
        }
        auto waited = wait_fd(fd, POLLIN, static_cast<int>(timeout_ms), "read upstream");
        if (waited.is_err()) {
            if (parsed.header_bytes > 0 && parsed.content_length == 0 && !parsed.chunked) {
                return Result<std::vector<unsigned char>>::ok(response.peek(response.readable()));
            }
            return Result<std::vector<unsigned char>>::err(waited.error());
        }
        unsigned char tmp[16384];
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            auto appended =
                response.append(std::span<const unsigned char>(tmp, static_cast<size_t>(n)));
            if (appended.is_err()) {
                return Result<std::vector<unsigned char>>::err(appended.error());
            }
            continue;
        }
        if (n == 0) {
            return Result<std::vector<unsigned char>>::ok(response.peek(response.readable()));
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }
        return Result<std::vector<unsigned char>>::err(Error::from_errno("read upstream"));
    }
}

std::string upstream_host(const UpstreamEndpoint& upstream) {
    return upstream.addr + ":" + std::to_string(upstream.port);
}

Result<std::vector<unsigned char>> forward_http1(const HttpRequest& req,
                                                 std::span<const unsigned char> body,
                                                 const UpstreamEndpoint& upstream,
                                                 const PoolConfig& pool) {
    RingBuffer upstream_request(65536 + body.size());
    Http1Serializer serializer;
    auto serialized = serializer.write_request(upstream_request, req, upstream_host(upstream));
    if (serialized.is_err()) {
        return Result<std::vector<unsigned char>>::err(serialized.error());
    }
    if (!body.empty()) {
        auto appended = upstream_request.append(body);
        if (appended.is_err()) {
            return Result<std::vector<unsigned char>>::err(appended.error());
        }
    }
    auto connected = connect_upstream(upstream, pool.connect_timeout_ms);
    if (connected.is_err()) {
        return Result<std::vector<unsigned char>>::err(connected.error());
    }
    Uniquefd upstream_fd = std::move(connected).value();
    const auto request_bytes = upstream_request.peek(upstream_request.readable());
    auto wrote = write_all(upstream_fd.get(), request_bytes, pool.connect_timeout_ms);
    if (wrote.is_err()) {
        return Result<std::vector<unsigned char>>::err(wrote.error());
    }
    return read_response(upstream_fd.get(), pool.connect_timeout_ms);
}

} // namespace

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
            const size_t buffered_body = conn.rbuf.readable() > req.header_bytes
                                             ? conn.rbuf.readable() - req.header_bytes
                                             : 0;
            if (req.content_length > buffered_body) {
                return;
            }
            conn.rbuf.commit_read(req.header_bytes);
            const auto body = conn.rbuf.peek(static_cast<size_t>(req.content_length));
            conn.rbuf.commit_read(static_cast<size_t>(req.content_length));
            auto upstream = lb_.next();
            if (upstream == nullptr) {
                static constexpr std::string_view unavailable =
                    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: "
                    "close\r\n\r\n";
                (void)conn.wbuf.append(unavailable);
                conn.keep_alive = false;
            } else {
                conn.upstream = upstream;
                auto response = forward_http1(req, body, *upstream, cfg_.pool);
                if (response.is_err()) {
                    upstream->failed_requests.fetch_add(1);
                    LOG_WARN("upstream request failed", "upstream", upstream->name, "error",
                             response.error().to_string());
                    static constexpr std::string_view bad_gateway =
                        "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: "
                        "close\r\n\r\n";
                    (void)conn.wbuf.append(bad_gateway);
                } else {
                    (void)conn.wbuf.append(std::move(response).value());
                }
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
