# Week 3 技术要点 / Week 3 Technical Summary

> HTTP Server with epoll — Week 3: HTTP 协议解析 + 静态文件服务 / HTTP Parsing + Static File Serving

---

# 📘 中文版

## 1. HTTP 请求 / 响应结构

**请求**：

```
GET /index.html HTTP/1.1\r\n      ← 请求行: method 空格 path 空格 version
Host: localhost:8080\r\n           ← header
User-Agent: ...\r\n                ← header
\r\n                                ← 空行 = header 区结束的标志
[body, GET 通常没有]
```

**响应**（镜像着的）：

```
HTTP/1.1 200 OK\r\n               ← 状态行: version 空格 code 空格 reason
Content-Type: text/html\r\n        ← header
Content-Length: 13\r\n             ← 必须告诉客户端 body 多长
Connection: close\r\n              ← 我发完就关
\r\n                                ← 空行
Hello, World!                       ← body (Content-Length 字节)
```

每行 `\r\n` 结尾（CRLF），header 区以**空行 `\r\n\r\n`** 结束。

## 2. TCP 字节流 ≠ HTTP 请求（关键认知）

- TCP 是字节流，**没有消息边界**
- 一个 HTTP 请求可能跨多次 `read()` 才到齐
- 一次 `read()` 也可能拿到多个请求（keep-alive）
- ET 模式 drain 出来的字节**很可能是半个请求**

→ 解析器必须**可暂停、可恢复**：字节来一截处理一截，没凑齐就停下，等下次。

## 3. 状态机设计

按"行"推进的状态：

```
PARSE_REQUEST_LINE → PARSE_HEADERS → PARSE_DONE
   |                    ↑    ↓        
   ↓                    └────┘       
   解析第一行         循环吃 header   遇到空行 → DONE
```

每来一截字节：从缓冲区切出完整 `\r\n` 行，按当前 state 处理；切不出完整行就返回 `INCOMPLETE` 等下次。

## 4. 解析器 API 设计：填 struct + 返回状态码

**为什么不返回 `char *`**：解析产出多个结构化字段（method、path、N 个 headers），一个指针装不下。

**正确模式**（跟 `accept` 同款）：

```c
enum parse_result {
    PARSE_OK,           // 完整请求,可处理
    PARSE_INCOMPLETE,   // 字节没到齐,等下次
    PARSE_ERROR,        // 格式坏,回 400
};

enum parse_result parse_request(struct request *req,
                                const char *buf, size_t len);
```

**三态返回是 TCP 字节流逼出来的**——`bool` 不够用，必须区分"还没到齐"和"真坏了"，两种处理完全相反。

## 5. 可恢复解析的核心：parsed 偏移

```c
struct request {
    enum parse_state state;     // 状态机当前位置
    size_t parsed;              // 已消费到 buf 第几字节
    char method[8];
    char path[1024];
    char version[16];
    struct header headers[32];
    int header_count;
};
```

`buf` 永远传**累积缓冲区**，`parsed` 记录"已经解析到哪了"。新数据来追加到 buf 末尾，解析器从 `parsed` 继续。

`{0}` 初始化正好让 state 从 `PARSE_REQUEST_LINE` 开始（enum 第一个值 = 0）。

## 6. per-connection 状态结构

每个连接有自己的累积缓冲、解析进度、发送状态：

```c
struct connection {
    int fd;
    char read_buf[8192];        // 累积输入
    size_t read_len;
    struct request req;         // 解析状态
    int sending_fd;             // 当前在传的文件 fd(-1 表示没在传)
    off_t send_offset;
    size_t send_total;
};
```

## 7. epoll_data：从 `.fd` 升级到 `.ptr`

`epoll_data` 是个 union，`fd` 和 `ptr` 共用内存。早期存 `data.fd` 因为没别的可存；引入 connection 后存 `data.ptr` 指向 connection，所有 per-fd 状态一并带回：

```c
ev.data.ptr = conn;                          // 注册
struct connection *conn = events[i].data.ptr; // 取出
```

server_fd 也包一个 connection（只用 fd 字段），统一所有 epoll_data 都用 `.ptr`，事件分发用 `conn->fd == server_fd` 区分。

## 8. 字节流处理工具

**永远不用 `str` 系列函数处理网络数据**——它们依赖 `\0`，TCP 字节流没有 `\0` 保证。

