# Week 2 技术要点 / Week 2 Technical Summary

> HTTP Server with epoll — Week 2: 多路复用 / I/O Multiplexing with epoll

---

# 📘 中文版

## 1. 为什么需要多路复用

Week 1 的单线程阻塞模型：`read` 阻塞时，其他客户端只能干等。三条出路：

| 方案 | 思路 | 问题 |
|---|---|---|
| 多进程/多线程 | 每连接一个线程 | 上万连接 = 上万线程，内存爆炸、上下文切换拖垮 CPU |
| 非阻塞 + 轮询 | 所有 fd 设非阻塞，循环挨个 read | 没数据时空转，CPU 100% |
| **I/O 多路复用** | 让内核告诉你哪些 fd 就绪，没事干就睡 | ✅ select / poll / epoll |

多路复用核心诉求：**一个线程看着上千个 fd，谁就绪处理谁，全空闲时睡眠，不空转。**

## 2. 阻塞 vs 非阻塞 I/O

```c
// 阻塞(默认): 没数据 → 进程睡眠直到数据到达
// 非阻塞: 没数据 → 立刻返回 -1, errno == EAGAIN/EWOULDBLOCK
```

设置非阻塞的标准写法（先读再或上再写回，不能冲掉其他 flag）：

```c
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

非阻塞本身不解决问题（仍需空转轮询），它是 epoll 的搭档：epoll 负责"通知谁就绪"，非阻塞负责"去 read 时不被卡住"。

## 3. select / poll / epoll 演进

| | select / poll | epoll |
|---|---|---|
| 状态 | 无状态：每次重传整个 fd 集合 | 有状态：fd 集合常驻内核 |
| 就绪判断 | 每次遍历所有 fd 逐个检查 | 回调在数据到达时增量挂入就绪链表 |
| 拷贝 | 每次整个集合在用户/内核态拷来拷去 | 注册一次，之后只拷就绪的 |
| 复杂度 | O(总 fd 数) 每次调用 | O(就绪 fd 数) 每次调用 |
| fd 上限 | select 有 1024 限制 | 无硬性限制 |

一句话：select/poll 靠"现场点名"，epoll 是"谁就绪谁举手"。

## 4. epoll 底层原理

epoll **不主动监听/扫描**，它是**回调驱动、被动**的。

**数据结构**：

```
struct eventpoll  ← 一个 epoll 实例
    ├── rbr      : 红黑树根   ── 监听集合 (interest list)
    ├── rdllist  : 双向链表   ── 就绪链表 (ready list)
    └── wq       : 等待队列   ── 阻塞在 epoll_wait 的进程

struct epitem     ← 每个被监听的 fd 一个
    ├── rbn      : 红黑树节点
    ├── rdllink  : 就绪链表节点   (同一对象可同时挂在两处)
    └── event    : 注册的 epoll_event
