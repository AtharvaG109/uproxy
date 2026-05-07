# Security Policy

Report security issues privately to the repository owner. Do not open public
issues for vulnerabilities.

`uproxy` treats all network, config, and CLI input as hostile. The parser and
proxy layers enforce explicit limits for header count, header size, frame size,
connection pool capacity, and HTTP/2 flow-control windows.

## Threat Model

### Trusted vs Untrusted Inputs

All data arriving over the network (client HTTP requests, upstream responses) is
treated as **untrusted**. Parsers enforce strict length and count limits before
any processing occurs. Configuration files and CLI arguments are considered
**trusted** -- they are read at startup by a privileged operator. However, all
parsed values are validated (port ranges, file paths, timeouts) before use.

### TLS Key File Handling

Private key files are read once at startup by the TLS context. The process
should be run under a dedicated service account that has read-only access to the
key material. Log files are opened with mode 0600 to prevent other users from
reading potentially sensitive request data. Key file paths are never logged.

### Signal Safety

Signal handlers (`SIGINT`, `SIGTERM`) only perform a single atomic store to a
`std::atomic<bool>` flag (`g_shutdown`). No memory allocation, logging, or other
async-signal-unsafe operations occur inside signal handlers. `SIGPIPE` is
ignored to prevent unexpected process termination on broken connections.