| 工具 | 用途 |
|---|---|
| `memmem(haystack, hsize, needle, nsize)` | 在字节流里找一个序列（如 `\r\n`），需要 `_GNU_SOURCE` |
| `memchr(buf, c, n)` | 找单个字节（如 `' '`、`':'`） |
| `strrchr(str, c)` | 找**最后一个**指定字符（如文件后缀的 `.`） |
| `memcpy(dst, src, n)` | 字节级拷贝，不看 `\0` |

**指针就是位置，指针相减就是距离（字节数）**——切行靠这个：

```c
const char *crlf = memmem(buf+parsed, len-parsed, "\r\n", 2);
size_t line_len = crlf - (buf + parsed);   // 行内容长度,不含 \r\n
parsed += line_len + 2;                     // 推进:跨过行 + \r\n
```

## 9. `copy_field` helper

固定 buffer 接收变长 src，复用率高：

```c
static int copy_field(char *dst, size_t dst_size,
                      const char *src, size_t src_len) {
    if (src_len >= dst_size) return -1;    // 留 1 字节给 \0
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';                    // C 字符串必加 \0
    return 0;
}
```

请求行三段（method/path/version）和每个 header 的 name/value，都用它，bounds check + memcpy + `\0` 集中维护一次。

## 10. HTTP 响应生成

状态行 + headers + 空行 + body。常用状态码：

| 码 | reason | 何时用 |
|---|---|---|
| 200 | OK | 成功 |
| 400 | Bad Request | parse_request 返回 ERROR |
| 404 | Not Found | open() 返回 ENOENT |
| 500 | Internal Server Error | 其他系统调用失败 |

抽 helper：

```c
static void send_response(int fd, int status, const char *reason,
                          const char *content_type, const char *body);
```

`Content-Length` 必须正确（让浏览器知道 body 边界），`Connection: close` 表明不复用连接。

## 11. MIME 类型推断

浏览器靠 `Content-Type` 决定怎么处理 body。每个资源（HTML、CSS、JS、PNG、JPG…）都要正确的 MIME。

```c
const char *dot = strrchr(path, '.');      // 找最后一个 '.'
if (strcmp(dot, ".html") == 0) return "text/html";
if (strcmp(dot, ".png")  == 0) return "image/png";
// ...
return "application/octet-stream";          // 不认识的兜底
```

**每个 HTTP 请求独立**：HTML 不"声明"子资源的类型。浏览器载入一个页面会触发几十个独立请求（CSS / JS / 图片 / favicon），每个都要服务器单独给正确的 Content-Type。

## 12. 静态文件服务流程

```
GET /foo.html → req->path = "/foo.html"
  → "/" 映射到 "/index.html"
  → 安全检查: path 含 ".." → 400
  → 拼磁盘路径: "./www/foo.html"
  → open()
       失败 ENOENT → 404
       失败其他    → 500
  → fstat() 拿文件大小
  → write() HTTP header
  → sendfile() 发文件内容
  → close(file_fd) + close_connection
```

## 13. 安全：目录穿越（Directory Traversal）

```
GET /../../etc/passwd HTTP/1.1
```

不防御的话拼成 `./www/../../etc/passwd`，`open()` 真打开系统密码文件。

**Web 安全第一坑：永远不信客户端传的 path。**

简陋防御：拒绝任何含 `..` 的 path。生产做法：`realpath()` 解析后校验在 doc root 内。

## 14. `sendfile()` 零拷贝

**普通做法**（数据在用户态绕一圈）：

```
file → kernel page cache → user buffer → kernel socket buffer → NIC
                  read() 拷一次          write() 拷一次
                  + 内核↔用户切换           + 内核↔用户切换
```

**sendfile 做法**（数据全程留在内核态）：

```
file → kernel page cache ────────→ kernel socket buffer → NIC
                  sendfile() 内核内倒
                  一次 syscall, 零用户态
```

```c
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
```

- 省一次拷贝、一次 syscall 切换
- 不占用户态内存（无 malloc）
- 大文件性能巨大差距，**面试经典"零拷贝"题**
- 严格说不是真"零次"——page cache 到 socket buffer 还是有内核内拷贝，配合 NIC DMA gather/scatter 才接近真零拷贝

## 15. EPOLLOUT 的真实用法：背压处理

`sendfile`（或 write）在非阻塞 socket 上可能返回 `EAGAIN`——内核发送缓冲满了（对端读得慢）。不能空转重试，正确做法：