```

**`epoll_ctl ADD` 做的事**：① 创建 epitem 插入红黑树；② **在该 fd 的等待队列挂一个回调 `ep_poll_callback`**；③ 做一次初始就绪检查。

**数据到达链路**：

```
网卡收包 → 硬件中断 → 协议栈处理 → 数据进 socket 接收缓冲区
→ 遍历该 socket 的等待队列 → 执行 ep_poll_callback
→ 把该 fd 的 epitem 挂进就绪链表 + 唤醒阻塞在 epoll_wait 的进程
```

**`epoll_wait` 做的事**：看就绪链表 —— 非空就拷给用户返回；空就把进程挂进 wq 睡眠。

精髓：复用 socket 本来就有的等待队列唤醒机制，把"唤醒进程"替换成"执行回调"。全程无扫描。

## 5. 三个核心 API

```c
int epfd = epoll_create1(0);                              // 创建实例
int epoll_ctl(epfd, op, fd, struct epoll_event *event);   // op: ADD/MOD/DEL
int n = epoll_wait(epfd, events, maxevents, timeout);     // 等就绪事件
```

```c
struct epoll_event {
    uint32_t      events;   // EPOLLIN / EPOLLOUT / EPOLLET ...
    epoll_data_t  data;     // 用户数据,通常存 data.fd
};
```

`data` 字段是桥梁：注册时把 fd 存进去，`epoll_wait` 返回时内核原样还给你，靠它认出"是哪个 fd 就绪"。

**ev vs events**：

| | `ev`（传给 epoll_ctl） | `events`（传给 epoll_wait） |
|---|---|---|
| 类型 | 单个 struct | struct 数组 |
| 方向 | 输入：你 → 内核 | 输出：内核 → 你 |
| 比喻 | 递进去、写着需求的盘子 | 递出来、盛着结果的盘子 |
| 数量 | epoll_ctl 一次操作一个 fd | epoll_wait 一次返回多个就绪 fd |

`ev` 内容被内核拷走，所以可反复重填注册不同 fd。

## 6. LT vs ET（全称 + 机制）

- **LT** = Level Triggered 水平触发 —— 状态为真就持续通知
- **ET** = Edge Triggered 边缘触发 —— 只在状态跳变的边沿通知一次

术语借自数字电路：Level = 信号当前是高/低（持续状态）；Edge = 高低跳变的瞬间。

**机制层真相**：`epoll_wait` 取走就绪 epitem 后——
- LT：再检查一次 fd 是否仍就绪，仍就绪就**重新挂回就绪链表** → 下次还报告
- ET：取走就完了，只有新事件（新边沿）才会重新挂入 → 只通知一次

| | LT（默认） | ET（加 EPOLLET） |
|---|---|---|
| 通知 | 缓冲区有数据就每次都报告 | 只在数据到达边沿报告一次 |
| 你可以 | 一次只读一部分，下次会再提醒 | 必须一次读干（循环到 EAGAIN） |
| 非阻塞 | 可选 | **强制**（drain 循环最后一次 read 否则永久阻塞） |
| 难度 | 简单容错 | 易漏数据，难调试 |

## 7. LT 行为的可验证特性（实验）

| 实验 | 验证的 LT 特性 |
|---|---|
| 一次只读 1 字节，发 6 字节 | LT 触发 6 次唤醒（缓冲区有剩余就反复通知） |
| 单次 accept，快速开多个 nc | accept 队列非空 → 持续通知 server_fd，单次 accept 不丢连接 |
| 给 client 注册 EPOLLOUT | "可写"长期满足 → epoll_wait 疯狂空转，CPU 100% |

最后一个的设计启示：**LT 下不要长期注册 EPOLLOUT**，应按需开启——write 遇 EAGAIN 才 MOD 加上，数据发完再 MOD 去掉。

## 8. LT → ET 改造清单

1. **加 flag**：`server_fd` 和 `client_fd` 注册都用 `EPOLLIN | EPOLLET`
2. **read 循环到 EAGAIN**：一次通知必须把 fd 榨干
3. **accept 也循环到 EAGAIN**：最易漏；server_fd 是 ET，一瞬多个连接只通知一次
4. **非阻塞从可选变强制**：drain 循环最后一次 read 在阻塞 fd 上会永久卡死
5. **错误处理必须改**：`EAGAIN` 是 drain 循环的**正常退出**，不是错误，绝不能 close

ET drain 循环正确写法：

```c
while (1) {
    ssize_t cnt = read(fd, buf, BUF_SIZE);
    if (cnt > 0) {
        write(fd, buf, cnt);
    } else if (cnt == 0) {              // 对端关闭
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        break;
    } else {                            // cnt < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读干净了,正常退出
        if (errno == EINTR) continue;                        // 信号打断,重试
        perror("read");                                      // 真错误
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        break;
    }
}
```

## 9. 错误码辨析

| 错误码 | 含义 | 出现场景 | 处理 |
|---|---|---|---|
| `EAGAIN` / `EWOULDBLOCK` | "现在没活干" | 非阻塞 read/accept/write 无法立即完成 | drain 循环里当**正常退出**，不 close |
| `EINTR` | 被信号打断 | 阻塞调用（如 `epoll_wait`）执行中收到信号 | `continue` 重试 |

- `EWOULDBLOCK` 在 Linux 上 `#define` 等于 `EAGAIN`（同值 11），名字差异是 BSD vs System V 的历史包袱；POSIX 允许不同值，跨平台代码两个都判。
- `EAGAIN` 是非阻塞 I/O 的"没活干"；`EINTR` 是阻塞调用被信号打断。两者别混。
- 这些常量都在 `<errno.h>`。

