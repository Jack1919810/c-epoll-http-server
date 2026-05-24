# HTTP Server from Scratch in C

A Linux HTTP server written in C using only POSIX socket API and epoll — 
no libraries, no frameworks. Built to deeply understand the syscall layer, 
TCP byte-stream semantics, and the event-driven concurrency model behind 
nginx and Redis.

## Technical Highlights

- **State-machine HTTP parser**: resumable across fragmented `read()` calls,
  byte-stream aware (no regex, no string functions on network data)
- **Edge-triggered epoll**: drain loops to `EAGAIN`, callback-driven kernel
  notification with red-black tree + ready list
- **Zero-copy file transfer** with `sendfile(2)`: file bytes never enter user space
- **EPOLLOUT-based backpressure**: handles slow clients without busy-spinning
  or dropping data
- **Per-connection state via `epoll_data.ptr`**: cleanly separates connection
  lifecycle from event dispatch

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

## Learning Notes

Detailed bilingual (CN/EN) technical notes from each week:

- [Week 1 — Socket API fundamentals](docs/week1_summary.md)
- [Week 2 — epoll & I/O multiplexing](docs/week2_summary.md)  
- [Week 3 — HTTP protocol & static file serving](docs/week3_summary.md)

# Historical Code Snapshots

Earlier versions of the server, preserved here to show the progression.

The current production code is in the **repo root** (`HTTP_server_epoll_ET.c`).

| File | Week | Description |
|---|---|---|
| `echo_server.c`            | 1 | Blocking single-threaded TCP echo server |
| `echo_server_epoll_LT.c`   | 2 | Non-blocking + epoll **Level-Triggered** echo server |
| `echo_server_epoll_ET.c`   | 2 | Non-blocking + epoll **Edge-Triggered** echo server |

See [`../docs/`](../docs/) for the technical write-ups corresponding to each week.