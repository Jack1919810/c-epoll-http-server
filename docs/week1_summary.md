# Week 1 技术要点 / Week 1 Technical Summary

> HTTP Server with epoll — Week 1: TCP Echo Server (基础打底 / Foundation)

---

# 📘 中文版

## 1. Socket API 基础流程

服务器端五步走：

```
socket() → setsockopt() → bind() → listen() → accept() → read()/write() → close()
```

| 步骤 | 作用 |
|---|---|
| `socket()` | 内核创建通信端点，返回 fd |
| `setsockopt()` | 配置 socket 行为选项 |
| `bind()` | 把 socket 绑定到具体 IP:Port |
| `listen()` | 把主动 socket 转为被动监听 |
| `accept()` | 阻塞等待客户端连接，返回新 fd |
| `read()/write()` | 双向数据传输 |
| `close()` | 释放 fd |

## 2. 关键数据结构

```c
struct sockaddr_in {
    sa_family_t    sin_family;   // AF_INET (IPv4)
    in_port_t      sin_port;     // 端口号(网络字节序)
    struct in_addr sin_addr;     // IP 地址(网络字节序)
};
```

- `bind`/`accept` 等函数接收的是 `struct sockaddr *`（通用基类）
- `sockaddr_in` 是 IPv4 的特化版本，需要强制类型转换
- 这是 C 在没有继承时代实现的"多态"

## 3. 字节序

**网络字节序 = 大端**

| 函数 | 含义 | 用途 |
|---|---|---|
| `htons()` | host to network short | 端口号转网络字节序 |
| `htonl()` | host to network long | IP 地址转网络字节序 |
| `ntohs()` / `ntohl()` | 反向 | 把网络字节序转回主机字节序 |

不同 CPU 字节序不同（x86 小端，部分 ARM 大端），网络协议规定统一用大端。

## 4. TIME_WAIT 与 SO_REUSEADDR

**TIME_WAIT**：主动关闭 TCP 连接的一方进入此状态，停留 2×MSL（Linux 默认 60s）

存在原因：
1. 防止"幽灵数据包"误投递到下一个使用相同四元组的连接
2. 保证对方能收到最后的 FIN ACK（万一丢了能重发）

**SO_REUSEADDR**：允许 bind 到 TIME_WAIT 状态的端口，必须在 bind 之前设置：

```c
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

**SO_REUSEADDR vs SO_REUSEPORT**：

| | SO_REUSEADDR | SO_REUSEPORT |
|---|---|---|
| 解决 | TIME_WAIT 端口复用 | 多进程绑同端口 |
| 多进程同时 listen | ❌ | ✅ (Linux 3.9+) |
| 用途 | 服务器快速重启 | 高性能负载均衡 |

## 5. TCP 握手队列

```
客户端 SYN → [SYN 队列] → 内核回 SYN+ACK
                   ↓
            客户端 ACK → [accept 队列]
                              ↓
                         你的 accept() 来取