## 10. EPOLLOUT 何时用

`write` 没法一次写完，是因为**内核发送缓冲区满了**（对端读得慢）：短写或返回 `EAGAIN`。

正确做法不是空转重试 write，而是：① 把没发完的数据存进该连接的输出缓冲区；② `epoll_ctl MOD` 加 `EPOLLOUT`；③ 回到 `epoll_wait`；④ 缓冲区腾空后 epoll 报告 `EPOLLOUT`，再续写；全发完后 MOD 去掉 `EPOLLOUT`。

状态机：有待发数据 ↔ EPOLLOUT 开；数据发完 ↔ EPOLLOUT 关。（基础 echo server 用不到，Week 5 压测再处理。）

## 11. 事件循环结构（与 Week 1 对比）

Week 1：`while(1){ accept(); while(1){read/write} }` —— 一次只盯一个 fd

epoll：单个 `epoll_wait` 同时盯所有 fd，扁平事件循环：

```
epfd = epoll_create1(0)
epoll_ctl(ADD, server_fd)
while (1) {
    n = epoll_wait(epfd, events, ...)
    for 每个就绪事件:
        if fd == server_fd:  循环 accept + 注册新连接
        else:                drain 循环 read + echo
}
```

心智转变：没有"专伺候一个客户端的内层循环"，所有连接在一个扁平循环里平等推进，谁也不卡谁。

## 12. 面试题清单

- ✅ epoll 和 select/poll 的区别？→ 无状态 vs 有状态、拷贝、扫描、上限
- ✅ epoll 为什么高效？→ 红黑树 + 就绪链表 + 回调，无扫描，O(就绪数)
- ✅ epoll 底层数据结构？→ 红黑树（监听集）+ 双向链表（就绪链表）
- ✅ LT 和 ET 的区别？→ 持续通知 vs 边沿通知一次；机制是"取走后是否重新入队"
- ✅ ET 为什么必须配非阻塞？→ drain 循环最后一次 read 否则永久阻塞
- ✅ ET 为什么必须循环读到 EAGAIN？→ 只通知一次，不读干会漏数据
- ✅ 阻塞 vs 非阻塞 I/O 的区别？
- ✅ EAGAIN 和 EINTR 的区别？

---

---

# 📗 English Version

## 1. Why I/O Multiplexing

Week 1's single-threaded blocking model: while `read` blocks, other clients just wait. Three ways out:

| Approach | Idea | Problem |
|---|---|---|
| Multi-process/thread | One thread per connection | 10k connections = 10k threads; memory explosion, context-switch overhead |
| Non-blocking + polling | Set all fds non-blocking, loop-read each | Busy-spin when no data, CPU 100% |
| **I/O multiplexing** | Let the kernel tell you which fds are ready; sleep when idle | ✅ select / poll / epoll |

Core need: **one thread watches thousands of fds, handles whoever is ready, sleeps when all idle, no busy-spin.**

## 2. Blocking vs Non-blocking I/O

```c
// Blocking (default): no data → process sleeps until data arrives
// Non-blocking: no data → returns -1 immediately, errno == EAGAIN/EWOULDBLOCK
```

