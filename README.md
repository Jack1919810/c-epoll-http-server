# HTTP Server from Scratch in C

A learning project: building a single-threaded, non-blocking HTTP server in C from scratch, following a 5-week curriculum.

## Progress

- **Week 1**: Blocking single-threaded TCP echo server (socket API)
- **Week 2**: Non-blocking I/O multiplexing with epoll (LT + ET)
- **Week 3**: HTTP/1.1 protocol parsing, static file serving, MIME types, `sendfile()` zero-copy
- **Week 4**: Keep-alive, connection timeout, thread pool / Reactor (in progress)
- **Week 5**: Performance tuning, benchmarking vs nginx (planned)

## Build

```bash
gcc -Wall -Wextra -o HTTP_server HTTP_server_epoll_ET.c
```

## Run

```bash
mkdir -p www
echo '<h1>Hello</h1>' > www/index.html
./HTTP_server
# Visit http://localhost:8080/ in browser
```

## Architecture (current)

- Single-threaded event loop on epoll (ET mode)
- Per-connection state struct (input buffer + parse state + send state)
- Resumable HTTP request parser (state machine, not regex)
- Static file serving from `./www/` with `sendfile()` zero-copy
- EPOLLOUT-based backpressure handling for slow clients

## Notes

This is a learning project, not production-ready. Notably missing:
- Keep-alive (Week 4)
- HTTPS / TLS
- Logging
- Rate limiting
- Realpath-based directory traversal defense (currently just rejects `..`)