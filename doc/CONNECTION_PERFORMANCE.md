# Connection Performance & Transport Architecture: onsoctcp vs. onipcstr

This document provides a technical analysis of database connection establishment in `libsqli`, comparing the **`onsoctcp`** (TCP/IP network socket) and **`onipcstr`** (UNIX domain stream pipe socket) transport mechanisms. It details empirical profiling results obtained using Linux `perf`, analyzes connection handshakes, identifies server- and client-side bottlenecks, and documents optimizations available in `libsqli` and the Informix server configuration (`onconfig`).

---

## 1. Transport Architectural Differences

Informix supports multiple communication protocols configured via `sqlhosts`. For local and containerized deployments, the two primary stream-based protocols are:

| Characteristic | `onsoctcp` (TCP Socket) | `onipcstr` (Stream Pipe) |
| :--- | :--- | :--- |
| **Address Family** | `AF_INET` / `AF_INET6` (`SOCK_STREAM`) | `AF_UNIX` (`SOCK_STREAM`) |
| **Rendezvous Point** | Host IP + TCP Port (e.g. `127.0.0.1:9088`) | Filesystem Inode on `tmpfs` (`/INFORMIXTMP/ol_ies.str`) |
| **Kernel Data Path** | Full TCP/IP stack (`tcp_sendmsg`, `ip_output`, bridge, checksumming, packet sequencing, softirq) | Direct in-kernel memory queues (`unix_stream_sendmsg` $\to$ `unix_stream_recvmsg`, pointer handoff via `sk_buff`) |
| **Handshake Sequence** | **Client initiates:** Client sends 72-byte `CONREQ` SL frame immediately after TCP 3-way handshake | **Server initiates:** Server sends 12-byte greeting frame (`0c 00 00 00 ...`), on which the client must wait |
| **Preamble Format** | Binary Session Layer (SL) header | Hybrid: `sqAZ...` dynamic magic, plain ASCII environment variables, `:` delimiter, and custom Base64-encoded binary payload (`0-9, A-Z, a-z, @, _`) |
| **Authentication** | Explicit user and password; server performs cryptographic hash verification (`libcrypt.so`) | Trusted local OS identity (kernel-verified UID/PID); no cryptographic hashing |
| **Socket Teardown** | 4-way TCP `FIN`/`ACK` handshake; socket transitions into `TIME_WAIT` (default 60s) | Immediate synchronous release of socket inode; no linger state or port exhaustion |

### The Role of `INFORMIXTMP` as `tmpfs`

For `onipcstr`, Informix creates its listening sockets under `/INFORMIXTMP/<server>.str`. In containerized and high-performance environments, `/INFORMIXTMP` is mounted as a **`tmpfs`** (RAM filesystem):
- **RAM-Backed Resolution:** Socket path lookups (`connect()`, `stat()`, `unlink()`) execute entirely within Linux VFS dcache/icache in RAM with zero block I/O, storage controller latency, or filesystem journaling overhead.
- **Clean Lifecycle:** Ephemeral sockets and VP stream pipes (`VP.<server>.<vpid>s`) are automatically wiped when the container namespace or tmpfs unmounts, preventing stale socket files from blocking subsequent engine starts.

---

## 2. Empirical Performance Measurements (`perf`)

Benchmarks were executed using `tools/sqli_bench_connect.c` compiled in Release mode (`-O2 -g -DNDEBUG`, sanitizers disabled) against an Informix 14.10 server.

### A. Client-Side Performance (200 Consecutive Connect/Close Cycles)

| Metric | `onsoctcp` (TCP) | `onipcstr` (IPC Stream) | Delta / Advantage |
| :--- | :--- | :--- | :--- |
| **Throughput** | 85.0 conn/s | **88.4 conn/s** | **+4.0% higher throughput** |
| **Total Wall Time** | 2.36 s | **2.26 s** | **100 ms faster** |
| **Client Kernel CPU (`sys`)** | 56.5 ms | **40.7 ms** | **27.9% less kernel CPU overhead** |
| **Connection Latency (Mean)** | 11.46 ms | **11.13 ms** | IPC faster |
| **Close Latency (Mean)** | 0.33 ms | **0.15 ms** | **IPC 2.2x faster teardown** |
| **L1-Data-Cache Miss Rate** | 5.36% (886,580) | **2.84% (368,185)** | **2.4x fewer L1 cache misses** |
| **Branch Miss Rate (`cpu_core`)** | 11.35% | **4.46%** | Lower branch mispredictions |

### B. Informix Server-Side Performance (300 Connect Cycles Across Engine VPs)

`perf stat` captured across Informix Virtual Processors (`cpu` VP 1, `str` VP 13, `soc` VP 14):