Standard way to set non-blocking (read-then-OR-then-write-back, don't clobber other flags):

```c
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

Non-blocking alone doesn't solve the problem (still need busy-polling). It's epoll's partner: epoll handles "who is ready," non-blocking ensures "read won't get stuck."

## 3. select / poll / epoll Evolution

| | select / poll | epoll |
|---|---|---|
| State | Stateless: resend the whole fd set each call | Stateful: fd set stays resident in kernel |
| Readiness check | Scans all fds each call | Callbacks incrementally add ready fds to ready list |
| Copying | Whole set copied between user/kernel each call | Register once, then only copy ready ones |
| Complexity | O(total fds) per call | O(ready fds) per call |
| fd limit | select capped at 1024 | No hard limit |

In short: select/poll do a "roll call"; epoll has "whoever is ready raises their hand."

## 4. epoll Internals

epoll does **not actively monitor/scan** — it is **callback-driven and passive**.

**Data structures**:

```
struct eventpoll  ← one epoll instance
    ├── rbr      : red-black tree root  ── interest list
    ├── rdllist  : doubly linked list   ── ready list
    └── wq       : wait queue           ── processes blocked in epoll_wait

struct epitem     ← one per monitored fd
    ├── rbn      : red-black tree node
    ├── rdllink  : ready list node      (same object can sit in both)
    └── event    : the registered epoll_event
```

**What `epoll_ctl ADD` does**: ① create an epitem, insert into the red-black tree; ② **register a callback `ep_poll_callback` on that fd's wait queue**; ③ do one initial readiness check.

**Data-arrival chain**:

```
NIC receives packet → hardware interrupt → network stack processing
→ data into the socket's receive buffer
→ walk the socket's wait queue → run ep_poll_callback
→ link the fd's epitem into the ready list + wake the process blocked in epoll_wait
```

**What `epoll_wait` does**: check the ready list — non-empty → copy to user and return; empty → put process on `wq` and sleep.

The essence: reuse the socket's existing wait-queue wakeup machinery, swapping "wake a process" for "run a callback." No scanning anywhere.

## 5. The Three Core APIs

```c
int epfd = epoll_create1(0);                              // create instance
int epoll_ctl(epfd, op, fd, struct epoll_event *event);   // op: ADD/MOD/DEL
int n = epoll_wait(epfd, events, maxevents, timeout);     // wait for ready events
```

```c
struct epoll_event {
    uint32_t      events;   // EPOLLIN / EPOLLOUT / EPOLLET ...
    epoll_data_t  data;     // user data, usually data.fd
};
```

The `data` field is the bridge: store the fd at registration, the kernel hands it back unchanged in `epoll_wait`, letting you identify which fd is ready.

**ev vs events**:

| | `ev` (passed to epoll_ctl) | `events` (passed to epoll_wait) |
|---|---|---|
| Type | Single struct | Array of structs |
| Direction | Input: you → kernel | Output: kernel → you |
| Analogy | The plate you hand in with your order note | The plate handed back holding the result |
| Quantity | epoll_ctl handles one fd per call | epoll_wait returns many ready fds at once |

The kernel copies `ev`'s contents, so you can reuse it to register different fds.

## 6. LT vs ET (Full Names + Mechanism)

- **LT** = Level Triggered — keeps notifying as long as the condition holds
- **ET** = Edge Triggered — notifies only once, on the edge where state changes

Terms borrowed from digital electronics: Level = whether the signal is currently high/low (a sustained state); Edge = the instant of a high/low transition.

**Mechanism-level truth**: after `epoll_wait` removes a ready epitem —
- LT: re-checks whether the fd is still ready, and if so **re-adds it to the ready list** → reported again next time
- ET: just removes it; only a new event (new edge) re-adds it → notified only once

| | LT (default) | ET (add EPOLLET) |
|---|---|---|
| Notification | Reports every time as long as buffer has data | Reports once, on the data-arrival edge |
| You may | Read partially, will be reminded next time | Must drain in one go (loop to EAGAIN) |
| Non-blocking | Optional | **Mandatory** (else the drain loop's last read blocks forever) |
| Difficulty | Simple, forgiving | Easy to miss data, hard to debug |

## 7. Verifiable LT Behaviors (Experiments)

| Experiment | LT property verified |
|---|---|
| Read 1 byte at a time, send 6 bytes | LT fires 6 wakeups (keeps notifying while buffer has leftover) |
| Single accept, open many nc quickly | Accept queue non-empty → server_fd keeps being reported, single accept loses no connection |
| Register EPOLLOUT on a client | "Writable" holds continuously → epoll_wait busy-spins, CPU 100% |

Design takeaway from the last one: **don't keep EPOLLOUT registered under LT** — enable it on demand: MOD it on when write hits EAGAIN, MOD it off once data is sent.

## 8. LT → ET Conversion Checklist

1. **Add the flag**: register both `server_fd` and `client_fd` with `EPOLLIN | EPOLLET`
2. **Loop read to EAGAIN**: one notification must drain the fd fully
3. **Loop accept to EAGAIN too**: most-forgotten; server_fd is ET, a burst of connections is notified only once
4. **Non-blocking goes from optional to mandatory**: the drain loop's last read would block forever on a blocking fd
5. **Error handling must change**: `EAGAIN` is the **normal exit** of the drain loop, not an error — never close on it

Correct ET drain loop:

```c
while (1) {
    ssize_t cnt = read(fd, buf, BUF_SIZE);
    if (cnt > 0) {
        write(fd, buf, cnt);
    } else if (cnt == 0) {              // peer closed
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        break;
    } else {                            // cnt < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained, normal exit
        if (errno == EINTR) continue;                        // signal interrupt, retry
        perror("read");                                      // real error
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        break;
    }
}
```

## 9. Error Code Distinctions

| Error code | Meaning | When it occurs | Handling |
|---|---|---|---|
| `EAGAIN` / `EWOULDBLOCK` | "Nothing to do right now" | Non-blocking read/accept/write can't complete immediately | Treat as **normal exit** in the drain loop, don't close |
| `EINTR` | Interrupted by signal | A blocking call (e.g. `epoll_wait`) receives a signal mid-call | `continue` to retry |

- `EWOULDBLOCK` is `#define`d equal to `EAGAIN` on Linux (same value, 11); the name difference is BSD-vs-System-V historical baggage. POSIX allows them to differ, so portable code checks both.
- `EAGAIN` = "nothing to do" for non-blocking I/O; `EINTR` = blocking call interrupted by a signal. Don't conflate them.
- These constants live in `<errno.h>`.

## 10. When to Use EPOLLOUT

`write` failing to send everything at once means the **kernel send buffer is full** (peer reading slowly): a short write or `EAGAIN`.

The correct approach is NOT busy-retrying write, but: ① stash the unsent data in a per-connection output buffer; ② `epoll_ctl MOD` to add `EPOLLOUT`; ③ return to `epoll_wait`; ④ once the buffer drains, epoll reports `EPOLLOUT`, then continue writing; once fully sent, MOD `EPOLLOUT` off.

State machine: has pending data ↔ EPOLLOUT on; data fully sent ↔ EPOLLOUT off. (Not needed for a basic echo server; handle it during Week 5 load testing.)

## 11. Event Loop Structure (vs Week 1)

Week 1: `while(1){ accept(); while(1){read/write} }` — watches only one fd at a time

epoll: a single `epoll_wait` watches all fds, a flat event loop:

```
epfd = epoll_create1(0)
epoll_ctl(ADD, server_fd)
while (1) {
    n = epoll_wait(epfd, events, ...)
    for each ready event:
        if fd == server_fd:  loop accept + register new connections
        else:                drain-loop read + echo
}
```

Mental shift: no more "inner loop dedicated to one client" — all connections advance equally in one flat loop, none blocks another.

## 12. Interview Questions Checklist

- ✅ Difference between epoll and select/poll? → stateless vs stateful, copying, scanning, fd limit
- ✅ Why is epoll efficient? → red-black tree + ready list + callbacks, no scanning, O(ready count)
- ✅ epoll's underlying data structures? → red-black tree (interest set) + doubly linked list (ready list)
- ✅ Difference between LT and ET? → continuous vs single edge notification; mechanism = whether the epitem is re-added after removal
- ✅ Why must ET use non-blocking fds? → the drain loop's last read would block forever otherwise
- ✅ Why must ET loop read to EAGAIN? → notified only once; not draining loses data
- ✅ Difference between blocking and non-blocking I/O?
- ✅ Difference between EAGAIN and EINTR?
