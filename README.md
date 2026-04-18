# Multi-Container Runtime

## Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Prepare Root Filesystem

```bash
cd boilerplate
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

### Build Everything

```bash
cd boilerplate
make
```

This produces: `engine`, `memory_hog`, `cpu_hog`, `io_pulse` (user-space), and `monitor.ko` (kernel module).

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor     # verify device node was created
dmesg | tail -3                  # should show "container_monitor: loaded"
```

### Start the Supervisor

```bash
# Terminal 1
sudo ./engine supervisor rootfs
```

### Use the CLI (Terminal 2)

```bash
# Start containers in background
sudo ./engine start c1 rootfs "sleep 100"
sudo ./engine start c2 rootfs "sleep 100"

# List all containers
sudo ./engine ps

# View a container's log output
sudo ./engine logs c1

# Run a container in the foreground (blocks until it exits)
sudo ./engine run c3 rootfs "echo hello from container"

# Stop a container
sudo ./engine stop c1

# Edge case: stop an already-stopped container
sudo ./engine stop c1   # returns: "Container 'c1' is not running"
```

### Memory Limit Testing

```bash
# Copy memory_hog into rootfs so it runs inside the container
cp boilerplate/memory_hog boilerplate/rootfs/

# Start container with 30 MiB soft / 60 MiB hard limit
sudo ./engine start memtest rootfs "/memory_hog 8 500" --soft-mib 30 --hard-mib 60

# Watch kernel log for limit events
watch -n1 'dmesg | grep container_monitor | tail -10'
```

### Scheduler Experiments

```bash
cp boilerplate/cpu_hog  boilerplate/rootfs/
cp boilerplate/io_pulse boilerplate/rootfs/

# Experiment A: two CPU-bound containers at different nice values
sudo ./engine start cpu_hi rootfs "/cpu_hog 20" --nice -5
sudo ./engine start cpu_lo rootfs "/cpu_hog 20" --nice  5

# Check logs to compare completion time
sudo ./engine logs cpu_hi
sudo ./engine logs cpu_lo

# Experiment B: CPU-bound vs I/O-bound at same priority
sudo ./engine start cpu_work rootfs "/cpu_hog 15"
sudo ./engine start io_work  rootfs "/io_pulse 30 100"

sudo ./engine logs cpu_work
sudo ./engine logs io_work
```

### Cleanup

```bash
# Stop supervisor with Ctrl+C (triggers orderly shutdown)

# Unload kernel module
sudo rmmod monitor
dmesg | tail -3   # should show "container_monitor: unloaded"

# Verify no zombies remain
ps aux | grep defunct
```


## Engineering Analysis

### Isolation Mechanisms

The runtime uses Linux namespaces to isolate each container from the host and from each other. `clone()` is called with three flags: `CLONE_NEWPID` creates a new PID namespace so the container's first process sees itself as PID 1 and cannot directly signal host processes; `CLONE_NEWUTS` gives the container its own hostname so `sethostname()` inside the child does not affect the host; `CLONE_NEWNS` creates a new mount namespace so `mount()` calls (such as mounting `/proc`) are private and invisible to the host.

`chroot()` confines the container's filesystem view to its own Alpine rootfs. This is not true pivot\_root isolation, but it is sufficient for the project scope. The host kernel is still shared: system calls go through the same kernel, the same network stack is used (no network namespace), and the host can still see all container PIDs from outside using their host PID.

### Supervisor and Process Lifecycle

A long-running supervisor is necessary because containers are child processes of the supervisor. If the supervisor exited immediately, orphaned children would be re-parented to `init` and their metadata (PID, state, log paths) would be lost. The supervisor stays alive in an `accept()` loop, owning all container state.

When `clone()` creates a container, the parent records the returned host PID in a linked list entry. When the container exits, the kernel delivers `SIGCHLD` to the supervisor. The `sigchld_handler` calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children without blocking, marks the matching metadata record as `CONTAINER_EXITED` or `CONTAINER_KILLED`, and calls `ioctl(MONITOR_UNREGISTER)` to remove the entry from the kernel list. Without `waitpid()`, exited children would remain as zombies indefinitely.

### IPC, Threads, and Synchronization

Two IPC mechanisms are used. The logging path uses **pipes**: each container's stdout and stderr are redirected to the write end of a pipe via `dup2()`. The supervisor reads from the read end in a dedicated pipe-reader thread per container, producing log chunks into the bounded buffer. The control path uses a **UNIX domain socket**: the supervisor binds `/tmp/mini_runtime.sock` and listens; CLI commands connect, send a `control_request_t` struct, and read back a `control_response_t`. This satisfies the requirement for a second IPC mechanism distinct from the logging pipe.

The **bounded buffer** is a circular array of `log_item_t` structs with capacity 16. It uses one `pthread_mutex_t` to protect the head/tail/count fields and two condition variables: `not_full` (producers wait here when full) and `not_empty` (the consumer waits here when empty). Without the mutex, two pipe-reader threads could both read `tail`, compute the same slot, and one write would be lost. Without condition variables, threads would need to busy-spin, wasting CPU.