| Metric (Server VPs: CPU, STR, SOC) | `onsoctcp` (TCP) | `onipcstr` (IPC Stream) | Delta / Advantage |
| :--- | :--- | :--- | :--- |
| **Server CPU Task-Clock** | 744.4 ms | **526.7 ms** | **29.3% less CPU time** |
| **Instructions Retired** | **21.25 Billion** | **1.03 Billion** | **20.5x reduction in instructions!** |
| **Total CPU Cycles** | 6.94 Billion | **1.13 Billion** | **6.1x fewer CPU cycles** |
| **L1-Data-Cache Accesses** | 2.56 Billion | 124 Million | Dramatic memory bus reduction |
| **Cryptographic Overhead (`libcrypt`)** | Present (password hashing) | **None** (trusted OS auth) | Eliminates CPU hashing cost |

---

## 3. Microsecond Handshake Phase Breakdown

Instrumenting `src/sqli_handshake.c` with high-resolution clocks (`CLOCK_MONOTONIC_RAW`) reveals the per-phase latency breakdown during connection setup:

```
onsoctcp:
[HS_TIMING] proto=tlitcp total=11.49ms (sock=0.05ms conacc=2.73ms proto=8.01ms info=0.30ms dbopen=0.39ms)

onipcstr:
[HS_TIMING] proto=ipcstr total=11.72ms (sock=0.01ms conacc=10.85ms proto=0.32ms info=0.30ms dbopen=0.25ms)
```

### Phase Analysis:
1. **Socket Connect (`sock`):**
   - `onipcstr`: **0.01 ms (10 µs)** for `AF_UNIX` connect on tmpfs.
   - `onsoctcp`: **0.05 ms – 0.12 ms** for the TCP 3-way handshake (`SYN` $\to$ `SYN-ACK` $\to$ `ACK`).
2. **Initial Handshake (`conacc`):**
   - For TCP, `conacc` takes **2.7 ms** (server validates SL header and acknowledges).
   - For IPC, `conacc` takes **10.8 ms** (server reads the 340-byte Base64 preamble, verifies local credentials, and initialises internal session structures before returning CONACC).
3. **Capability Exchange (`proto`):**
   - For TCP, `proto` (`SQ_PROTOCOLS`) takes **8.0 ms** (Informix performs deferred session allocation, thread creation, and password validation here).
   - For IPC, `proto` takes only **0.32 ms**.
4. **Environment & Database Open (`info` & `dbopen`):**
   - Identical on both transports: **~0.30 ms** for `SQ_INFO` and **~0.30 ms** for `SQ_DBOPEN`.

Both transports require almost identical overall time (**~11.5 ms**). The server merely performs the heavy session onboarding at a different stage in the protocol sequence.

---

## 4. Connection Bottlenecks

Profiling reveals five distinct bottlenecks limiting connection setup:

### 1. Multi-Round-Trip Ping-Pong (Handshake Multi-RTT)
Establishing a connection requires five sequential synchronous request/response round-trips:
1. Socket connection / Server greeting
2. Connection request (`CONREQ` or Preamble) $\to$ `CONACC`
3. Capability exchange: `SQ_PROTOCOLS` $\to$ `SQ_PROTOCOLS` + `SQ_EOT`
4. Environment exchange: `SQ_INFO` $\to$ `SQ_EOT`
5. Database attachment: `SQ_DBOPEN` $\to$ `SQ_DONE` / `SQ_EOT`

Each round-trip incurs kernel socket buffering, context switching, and engine scheduler dispatch.

### 2. Informix Server Multi-Threading (MT) & Inter-VP Semaphore Overhead
In `perf report`, the dominant hotspots inside `oninit` are:
- `libc.so.6: __memset_avx2_unaligned_erms` (**35% to 52% of server CPU time**): The engine zeros new thread stacks and Session Control Blocks (SCB) on every connection.
- `__semtimedop` / `do_semtimedop`: Inter-VP synchronization. Default Informix instances run with a single CPU VP (`VPCLASS cpu,num=1`). The network listeners run on dedicated Network VPs (`soc` VP 14 for TCP, `str` VP 13 for IPC). When a connection arrives, the NET VP must signal the CPU VP via System V semaphores and wait for user thread allocation.
- `mt_yield` / `mt_test_primitive_lock`: Cooperative scheduler yielding and thread switching.

### 3. Cryptographic Password Hashing on TCP
For `onsoctcp`, `perf report` identifies significant CPU time in `libcrypt.so.1.1.0`. The engine computes password hashes on each connection. On `onipcstr`, this cost is zero because the server uses trusted OS process credentials.

### 4. TCP Teardown & Ephemeral Port Exhaustion (`TIME_WAIT`)
When client applications open and close TCP connections at high frequencies without pooling, sockets enter the 60-second `TIME_WAIT` state. Under heavy churn, this exhausts ephemeral port ranges (1024–65535) and inflates kernel socket table lookups.

