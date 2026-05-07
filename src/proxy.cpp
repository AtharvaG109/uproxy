#include "uproxy/proxy.h"
#include "uproxy/http2.h"

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
                                                 std::shared_ptr<UpstreamEndpoint> upstream,
                                                 PoolManager& pools,
                                                 EventLoop& loop) {
    RingBuffer upstream_request(65536 + body.size());
    Http1Serializer serializer;
    auto serialized = serializer.write_request(upstream_request, req, upstream_host(*upstream));
    if (serialized.is_err()) {
        return Result<std::vector<unsigned char>>::err(serialized.error());
    }
    if (!body.empty()) {
        auto appended = upstream_request.append(body);
        if (appended.is_err()) {
            return Result<std::vector<unsigned char>>::err(appended.error());
        }
    }

    auto& up_pool = pools.get(upstream);
    const auto request_bytes = upstream_request.peek(upstream_request.readable());

    // Retry once on connection failure (handles stale pooled connections)
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto pool_res = up_pool.acquire(loop);
        if (pool_res.is_err()) {
            return Result<std::vector<unsigned char>>::err(pool_res.error());
        }
        auto conn = std::move(pool_res.value());
        auto wrote = write_all(conn->fd.get(), request_bytes, 5000);
        if (wrote.is_err()) {
            up_pool.release(nullptr);
            if (attempt == 0) continue; // retry with fresh connection
            return Result<std::vector<unsigned char>>::err(wrote.error());
        }
        auto response = read_response(conn->fd.get(), 5000);
        if (response.is_err()) {
            up_pool.release(nullptr);
            if (attempt == 0) continue; // retry with fresh connection
            return response;
        }
        conn->request_count++;
        up_pool.release(std::move(conn));
        return response;
    }
    return Result<std::vector<unsigned char>>::err(
        Error::from_code(ErrCode::NoUpstream, "upstream request failed after retry"));
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
    // Plain HTTP listener
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
        LOG_ERROR("invalid listen_addr, inet_pton failed", "addr", cfg_.listen_addr);
        return Result<void>::err(
            Error::from_code(ErrCode::SysError, "invalid listen address: " + cfg_.listen_addr));
    }
    if (::bind(listen_fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return Result<void>::err(Error::from_errno("bind listen"));
    }
    if (::listen(listen_fd_.get(), SOMAXCONN) < 0) {
        return Result<void>::err(Error::from_errno("listen"));
    }
    auto added = loop_->add(listen_fd_.get(), Event::Read, nullptr);
    if (added.is_err()) {
        return added;
    }

    // TLS listener (if enabled)
    if (cfg_.tls.enabled) {
        tls_listen_fd_.reset(::socket(AF_INET, SOCK_STREAM, 0));
        if (!tls_listen_fd_.valid()) {
            return Result<void>::err(Error::from_errno("socket tls listen"));
        }
        auto tls_reuse = tls_listen_fd_.set_reuse();
        if (tls_reuse.is_err()) {
            return tls_reuse;
        }
        auto tls_nb = tls_listen_fd_.set_nonblocking();
        if (tls_nb.is_err()) {
            return tls_nb;
        }
        sockaddr_in tls_addr{};
        tls_addr.sin_family = AF_INET;
        tls_addr.sin_port = htons(cfg_.tls_port);
        if (::inet_pton(AF_INET, cfg_.listen_addr.c_str(), &tls_addr.sin_addr) != 1) {
            LOG_ERROR("invalid listen_addr for TLS, inet_pton failed", "addr", cfg_.listen_addr);
            return Result<void>::err(
                Error::from_code(ErrCode::SysError, "invalid TLS listen address: " + cfg_.listen_addr));
        }
        if (::bind(tls_listen_fd_.get(), reinterpret_cast<sockaddr*>(&tls_addr), sizeof(tls_addr)) < 0) {
            return Result<void>::err(Error::from_errno("bind tls listen"));
        }
        if (::listen(tls_listen_fd_.get(), SOMAXCONN) < 0) {
            return Result<void>::err(Error::from_errno("listen tls"));
        }
        auto tls_added = loop_->add(tls_listen_fd_.get(), Event::Read, nullptr);
        if (tls_added.is_err()) {
            return tls_added;
        }
    }

    return Result<void>::ok();
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
        if (tls) {
            conn->state = ConnState::TLSHandshake;
            conn->tls_conn = std::make_unique<TLSConn>(tls_ctx_.get(), true);
        } else {
            conn->state = ConnState::ReadRequest;
        }
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
    
    if (conn.state == ConnState::TLSHandshake) {
        if (has(events, Event::Read)) {
            unsigned char tmp[8192];
            for (;;) {
                const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
                if (n > 0) {
                    (void)conn.tls_conn->feed_encrypted(std::span<const unsigned char>(tmp, static_cast<size_t>(n)));
                } else if (n == 0) {
                    close_conn(fd, "eof in handshake");
                    return;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    close_conn(fd, "recv error in handshake");
                    return;
                }
            }
        }
        auto state_res = conn.tls_conn->do_handshake();
        unsigned char out_tmp[8192];
        for (;;) {
            auto out_res = conn.tls_conn->take_encrypted(out_tmp);
            if (out_res.is_err() || out_res.value() == 0) break;
            size_t off = 0;
            while (off < out_res.value()) {
                ssize_t sent = ::send(fd, out_tmp + off, out_res.value() - off, 0);
                if (sent > 0) {
                    off += static_cast<size_t>(sent);
                } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // For simplicity, we assume we can write everything in the handshake.
                    break;
                } else {
                    close_conn(fd, "send error in handshake");
                    return;
                }
            }
        }
        if (state_res.is_err()) {
            close_conn(fd, "tls handshake error: " + state_res.error().to_string());
            return;
        }
        if (state_res.value() == TLSHandshakeState::Done) {
            conn.state = ConnState::ReadRequest;
            if (conn.tls_conn->alpn_protocol() == "h2") {
                conn.proto = ClientConn::Proto::HTTP2;
            } else {
                conn.proto = ClientConn::Proto::HTTP1;
            }
            (void)loop_->modify(fd, Event::Read | Event::Write, &conn);
        }
        return; // Handshake takes precedence
    }
    
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

        if (conn.proto == ClientConn::Proto::HTTP2) {
            handle_h2_client(conn, fd);
            return;
        }

        // HTTP/1.1 path
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
            auto body_vec = conn.rbuf.peek(static_cast<size_t>(req.content_length));
            conn.rbuf.commit_read(static_cast<size_t>(req.content_length));

            conn.keep_alive = req.keep_alive;

            auto upstream = lb_.next();
            if (upstream == nullptr) {
                static constexpr std::string_view unavailable =
                    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: "
                    "close\r\n\r\n";
                (void)conn.wbuf.append(unavailable);
                conn.keep_alive = false;
            } else {
                conn.upstream = upstream;
                auto response = forward_http1(req, body_vec, upstream, pools_, *loop_);
                if (response.is_err()) {
                    upstream->failed_requests.fetch_add(1);
                    LOG_WARN("upstream request failed", "upstream", upstream->name, "error",
                             response.error().to_string());
                    static constexpr std::string_view bad_gateway =
                        "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: "
                        "close\r\n\r\n";
                    (void)conn.wbuf.append(bad_gateway);
                    conn.keep_alive = false;
                } else {
                    (void)conn.wbuf.append(std::move(response).value());
                }
            }
            conn.req_count++;
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