```

- **SYN 队列**：未完成握手，由 `/proc/sys/net/ipv4/tcp_max_syn_backlog` 控制
- **accept 队列**：已完成握手，由 `listen(fd, BACKLOG)` 中的 backlog 控制
- 内核会把 backlog 截断到 `/proc/sys/net/core/somaxconn` 上限

面试题：SYN Flood 攻击如何防？答：`tcp_syncookies`

## 6. 系统调用错误处理约定

Linux 通用约定：
- 成功：返回 ≥ 0（fd、字节数、或 0）
- 失败：返回 -1，全局变量 `errno` 被设置

```c
if (some_syscall(...) < 0) {
    perror("syscall_name");    // 自动读 errno 翻译成人话
    // 清理资源
    // exit / continue / break (按场景)
}
```

## 7. 错误处理哲学

**两类错误**：

| 类型 | 例子 | 处理 |
|---|---|---|
| 服务器级 | socket/bind/listen 失败 | `perror` + 清理 + `exit` |
| 客户端级 | accept/read/write 失败 | `perror` + 关 client_fd（如有）+ `continue` |

**核心原则**：一个客户端的问题不能搞垮服务器

特例：accept 失败时 client_fd = -1，不需要 close

## 8. SIGPIPE 陷阱

**场景**：客户端先关连接 → 服务器 write → 进程收到 SIGPIPE → 直接被杀

**解决**：程序开头加：

```c
#include <signal.h>
signal(SIGPIPE, SIG_IGN);
```

之后 write 失败正常返回 -1，`errno = EPIPE`

## 9. read() 返回值的三种情况

| 返回值 | 含义 | 处理 |
|---|---|---|
| `> 0` | 读到 n 字节 | 正常处理 |
| `== 0` | 对端关闭连接(EOF) | 不是错误！break + close |
| `< 0` | 出错 | `perror` + break + close |

`n == 0` 是正常的客户端断开信号。

## 10. 字节流 vs 字符串

**TCP 是字节流，不是字符串**

- ❌ 不要写 `buf[n] = '\0'` 然后 `strlen(buf)`
- ✅ 永远用 `(buffer, length)` 配对处理
- ❌ 不要用字符串函数（`strcpy`、`strlen`、`printf("%s")`）
- ✅ 用字节函数（`memcpy`、`memmove`）

原因：网络数据可能含 `0x00`（PNG、二进制协议）

## 11. write() 短写问题

`write(fd, buf, n)` 可能只写一部分就返回。正确写法是循环：

```c
ssize_t total = 0;
while (total < n) {
    ssize_t written = write(fd, buf + total, n - total);
    if (written < 0) { perror("write"); break; }
    total += written;
}
```

Week 2 ET 模式下短写是必然事件。

## 12. C 指针核心

```c
int x = 5;
int *p = &x;     // & 取地址
int y = *p;      // * 解引用
```

**`*` 的两个用法**：
- 类型声明中：`int *p` 声明指针
- 表达式中：`*p` 解引用

**结构体访问**：
- 直接：`addr.sin_family`（点号）
- 指针：`p->sin_family`（箭头，等价于 `(*p).sin_family`）

**Value-result 参数**：函数通过指针修改你的变量。典型例子 `accept(fd, &addr, &len)` 中的 `&len` 既是输入也是输出。

## 13. 单线程阻塞模型的缺陷

亲眼观察到的现象：
- accept 后进入 read 循环 → 阻塞 → 无法处理其他客户端
- 第二个客户端被塞进 accept 队列等待
- 第一个客户端不断开，第二个永远干等

**Slowloris 攻击**：攻击者发慢请求占住 worker，使服务器无法响应其他请求

Week 2 epoll 的根本动机：单线程同时管多个 fd

## 14. 调试工具

| 工具 | 用途 |
|---|---|
| `man 2 xxx` | 查系统调用 |
| `man 7 socket` / `man 7 tcp` | 查协议层概念 |
| `perror(msg)` | 自动翻译 errno |
| `strace -e trace=network ./prog` | 跟踪网络系统调用 |
| `ss -tan` | 看 socket 状态（TIME_WAIT 等） |
| `nc localhost PORT` | 比 telnet 干净的连接工具 |
| `gcc -Wall -Wextra` | 开启编译警告 |

## 15. 面试题清单

- ✅ socket() 三个参数的含义？
- ✅ 字节序是什么？为什么要 htons？
- ✅ bind 第二参数为何是 `sockaddr *` 强转？
- ✅ listen 的 backlog 控制什么？
- ✅ SYN 队列 vs accept 队列？
- ✅ TIME_WAIT 为什么存在？
- ✅ SO_REUSEADDR 与 SO_REUSEPORT 的区别？
- ✅ accept 返回 -1 该怎么处理？
- ✅ read 返回 0 是什么意思？
- ✅ SIGPIPE 在什么场景出现？怎么处理？
- ⏳ select / poll / epoll 的区别？（Week 2）
- ⏳ epoll LT vs ET？（Week 2）

---

---

# 📗 English Version

## 1. Basic Socket API Flow

Server-side 5-step flow:

```
socket() → setsockopt() → bind() → listen() → accept() → read()/write() → close()
```

| Step | Purpose |
|---|---|
| `socket()` | Kernel creates a communication endpoint, returns an fd |
| `setsockopt()` | Configure socket behavior options |
| `bind()` | Bind the socket to a specific IP:Port |
| `listen()` | Convert active socket to passive listening |
| `accept()` | Block waiting for a client, returns a new fd |
| `read()/write()` | Bidirectional data transfer |
| `close()` | Release the fd |

## 2. Key Data Structures

```c
struct sockaddr_in {
    sa_family_t    sin_family;   // AF_INET (IPv4)
    in_port_t      sin_port;     // Port (network byte order)
    struct in_addr sin_addr;     // IP address (network byte order)
};
```

- Functions like `bind`/`accept` take `struct sockaddr *` (generic base type)
- `sockaddr_in` is the IPv4 specialization; explicit cast required
- This is "polymorphism" implemented in pre-OOP-era C

## 3. Byte Order

**Network byte order = Big-endian**

| Function | Meaning | Usage |
|---|---|---|
| `htons()` | host to network short | Port → network order |
| `htonl()` | host to network long | IP → network order |
| `ntohs()` / `ntohl()` | reverse | Network order → host order |

Different CPUs use different endianness (x86 little, some ARM big); protocols mandate big-endian.

## 4. TIME_WAIT and SO_REUSEADDR

**TIME_WAIT**: The side that actively closes a TCP connection enters this state, lasting 2×MSL (Linux default: 60s)

Reasons it exists:
1. Prevent "ghost packets" from being misdelivered to the next connection using the same 4-tuple
2. Ensure the peer can receive the final FIN ACK (can retransmit if lost)

**SO_REUSEADDR**: Allow binding to ports in TIME_WAIT state. Must be set before `bind()`:

```c
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

**SO_REUSEADDR vs SO_REUSEPORT**:

| | SO_REUSEADDR | SO_REUSEPORT |
|---|---|---|
| Solves | TIME_WAIT port reuse | Multi-process same-port binding |
| Multi-process listen | ❌ | ✅ (Linux 3.9+) |
| Use case | Fast server restart | High-perf load balancing |