### 5. Virtual Network & Container Bridge Routing
When connecting to containerized instances via TCP port forwarding (`0.0.0.0:9088`), packets traverse host iptables/netfilter, bridge virtual interfaces (`cni-podman0`), and NAT translation. Co-located container deployments sharing the `/INFORMIXTMP` volume bypass network translation entirely.

---

## 5. Client-Side Optimizations in `libsqli`

The following optimizations are implemented in `libsqli` to reduce connection latency and avoid socket resource leaks:

### 1. `TCP_NODELAY` & `TCP_QUICKACK`
- **`TCP_NODELAY`:** Enabled in `sqli_tcp_posix.c` via `setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, ...)` to disable Nagle's algorithm and ensure small handshake frames are transmitted immediately without buffering.
- **`TCP_QUICKACK`:** Linux defaults to Delayed ACKs (holding acknowledgments for up to 40 ms to piggyback data). Enabling `TCP_QUICKACK` forces the kernel to acknowledge handshake packets immediately:
  ```c
  #ifdef TCP_QUICKACK
      int quickack = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
  #endif
  ```

### 2. Batched Disconnect Pipelining
In `src/sqli_handshake.c`, graceful disconnection previously issued two separate `sendto()` syscalls for `SQ_DBCLOSE` (2 bytes) and `SQ_EXIT` (2 bytes). Batching them into a single 4-byte buffer:
```c
uint8_t close_buf[4];
size_t cpos = 0;
if (conn->database_open) {
    close_buf[cpos++] = 0;
    close_buf[cpos++] = SQLI_SQ_DBCLOSE;
}
close_buf[cpos++] = 0;
close_buf[cpos++] = SQLI_SQ_EXIT;
(void)sqli_tcp_send(conn->socket_fd, close_buf, cpos);
```
- **Result:** Teardown latency dropped from **0.33 ms to 0.098 ms (98 µs, over 3x faster)** and throughput increased from **85.6 to 93.7 connects/sec (+9.4%)**.

### 3. `TIME_WAIT` Mitigation via `SO_LINGER`
For applications requiring rapid connection churn without connection pooling, `libsqli` supports zero-time teardown via `SO_LINGER` (enabled via environment variable `SQLI_TCP_NO_TIMEWAIT=1`):
```c
struct linger sl = { .l_onoff = 1, .l_linger = 0 };
setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
```
When `close(fd)` is called, the kernel terminates the connection with a `RST` frame instead of a 4-way FIN handshake. The socket is destroyed immediately with **0 seconds in `TIME_WAIT`**, completely preventing ephemeral port leaks.

### 4. Connection Pooling (`sqli_pool_t`)
The most effective way to eliminate the 11.5 ms connection setup latency is reusing connections through `libsqli`'s thread-safe connection pool (`include/libsqli/sqli.h`):
- Acquiring an existing connection from `sqli_pool_get()` takes **< 0.001 ms (< 1 µs)**.
- Reusing connections completely bypasses the 5-round-trip handshake, password hashing, and SCB initialization.

---

## 6. Server-Side Optimization Recommendations (`onconfig`)

When modifying server code is not possible, the following configuration adjustments in Informix's `onconfig` file directly mitigate the identified bottlenecks:

### 1. Assign Poll Threads Directly to the CPU VP (`NETTYPE`)
By default, Informix allocates `soctcp` and `ipcstr` to separate Network Virtual Processors (`soc` and `str`), forcing every connection event through System V IPC semaphores (`__semtimedop`).
Adding explicit `NETTYPE` directives assigns network polling directly to the CPU VP:
```text
NETTYPE soctcp,1,50,CPU
NETTYPE ipcstr,1,50,CPU
```
*Effect:* Eliminates inter-VP context switches and semaphore wait states during connection onboarding.

### 2. Parallelize Engine Processing (`VPCLASS cpu`)
Instances configured without explicit CPU VPs run with a single thread (`VPCLASS cpu,num=1`), which serializes session onboarding, memory allocation, and query execution.
Increasing CPU VPs allows concurrent connection handshakes:
```text
VPCLASS cpu,num=2
```
*(or `num=4` matching available physical CPU cores).*

### 3. Enable Private VP Memory Caching (`VP_MEMORY_CACHE_KB`)
Informix defaults to `VP_MEMORY_CACHE_KB 0` (disabled). Enabling private VP memory caching:
```text
VP_MEMORY_CACHE_KB 16384
```
*Effect:* Allows CPU VPs to allocate session structures from a pre-allocated memory pool rather than obtaining and locking global shared memory segments on every connect.

### 4. Optimize Single-VP Mode (`SINGLE_CPU_VP`)
If the server must run with exactly one CPU VP, set:
```text
SINGLE_CPU_VP 1
```
*Effect:* Instructs the engine to omit inter-VP mutex spinlocks and atomic primitives in single-threaded environments.
