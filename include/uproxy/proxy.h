#pragma once

#include "uproxy/config.h"
#include "uproxy/event_loop.h"
#include "uproxy/http1.h"
#include "uproxy/http2.h"
#include "uproxy/load_balancer.h"
#include "uproxy/log.h"
#include "uproxy/tls.h"
#include "uproxy/upstream_pool.h"

#include <atomic>
#include <memory>
#include <unordered_map>

namespace uproxy {

enum class ConnState {
    TLSHandshake,
    ReadRequest,
    ConnectUpstream,
    SendRequest,
    ReadResponse,
    SendResponse,
    Done,
    Error,
};

struct ClientConn {
    Uniquefd fd;
    ConnState state{ConnState::ReadRequest};
    enum class Proto { HTTP1, HTTP2 } proto{Proto::HTTP1};
    // 65536 (64 KiB): matches HTTP/2 default max frame size and provides a
    // reasonable read/write buffer for a single client without excessive memory.
    RingBuffer rbuf{65536};
    RingBuffer wbuf{65536};
    Http1Parser h1parser;
    HttpRequest current_req;
    bool keep_alive{true};
    int req_count{0};
    std::unique_ptr<H2Conn> h2conn;
    std::shared_ptr<UpstreamEndpoint> upstream;
    PooledConn* upstream_conn{nullptr};
    // 65536 (64 KiB): same rationale as client rbuf/wbuf above.
    RingBuffer up_rbuf{65536};
    RingBuffer up_wbuf{65536};
    uint64_t connected_at_ms{0};
    uint64_t request_at_ms{0};
    std::string remote_addr;
    std::unique_ptr<TLSConn> tls_conn;
};

class ProxyServer {
    ProxyConfig cfg_;
    std::unique_ptr<EventLoop> loop_;
    Uniquefd listen_fd_;
    Uniquefd tls_listen_fd_;
    TLSContext tls_ctx_;
    WeightedRoundRobin lb_;
    PoolManager pools_;
    std::unordered_map<int, std::unique_ptr<ClientConn>> conns_;

  public:
    explicit ProxyServer(ProxyConfig cfg);
    [[nodiscard]] Result<void> run();
    // Intentionally public static: accessed directly from signal handlers which
    // lack an instance pointer. Atomic store is async-signal-safe on all
    // supported platforms.
    static std::atomic<bool> g_shutdown;

  private:
    [[nodiscard]] Result<void> setup_listeners();
    void on_accept(int listen_fd, bool tls);
    void on_client_event(int fd, Event events);
    void handle_h2_client(ClientConn& conn, int fd);
    void close_conn(int fd, std::string_view reason);
    [[nodiscard]] uint64_t now_ms() const noexcept;
    void on_tick();
};

} // namespace uproxy