void ProxyServer::handle_h2_client(ClientConn& conn, int fd) {
    // Initialize H2 connection on first use
    if (!conn.h2conn) {
        conn.h2conn = std::make_unique<H2Conn>(true /* is_server */);
    }

    // Feed buffered data into H2 connection
    if (conn.rbuf.readable() > 0) {
        auto data = conn.rbuf.peek(conn.rbuf.readable());
        auto fed = conn.h2conn->feed(data);
        if (fed.is_err()) {
            close_conn(fd, "h2 feed error: " + fed.error().to_string());
            return;
        }
        conn.rbuf.commit_read(data.size());
    }

    // Process frames and get ready streams
    auto ready_result = conn.h2conn->process();
    if (ready_result.is_err()) {
        close_conn(fd, "h2 process error: " + ready_result.error().to_string());
        return;
    }

    // Handle each complete request stream
    for (uint32_t stream_id : ready_result.value()) {
        const auto& headers = conn.h2conn->stream_headers(stream_id);

        // Convert H2 pseudo-headers to HTTP/1.1 request
        HttpRequest req;
        std::string method, path, authority;
        for (const auto& h : headers) {
            if (h.name == ":method") method = h.value;
            else if (h.name == ":path") path = h.value;
            else if (h.name == ":authority") authority = h.value;
            else if (h.name[0] != ':') {
                req.headers.push_back(HttpHeader{h.name, h.value});
            }
        }
        req.method = method;
        req.target = path;
        req.version = "HTTP/1.1";
        req.keep_alive = true;

        auto body_data = conn.h2conn->stream_body(stream_id);
        auto body = std::span<const unsigned char>(body_data.data(), body_data.size());
        req.content_length = body.size();

        // Select upstream and forward
        auto upstream = lb_.next();
        if (upstream == nullptr) {
            std::vector<HpackHeader> resp_headers;
            resp_headers.push_back({":status", "503"});
            resp_headers.push_back({"content-length", "0"});
            (void)conn.h2conn->send_response(stream_id, std::move(resp_headers), {}, true);
        } else {
            conn.upstream = upstream;
            auto response = forward_http1(req, body, upstream, pools_, *loop_);
            if (response.is_err()) {
                upstream->failed_requests.fetch_add(1);
                LOG_WARN("h2 upstream failed", "upstream", upstream->name, "error",
                         response.error().to_string());
                std::vector<HpackHeader> resp_headers;
                resp_headers.push_back({":status", "502"});
                resp_headers.push_back({"content-length", "0"});
                (void)conn.h2conn->send_response(stream_id, std::move(resp_headers), {}, true);
            } else {
                // Parse upstream HTTP/1.1 response and convert to H2
                auto& resp_bytes = response.value();
                RingBuffer resp_buf(resp_bytes.size());
                (void)resp_buf.append(resp_bytes);
                HttpResponse resp;
                Http1Parser parser;
                if (parser.parse_response(resp_buf, resp) == ParseResult::Complete) {
                    std::vector<HpackHeader> resp_headers;
                    resp_headers.push_back({":status", std::to_string(resp.status)});
                    for (const auto& h : resp.headers) {
                        // Skip hop-by-hop headers
                        std::string lname(h.name);
                        for (auto& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (lname == "connection" || lname == "keep-alive" ||
                            lname == "transfer-encoding" || lname == "upgrade") {
                            continue;
                        }
                        resp_headers.push_back({std::string(h.name), std::string(h.value)});
                    }
                    size_t body_offset = resp.header_bytes;
                    size_t body_len = resp_bytes.size() > body_offset
                                          ? resp_bytes.size() - body_offset
                                          : 0;
                    auto resp_body = std::span<const unsigned char>(
                        resp_bytes.data() + body_offset, body_len);
                    (void)conn.h2conn->send_response(stream_id, std::move(resp_headers),
                                                     resp_body, true);
                } else {
                    std::vector<HpackHeader> resp_headers;
                    resp_headers.push_back({":status", "502"});
                    resp_headers.push_back({"content-length", "0"});
                    (void)conn.h2conn->send_response(stream_id, std::move(resp_headers), {}, true);
                }
            }
        }
        conn.h2conn->consume_body(stream_id, body.size());
    }

    // Flush H2 output to client
    auto output = conn.h2conn->pending_output();
    if (!output.empty()) {
        (void)conn.wbuf.append(output);
        conn.h2conn->consume_output(output.size());
        (void)loop_->modify(fd, Event::Read | Event::Write, &conn);
    }

    if (conn.h2conn->is_done()) {
        close_conn(fd, "h2 done");
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