```
sendfile 遇到 EAGAIN
  → 把发送状态(sending_fd / send_offset / send_total)存进 conn
  → epoll_ctl(MOD, EPOLLIN | EPOLLOUT | EPOLLET)
  → return 让出去

EPOLLOUT 触发(发送缓冲腾出空间)
  → 从 conn 取出 send 状态
  → sendfile 续传
  → 发完 → close 文件 + 关连接
       继续 EAGAIN → 留着 EPOLLOUT, 等下次
```

**EPOLLOUT 必须按需开关**：不发数据时关掉（否则 LT 会 100% CPU 空转），有 backpressure 时开启。状态机：

| 状态 | EPOLLOUT |
|---|---|
| 没有待发数据 | **关** |
| 有数据没发完（write/sendfile 卡 EAGAIN） | **开** |
| 数据发完 | **关** |

## 16. 心智模型：组件协作

Week 1-2 是**单个组件**（socket / epoll）。Week 3 是**多组件协作**：

```
parser ←→ event loop ←→ file I/O ←→ response generation
   ↑           ↑            ↑              ↑
   状态可恢复  事件分发     sendfile      MIME + 错误码
```

每个组件单独写、单独测，再接进事件循环。难点不在单个组件，在**接口和状态传递**——比如 sendfile 卡 EAGAIN 时怎么把状态传给下次 EPOLLOUT。

## 17. 防御式编程：每个系统调用查返回值

每个分支对应一种真实场景：

| 检查 | 实际场景 |
|---|---|
| `strstr(path, "..")` | 恶意请求目录穿越 |
| `open()` + `ENOENT` | 文件不存在（用户输错 URL / favicon.ico） |
| `open()` + 其他 errno | EACCES（无权限）、EISDIR（是目录）、EMFILE（fd 用光）等 |
| `fstat()` 失败 | 极罕见但防御性补上 |
| `malloc()` 返 NULL | 大文件分配失败 |

99% 不会触发的分支，1% 在生产里跑两年总会撞到。`if (... < 0)` 不是仪式，是真防御。

## 18. 面试题清单

- ✅ HTTP 请求 / 响应的格式？请求行 / 状态行 / header / body
- ✅ 为什么 HTTP 解析器要写成状态机而不是正则？→ 字节流可能分片到达，必须可暂停可恢复
- ✅ TCP 字节流和 HTTP 消息的关系？→ 字节流没有消息边界
- ✅ 解析器的 INCOMPLETE / OK / ERROR 三态为什么必要？
- ✅ 零拷贝是什么？sendfile 怎么实现的？→ 内核内倒数据，减少 user-space 拷贝和 syscall
- ✅ EPOLLOUT 为什么不能常驻？→ LT 下 socket 几乎总可写，会 100% CPU 空转
- ✅ EPOLLOUT 什么时候用？→ write/sendfile 遇到 EAGAIN，开 EPOLLOUT 等可写边沿
- ✅ MIME type 的作用？→ 浏览器靠 Content-Type 决定怎么处理 body
- ✅ HTTP 各状态码常见用途？400 / 404 / 500
- ✅ 目录穿越攻击？怎么防御？

---

---

# 📗 English Version

## 1. HTTP Request / Response Structure

**Request**:

```
GET /index.html HTTP/1.1\r\n      ← request line: method SP path SP version
Host: localhost:8080\r\n           ← header
User-Agent: ...\r\n                ← header
\r\n                                ← blank line = end of headers
[body, usually empty for GET]
```

**Response** (mirror of request):

```
HTTP/1.1 200 OK\r\n               ← status line: version SP code SP reason
Content-Type: text/html\r\n
Content-Length: 13\r\n             ← must tell client body length
Connection: close\r\n
\r\n
Hello, World!                       ← body (Content-Length bytes)
```

Each line ends with `\r\n` (CRLF), headers section ends with the **blank line `\r\n\r\n`**.

## 2. TCP Byte Stream ≠ HTTP Request (Critical Insight)

- TCP is a byte stream with **no message boundaries**
- One HTTP request may span multiple `read()` calls
- One `read()` may return multiple requests (keep-alive)
- ET-mode drain may yield **half a request**

→ Parser must be **pause-able and resumable**: process bytes as they arrive, stop when incomplete, wait for more.

## 3. State Machine Design

Line-based progression:

```
PARSE_REQUEST_LINE → PARSE_HEADERS → PARSE_DONE
   |                    ↑    ↓        
   ↓                    └────┘       
   parse first line   loop over headers   empty line → DONE
```

Each call: extract complete `\r\n`-terminated lines from buffer, dispatch by current state; if no complete line, return `INCOMPLETE`.