## 5. TCP Handshake Queues

```
Client SYN → [SYN queue] → Kernel replies SYN+ACK
                   ↓
            Client ACK → [Accept queue]
                              ↓
                       Your accept() picks it up
```

- **SYN queue**: Incomplete handshakes, controlled by `/proc/sys/net/ipv4/tcp_max_syn_backlog`
- **Accept queue**: Completed handshakes, controlled by backlog arg of `listen(fd, BACKLOG)`
- Kernel caps backlog at `/proc/sys/net/core/somaxconn`

Interview question: How to defend against SYN flood? Answer: `tcp_syncookies`

## 6. Syscall Error Handling Convention

Linux universal convention:
- Success: returns ≥ 0 (fd, byte count, or 0)
- Failure: returns -1, global `errno` is set

```c
if (some_syscall(...) < 0) {
    perror("syscall_name");    // Auto-reads errno, prints readable message
    // Clean up resources
    // exit / continue / break (context-dependent)
}
```

## 7. Error Handling Philosophy

**Two error categories**:

| Type | Example | Handling |
|---|---|---|
| Server-level | socket/bind/listen failures | `perror` + cleanup + `exit` |
| Client-level | accept/read/write failures | `perror` + close client_fd (if any) + `continue` |

**Core principle**: A single client must not bring down the server

Special case: When accept() fails, client_fd = -1, no close needed.

## 8. SIGPIPE Pitfall

**Scenario**: Client closes first → server writes → process receives SIGPIPE → killed immediately

**Solution**: Add at program start:

```c
#include <signal.h>
signal(SIGPIPE, SIG_IGN);
```

After this, write returns -1 normally with `errno = EPIPE`

## 9. read() Three Return Cases

| Return | Meaning | Handling |
|---|---|---|
| `> 0` | Read n bytes | Normal processing |
| `== 0` | Peer closed (EOF) | **Not an error!** Break + close |
| `< 0` | Error | `perror` + break + close |

`n == 0` is the normal client disconnect signal.

## 10. Byte Stream vs String

**TCP is a byte stream, not a string**

- ❌ Don't write `buf[n] = '\0'` then `strlen(buf)`
- ✅ Always use `(buffer, length)` pairs
- ❌ Don't use string functions (`strcpy`, `strlen`, `printf("%s")`)
- ✅ Use byte-level functions (`memcpy`, `memmove`)

Reason: Network data may contain `0x00` (PNG, binary protocols)

## 11. write() Short Write Problem

`write(fd, buf, n)` may return having written only part of the data. Correct pattern is to loop:

```c
ssize_t total = 0;
while (total < n) {
    ssize_t written = write(fd, buf + total, n - total);
    if (written < 0) { perror("write"); break; }
    total += written;
}
```

Short writes are guaranteed under ET mode in Week 2.

## 12. C Pointer Essentials

```c
int x = 5;
int *p = &x;     // & = address-of operator
int y = *p;      // * = dereference operator
```

**Two roles of `*`**:
- In type declaration: `int *p` declares a pointer
- In expression: `*p` dereferences

**Struct access**:
- Direct: `addr.sin_family` (dot)
- Through pointer: `p->sin_family` (arrow, equivalent to `(*p).sin_family`)

**Value-result parameters**: Function modifies your variable through a pointer. Classic example: `&len` in `accept(fd, &addr, &len)` is both input and output.

## 13. Limitations of Single-threaded Blocking Model

Observed phenomena:
- After accept, enters read loop → blocks → cannot serve other clients
- Second client gets queued in accept queue
- Second client waits forever until the first disconnects

**Slowloris attack**: Attacker sends slow requests to occupy workers, blocking other requests

Root motivation for Week 2 epoll: Single thread monitors multiple fds simultaneously

## 14. Debug Tools

| Tool | Usage |
|---|---|
| `man 2 xxx` | Look up syscall |
| `man 7 socket` / `man 7 tcp` | Protocol-layer concepts |
| `perror(msg)` | Auto-translate errno |
| `strace -e trace=network ./prog` | Trace network syscalls |
| `ss -tan` | View socket states (TIME_WAIT, etc.) |
| `nc localhost PORT` | Cleaner connection tool than telnet |
| `gcc -Wall -Wextra` | Enable compile warnings |

## 15. Interview Questions Checklist

- ✅ Meaning of socket()'s three arguments?
- ✅ What is byte order? Why htons?
- ✅ Why cast to `sockaddr *` for bind's 2nd arg?
- ✅ What does listen's backlog control?
- ✅ SYN queue vs accept queue?
- ✅ Why does TIME_WAIT exist?
- ✅ Difference between SO_REUSEADDR and SO_REUSEPORT?
- ✅ How to handle accept() returning -1?
- ✅ What does read() returning 0 mean?
- ✅ When does SIGPIPE occur? How to handle?
- ⏳ select vs poll vs epoll? (Week 2)
- ⏳ epoll LT vs ET? (Week 2)