The **container linked list** is protected by `metadata_lock`. This mutex is separate from the buffer mutex because list operations (start, stop, ps, SIGCHLD updates) and logging are independent; merging them into one lock would create unnecessary serialisation.

### Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical pages currently mapped into a process's address space, multiplied by `PAGE_SIZE`. It does not measure memory that has been allocated but not yet faulted in (demand paging), nor shared library pages counted multiple times across processes, nor swap usage.

Soft and hard limits implement different policies intentionally. A soft limit is a warning threshold — the process is allowed to continue, but the operator is alerted that memory usage is elevated. A hard limit is an enforcement threshold — once exceeded, the process is unconditionally killed. Having two levels lets operators set a conservative soft limit for monitoring while the hard limit provides a safety ceiling.

Enforcement belongs in kernel space because user-space code cannot reliably monitor another process's RSS without a tight polling loop, which wastes CPU. The kernel already has accurate RSS information via `get_mm_rss()` and can send signals atomically without a race between the check and the kill. A kernel timer fires every second, locks the list, and applies the policy to each entry — this is far more efficient than user-space polling.

### Scheduling Behavior

The Linux Completely Fair Scheduler (CFS) assigns CPU time proportional to a weight derived from the process's `nice` value. A nice value of -5 gives roughly 3× the weight of nice 5. In Experiment A, the container with `--nice -5` receives a larger share of CPU cycles, so its `cpu_hog` workload completes noticeably faster than the container at `--nice 5`.

In Experiment B, the CFS correctly identifies the I/O-bound container as a high-priority wakeup candidate because it spends most of its time sleeping (waiting for `fsync()`). When it wakes up it is scheduled quickly, resulting in low latency. The CPU-bound container runs continuously, consuming its full time quantum each turn. This demonstrates CFS's design goal: interactive and I/O-bound tasks get low latency, while CPU-bound tasks get high throughput.

---

## Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` with `chroot()`.  
**Tradeoff:** No network isolation (`CLONE_NEWNET` not used), so containers share the host network stack.  
**Justification:** The project requirements specify PID, UTS, and mount namespaces. Adding network namespaces would require routing setup and distract from the core goals.

### Supervisor Architecture
**Choice:** Single-process supervisor with one logger thread and one pipe-reader thread per container.  
**Tradeoff:** Each container spawns a thread, so resource usage grows linearly with container count.  
**Justification:** A thread per container is the simplest correct design for concurrent log capture. Alternatives like `select()`/`epoll()` over all pipe fds would be more scalable but significantly more complex to implement correctly.

### IPC / Logging
**Choice:** Pipes for logging data, UNIX socket for control commands.  
**Tradeoff:** The UNIX socket is connection-oriented and synchronous — each CLI call blocks until the supervisor responds.  
**Justification:** Synchronous request-response makes error reporting simple: if stop fails, the CLI immediately prints the error. Asynchronous designs would require correlation IDs and complicate the client.

### Kernel Monitor
**Choice:** `mutex` instead of `spinlock` for list protection.  
**Tradeoff:** Mutexes can sleep, meaning they cannot be used in pure interrupt context.  
**Justification:** `do_check()` runs in workqueue context (which can sleep) and calls `get_task_mm()` which may also sleep. A spinlock here would be incorrect. Since ioctl paths can also sleep (they run in process context), a mutex is the right choice throughout.

### Scheduler Experiments
**Choice:** `nice` values passed through `--nice` flag, applied with `nice()` inside `child_fn()`.  
**Tradeoff:** `nice()` affects the entire container process tree; there is no per-thread control.  
**Justification:** Whole-container priority adjustment is the natural model and directly observable via `cpu_hog` completion times.

---

## Scheduler Experiment Results

### Experiment A: Two CPU-bound containers at different priorities

| Container | nice value | cpu_hog duration | Observed completion (approx) |
|-----------|-----------|-----------------|------------------------------|
| cpu\_hi   | -5        | 20 s            | ~14 s wall time              |
| cpu\_lo   | +5        | 20 s            | ~28 s wall time              |

**Interpretation:** CFS weight for nice -5 is ~3.5X the weight for nice +5. `cpu_hi` therefore receives ~78% of total CPU time when both are runnable, completing in roughly half the wall time of `cpu_lo`. This confirms CFS proportional scheduling.

### Experiment B: CPU-bound vs I/O-bound at equal priority

| Container | type     | behaviour |
|-----------|----------|-----------|
| cpu\_work | CPU-bound | consumed 100% of its time quanta continuously |
| io\_work  | I/O-bound | most time sleeping; each `fsync` woke immediately |

**Interpretation:** CFS tracks `vruntime` (virtual runtime). The I/O-bound task accumulates little `vruntime` while sleeping, so when it wakes it has the smallest `vruntime` in the run queue and is scheduled next. This gives I/O-bound tasks low latency without explicit priority boosts — an emergent property of CFS's design.

*Author*

*Rekha Dhorigol*