## 4. Parser API Design: Fill Struct + Return Status

**Why not return `char *`**: parsing produces multiple structured fields (method, path, N headers) — one pointer can't hold them.

**Correct pattern** (same as `accept`):

```c
enum parse_result {
    PARSE_OK,           // complete request, ready to handle
    PARSE_INCOMPLETE,   // not enough bytes yet, call again
    PARSE_ERROR,        // malformed, send 400
};

enum parse_result parse_request(struct request *req,
                                const char *buf, size_t len);
```

**The three-state return is forced by the TCP byte stream reality** — a `bool` isn't enough; you must distinguish "not yet" from "broken," which require opposite handling.

## 5. Core of Resumable Parsing: the `parsed` Offset

```c
struct request {
    enum parse_state state;
    size_t parsed;              // how many bytes consumed
    char method[8];
    char path[1024];
    char version[16];
    struct header headers[32];
    int header_count;
};
```

`buf` is always the **accumulated buffer**, `parsed` tracks "how far in." New bytes append to buf; parser resumes from `parsed`.

`{0}` initialization conveniently starts state at `PARSE_REQUEST_LINE` (first enum value = 0).

## 6. Per-Connection State

Each connection has its own input buffer, parse progress, send state:

```c
struct connection {
    int fd;
    char read_buf[8192];
    size_t read_len;
    struct request req;
    int sending_fd;             // -1 means not currently sending
    off_t send_offset;
    size_t send_total;
};
```

## 7. `epoll_data`: From `.fd` to `.ptr`

`epoll_data` is a union — `fd` and `ptr` share memory. Once you have a connection struct, use `data.ptr` to carry all per-fd state:

```c
ev.data.ptr = conn;                              // register
struct connection *conn = events[i].data.ptr;    // retrieve
```

Wrap `server_fd` in a connection too (only `fd` field used) so all event entries use `.ptr` uniformly; distinguish in dispatch with `conn->fd == server_fd`.

## 8. Byte-Stream Tools

**Never use `str*` functions on network data** — they rely on `\0`, which TCP byte streams don't guarantee.

| Tool | Purpose |
|---|---|
| `memmem(hay, hsize, needle, nsize)` | Find a sequence (e.g. `\r\n`); needs `_GNU_SOURCE` |
| `memchr(buf, c, n)` | Find a single byte (e.g. `' '`, `':'`) |
| `strrchr(str, c)` | Find the **last** occurrence of a char (e.g. last `.` for file extension) |
| `memcpy(dst, src, n)` | Byte-level copy, no `\0` awareness |

**A pointer is a position; subtracting pointers gives distance (bytes)** — line extraction relies on this:

```c
const char *crlf = memmem(buf+parsed, len-parsed, "\r\n", 2);
size_t line_len = crlf - (buf + parsed);   // content length, excluding \r\n
parsed += line_len + 2;                     // advance past line + \r\n
```

## 9. The `copy_field` Helper

Fixed-buffer destination, variable-length source — pattern used many times:

```c
static int copy_field(char *dst, size_t dst_size,
                      const char *src, size_t src_len) {
    if (src_len >= dst_size) return -1;    // leave 1 byte for \0
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';                    // C strings need \0
    return 0;
}
```

Used for request line's three parts (method/path/version) and each header's name/value — bounds check + memcpy + `\0` centralized in one place.

## 10. HTTP Response Generation

Status line + headers + blank line + body. Common status codes:

| Code | Reason | When |
|---|---|---|
| 200 | OK | Success |
| 400 | Bad Request | parse_request returns ERROR |
| 404 | Not Found | `open()` returns ENOENT |
| 500 | Internal Server Error | Other syscall failures |

Helper:

```c
static void send_response(int fd, int status, const char *reason,
                          const char *content_type, const char *body);
```

`Content-Length` must be correct (browser uses it to know body boundary); `Connection: close` indicates no keep-alive.

## 11. MIME Type Inference

Browser uses `Content-Type` to decide how to handle body. Each resource (HTML/CSS/JS/PNG/JPG…) needs the correct MIME.

```c
const char *dot = strrchr(path, '.');      // last '.'
if (strcmp(dot, ".html") == 0) return "text/html";
if (strcmp(dot, ".png")  == 0) return "image/png";
// ...
return "application/octet-stream";          // unknown fallback
```

**Each HTTP request is independent**: HTML doesn't "declare" sub-resource types. Loading a page triggers dozens of independent requests (CSS / JS / images / favicon); the server must set the correct Content-Type for each.

## 12. Static File Serving Flow

```
GET /foo.html → req->path = "/foo.html"
  → "/" maps to "/index.html"
  → security check: path contains ".." → 400
  → build disk path: "./www/foo.html"
  → open()
       failure ENOENT → 404
       other failure  → 500
  → fstat() for file size
  → write() HTTP headers
  → sendfile() body
  → close(file_fd) + close_connection
```

## 13. Security: Directory Traversal

```
GET /../../etc/passwd HTTP/1.1
```

Unguarded, this becomes `./www/../../etc/passwd` — `open()` actually opens the system password file.

**Web security rule #1: never trust client-supplied paths.**

Simple defense: reject any path containing `..`. Production: use `realpath()` and verify it's still inside doc root.

## 14. `sendfile()` Zero-Copy

**Without sendfile** (data detours through user space):

```
file → kernel page cache → user buffer → kernel socket buffer → NIC
                  read()                   write()
                  + kernel↔user switch     + kernel↔user switch
```

**With sendfile** (data stays in kernel):

```
file → kernel page cache ────────→ kernel socket buffer → NIC
                  sendfile() internal copy
                  one syscall, no user space
```

```c
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
```

- Saves one copy and one syscall switch
- No user-space memory required (no malloc)
- Big win on large files; **classic interview "zero-copy" question**
- Not literally "zero" copies — there's still a kernel-internal copy from page cache to socket buffer; combined with NIC DMA gather/scatter approaches true zero-copy

## 15. EPOLLOUT's Real Purpose: Backpressure Handling

`sendfile` (or write) on a non-blocking socket can return `EAGAIN` — the kernel send buffer is full (slow peer). You can't busy-retry. Correct flow:

```
sendfile hits EAGAIN
  → save send state (sending_fd / send_offset / send_total) in conn
  → epoll_ctl(MOD, EPOLLIN | EPOLLOUT | EPOLLET)
  → return, yield

EPOLLOUT fires (send buffer drained)
  → retrieve send state from conn
  → resume sendfile
  → done → close file + close connection
       still EAGAIN → keep EPOLLOUT registered, wait again
```

**EPOLLOUT must be toggled on demand**: register only when needed, deregister when done (otherwise LT will busy-spin at 100% CPU). State machine:

| State | EPOLLOUT |
|---|---|
| No data pending | **off** |
| Data pending, write/sendfile blocked on EAGAIN | **on** |
| Data fully sent | **off** |

## 16. Mental Model: Component Coordination

Weeks 1-2 were **single components** (socket / epoll). Week 3 is **multiple components cooperating**:

```
parser ←→ event loop ←→ file I/O ←→ response generation
   ↑           ↑            ↑              ↑
   resumable   dispatch    sendfile       MIME + errors
```

Each component written and tested independently, then integrated into the event loop. The hard part isn't any single component — it's **interface design and state passing**, e.g. how sendfile's EAGAIN state hands off to the next EPOLLOUT.

## 17. Defensive Programming: Check Every Syscall

Each branch corresponds to a real scenario:

| Check | Real-world cause |
|---|---|
| `strstr(path, "..")` | Malicious directory-traversal request |
| `open()` + `ENOENT` | File not found (typo, missing favicon.ico) |
| `open()` + other errno | EACCES, EISDIR, EMFILE, etc. |
| `fstat()` failure | Extremely rare; defensive |
| `malloc()` returns NULL | Large-file allocation failure |

The 99%-never branches still get hit eventually in production over months/years. `if (... < 0)` isn't ritual; it's real defense.

## 18. Interview Questions Checklist

- ✅ HTTP request / response format? Request line / status line / headers / body
- ✅ Why must HTTP parser be a state machine, not regex? → byte stream may arrive in fragments, must pause and resume
- ✅ How does TCP byte stream relate to HTTP messages? → byte streams have no message boundaries
- ✅ Why is the three-state INCOMPLETE / OK / ERROR return necessary?
- ✅ What is zero-copy? How does sendfile achieve it? → kernel-internal data transfer, fewer copies and syscalls
- ✅ Why can't EPOLLOUT be permanently registered? → under LT, socket is almost always writable → 100% CPU
- ✅ When do you use EPOLLOUT? → after write/sendfile hits EAGAIN, register and wait for writable edge
- ✅ Purpose of MIME types? → browser uses Content-Type to decide how to handle body
- ✅ Common HTTP status codes? 400 / 404 / 500
- ✅ Directory traversal attack? How to defend?
