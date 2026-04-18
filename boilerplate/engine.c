/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Architecture:
 *   - Supervisor: long-running daemon, manages containers, owns logging pipeline
 *   - CLI client: short-lived process, connects to supervisor via UNIX socket
 *
 * IPC mechanisms used:
 *   1. Pipes       - container stdout/stderr -> supervisor (logging path)
 *   2. UNIX socket - CLI client <-> supervisor (control plane)
 *
 * Concurrency:
 *   - metadata_lock (mutex) protects the container linked list
 *   - bounded_buffer uses mutex + two condition variables (not_full, not_empty)
 *   - A dedicated logger thread drains the bounded buffer to log files
 *   - A pipe-reader thread per container feeds stdout/stderr into the buffer
 *   - SIGCHLD reaps exited children; SIGINT/SIGTERM trigger orderly shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ constants */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 4096
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MONITOR_DEV         "/dev/container_monitor"

/* ------------------------------------------------------------------ types */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t     head;
    size_t     tail;
    size_t     count;
    int        shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

/* Arguments for the per-container pipe-reader thread */
typedef struct {
    int              read_fd;
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_args_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global pointer needed by signal handler */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ helpers */
static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <rootfs>\n"
        "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    errno = 0;
    unsigned long mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                  int argc, char *argv[], int start_index)
{
    for (int i = start_index; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            char *end = NULL;
            errno = 0;
            long nv = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' || nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid --nice value (expected -20..19): %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ bounded buffer */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    if ((rc = pthread_mutex_init(&buf->mutex, NULL)) != 0) return rc;
    if ((rc = pthread_cond_init(&buf->not_empty, NULL)) != 0) {
        pthread_mutex_destroy(&buf->mutex); return rc;
    }
    if ((rc = pthread_cond_init(&buf->not_full, NULL)) != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * bounded_buffer_push - producer side.
 * Blocks when full; returns -1 if shutdown is signalled.
 *
 * Race without lock: two producers could overwrite the same slot.
 * Mutex prevents that. not_full prevents wasted spin when buffer is full.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * bounded_buffer_pop - consumer side.
 * Blocks when empty; returns -1 when shutdown and buffer fully drained.
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    if (buf->count == 0 && buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ------------------------------------------------------------------ logger thread (consumer) */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        FILE *fp = fopen(path, "a");
        if (!fp) { perror("fopen log"); continue; }
        fwrite(item.data, 1, item.length, fp);
        fclose(fp);
    }
    return NULL;
}

/* ------------------------------------------------------------------ pipe-reader thread (producer) */
/*
 * One thread per container reads its stdout/stderr pipe and feeds the
 * bounded buffer. This keeps the supervisor accept() loop non-blocking.
 */
void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *pra = (pipe_reader_args_t *)arg;
    char raw[LOG_CHUNK_SIZE];
    ssize_t n;
    while ((n = read(pra->read_fd, raw, sizeof(raw))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pra->container_id, CONTAINER_ID_LEN - 1);
        memcpy(item.data, raw, (size_t)n);
        item.length = (size_t)n;
        bounded_buffer_push(pra->log_buffer, &item);
    }
    close(pra->read_fd);
    free(pra);
    return NULL;
}

/* ------------------------------------------------------------------ container child entrypoint */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* UTS namespace: give the container its own hostname */
    sethostname(cfg->id, strlen(cfg->id));

    /* Apply nice value for scheduling experiments */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Pivot into container filesystem */
    if (chroot(cfg->rootfs) != 0) { perror("chroot"); return 1; }
    if (chdir("/")           != 0) { perror("chdir");  return 1; }

    /* Mount /proc so the container has a working process table */
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);   /* non-fatal if it fails */

    /* Redirect stdout & stderr to the supervisor's logging pipe */
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    /* Try busybox sh first, fall back to /bin/sh */
    execl("/bin/busybox", "sh", "-c", cfg->command, NULL);
    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    perror("execl");
    return 1;
}

/* ------------------------------------------------------------------ kernel monitor helpers */

int register_with_monitor(int monitor_fd, const char *container_id,
                           pid_t host_pid,
                           unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    if (monitor_fd < 0) return 0;   /* module not loaded - skip silently */
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl MONITOR_REGISTER");
        return -1;
    }
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    if (monitor_fd < 0) return 0;
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) {
        perror("ioctl MONITOR_UNREGISTER");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ signal handlers */

/* SIGCHLD: reap all exited children without blocking */
static void sigchld_handler(int sig)
{
    (void)sig;
    if (!g_ctx) return;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *rec = g_ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                if (WIFEXITED(status)) {
                    rec->state     = CONTAINER_EXITED;
                    rec->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    rec->state       = CONTAINER_KILLED;
                    rec->exit_signal = WTERMSIG(status);
                }
                unregister_from_monitor(g_ctx->monitor_fd, rec->id, rec->host_pid);
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

/* SIGINT / SIGTERM: trigger orderly shutdown */
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ supervisor */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    if ((rc = pthread_mutex_init(&ctx.metadata_lock, NULL)) != 0) {
        errno = rc; perror("pthread_mutex_init"); return 1;
    }
    if ((rc = bounded_buffer_init(&ctx.log_buffer)) != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* 1) Open kernel monitor device (optional - gracefully skipped) */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "Warning: cannot open %s (%s) - memory limits disabled\n",
                MONITOR_DEV, strerror(errno));
    else
        printf("Kernel monitor device opened: %s\n", MONITOR_DEV);

    /* 2) Create UNIX domain control socket */
    struct sockaddr_un addr;
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }
    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    chmod(CONTROL_PATH, 0666);
    if (listen(ctx.server_fd, 5) < 0) { perror("listen"); return 1; }
    printf("Control socket ready: %s\n", CONTROL_PATH);

    /* 3) Install signal handlers */
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 4) Create log directory and start logger consumer thread */
    mkdir(LOG_DIR, 0755);
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }
    printf("Supervisor ready (rootfs: %s)\n", rootfs);

    /* 5) Main event loop */
    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!ctx.should_stop) perror("accept");
            break;
        }

        control_request_t  req;
        control_response_t res;
        memset(&req, 0, sizeof(req));
        memset(&res, 0, sizeof(res));

        if (read(client_fd, &req, sizeof(req)) <= 0) {
            perror("read request");
            close(client_fd);
            continue;
        }

        switch (req.kind) {

        /* -- ps -------------------------------------------------------- */
        case CMD_PS: {
            char buf[4096] = {0};
            int  off = 0;
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8s %-12s %-24s\n",
                            "ID", "PID", "STATE", "STARTED");
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = ctx.containers;
            while (c && off < (int)sizeof(buf) - 1) {
                char ts[24];
                struct tm *t = localtime(&c->started_at);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
                off += snprintf(buf + off, sizeof(buf) - off,
                                "%-16s %-8d %-12s %-24s\n",
                                c->id, c->host_pid,
                                state_to_string(c->state), ts);
                c = c->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            printf("%s", buf);
            res.status = 0;
            strncpy(res.message, buf, sizeof(res.message) - 1);
            break;
        }

        /* -- start ----------------------------------------------------- */
        case CMD_START: {
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                perror("pipe");
                res.status = -1;
                snprintf(res.message, sizeof(res.message), "pipe() failed");
                break;
            }

            child_config_t *cfg = calloc(1, sizeof(child_config_t));
            if (!cfg) {
                res.status = -1;
                snprintf(res.message, sizeof(res.message), "OOM");
                close(pipefd[0]); close(pipefd[1]);
                break;
            }
            strncpy(cfg->id,       req.container_id, CONTAINER_ID_LEN - 1);
            strncpy(cfg->rootfs,   req.rootfs,        PATH_MAX - 1);
            strncpy(cfg->command,  req.command,        CHILD_COMMAND_LEN - 1);
            cfg->nice_value   = req.nice_value;
            cfg->log_write_fd = pipefd[1];

            void *stack = malloc(STACK_SIZE);
            if (!stack) {
                res.status = -1;
                snprintf(res.message, sizeof(res.message), "malloc stack failed");
                free(cfg); close(pipefd[0]); close(pipefd[1]);
                break;
            }

            pid_t pid = clone(child_fn,
                              (char *)stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              cfg);
            if (pid < 0) {
                perror("clone");
                res.status = -1;
                snprintf(res.message, sizeof(res.message),
                         "clone() failed: %s", strerror(errno));
                free(stack); free(cfg);
                close(pipefd[0]); close(pipefd[1]);
                break;
            }

            close(pipefd[1]);   /* parent closes write end */

            /* Record metadata */
            container_record_t *rec = calloc(1, sizeof(container_record_t));
            if (rec) {
                strncpy(rec->id, req.container_id, CONTAINER_ID_LEN - 1);
                rec->host_pid         = pid;
                rec->state            = CONTAINER_RUNNING;
                rec->started_at       = time(NULL);
                rec->soft_limit_bytes = req.soft_limit_bytes;
                rec->hard_limit_bytes = req.hard_limit_bytes;
                snprintf(rec->log_path, sizeof(rec->log_path),
                         "%s/%s.log", LOG_DIR, req.container_id);

                pthread_mutex_lock(&ctx.metadata_lock);
                if (!ctx.containers) {
                    ctx.containers = rec;
                } else {
                    container_record_t *t = ctx.containers;
                    while (t->next) t = t->next;
                    t->next = rec;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            }

            /* Register with kernel memory monitor */
            register_with_monitor(ctx.monitor_fd,
                                   req.container_id, pid,
                                   req.soft_limit_bytes,
                                   req.hard_limit_bytes);

            /* Spawn pipe-reader thread so accept() loop stays responsive */
            pipe_reader_args_t *pra = calloc(1, sizeof(pipe_reader_args_t));
            if (pra) {
                pra->read_fd    = pipefd[0];
                pra->log_buffer = &ctx.log_buffer;
                strncpy(pra->container_id, req.container_id, CONTAINER_ID_LEN - 1);
                pthread_t reader_tid;
                if (pthread_create(&reader_tid, NULL, pipe_reader_thread, pra) != 0) {
                    perror("pthread_create pipe_reader");
                    close(pipefd[0]); free(pra);
                } else {
                    pthread_detach(reader_tid);
                }
            } else {
                close(pipefd[0]);
            }

            res.status = 0;
            snprintf(res.message, sizeof(res.message),
                     "Container '%s' started (PID %d)", req.container_id, pid);
            printf("%s\n", res.message);
            break;
        }

        /* -- run (foreground: blocks until container exits) ------------ */
        case CMD_RUN: {
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                res.status = -1;
                snprintf(res.message, sizeof(res.message), "pipe() failed");
                break;
            }

            child_config_t *cfg = calloc(1, sizeof(child_config_t));
            if (!cfg) {
                res.status = -1;
                snprintf(res.message, sizeof(res.message), "OOM");
                close(pipefd[0]); close(pipefd[1]);
                break;
            }
            strncpy(cfg->id,       req.container_id, CONTAINER_ID_LEN - 1);
            strncpy(cfg->rootfs,   req.rootfs,        PATH_MAX - 1);
            strncpy(cfg->command,  req.command,        CHILD_COMMAND_LEN - 1);
            cfg->nice_value   = req.nice_value;
            cfg->log_write_fd = pipefd[1];

            void *stack = malloc(STACK_SIZE);
            if (!stack) {
                res.status = -1;
                snprintf(res.message, sizeof(res.message), "malloc stack failed");
                free(cfg); close(pipefd[0]); close(pipefd[1]);
                break;
            }

            pid_t pid = clone(child_fn,
                              (char *)stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              cfg);
            if (pid < 0) {
                perror("clone");
                res.status = -1;
                snprintf(res.message, sizeof(res.message), "clone() failed");
                free(stack); free(cfg);
                close(pipefd[0]); close(pipefd[1]);
                break;
            }
            close(pipefd[1]);

            container_record_t *rec = calloc(1, sizeof(container_record_t));
            if (rec) {
                strncpy(rec->id, req.container_id, CONTAINER_ID_LEN - 1);
                rec->host_pid   = pid;
                rec->state      = CONTAINER_RUNNING;
                rec->started_at = time(NULL);
                snprintf(rec->log_path, sizeof(rec->log_path),
                         "%s/%s.log", LOG_DIR, req.container_id);
                pthread_mutex_lock(&ctx.metadata_lock);
                if (!ctx.containers) {
                    ctx.containers = rec;
                } else {
                    container_record_t *t = ctx.containers;
                    while (t->next) t = t->next;
                    t->next = rec;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            }
            register_with_monitor(ctx.monitor_fd, req.container_id, pid,
                                   req.soft_limit_bytes, req.hard_limit_bytes);

            /* Drain pipe while child runs */
            char raw[LOG_CHUNK_SIZE];
            ssize_t n;
            while ((n = read(pipefd[0], raw, sizeof(raw))) > 0) {
                log_item_t item;
                memset(&item, 0, sizeof(item));
                strncpy(item.container_id, req.container_id, CONTAINER_ID_LEN - 1);
                memcpy(item.data, raw, (size_t)n);
                item.length = (size_t)n;
                bounded_buffer_push(&ctx.log_buffer, &item);
            }
            close(pipefd[0]);

            int wstatus;
            waitpid(pid, &wstatus, 0);

            pthread_mutex_lock(&ctx.metadata_lock);
            if (rec) {
                rec->state     = CONTAINER_EXITED;
                rec->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            unregister_from_monitor(ctx.monitor_fd, req.container_id, pid);

            res.status = 0;
            snprintf(res.message, sizeof(res.message),
                     "Container '%s' exited (code %d)",
                     req.container_id,
                     WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);
            break;
        }

        /* -- stop ------------------------------------------------------ */
        case CMD_STOP: {
            int found_any = 0, found_running = 0;
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = ctx.containers;
            while (c) {
                if (strcmp(c->id, req.container_id) == 0) {
                    found_any = 1;
                    if (c->state == CONTAINER_RUNNING) {
                        found_running = 1;
                        if (kill(c->host_pid, SIGKILL) == 0) {
                            c->state = CONTAINER_STOPPED;
                            res.status = 0;
                            snprintf(res.message, sizeof(res.message),
                                     "Container '%s' stopped (PID %d)",
                                     c->id, c->host_pid);
                            printf("%s\n", res.message);
                            unregister_from_monitor(ctx.monitor_fd, c->id, c->host_pid);
                        } else {
                            perror("kill");
                            res.status = -1;
                            snprintf(res.message, sizeof(res.message),
                                     "kill() failed for '%s'", c->id);
                        }
                        break;
                    }
                }
                c = c->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            if (!found_any) {
                res.status = -1;
                snprintf(res.message, sizeof(res.message),
                         "Container '%s' not found", req.container_id);
            } else if (!found_running) {
                res.status = -1;
                snprintf(res.message, sizeof(res.message),
                         "Container '%s' is not running (already stopped/exited)",
                         req.container_id);
            }
            break;
        }

        /* -- logs ------------------------------------------------------ */
        case CMD_LOGS: {
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "%s/%s.log",
                     LOG_DIR, req.container_id);
            FILE *fp = fopen(log_path, "r");
            if (!fp) {
                res.status = -1;
                snprintf(res.message, sizeof(res.message),
                         "No log for '%s' (tried %s)", req.container_id, log_path);
                break;
            }
            size_t n = fread(res.message, 1, sizeof(res.message) - 1, fp);
            res.message[n] = '\0';
            fclose(fp);
            res.status = 0;
            break;
        }

        default:
            res.status = -1;
            snprintf(res.message, sizeof(res.message), "Unknown command");
            break;
        }

        if (write(client_fd, &res, sizeof(res)) < 0)
            perror("write response");
        close(client_fd);
    }

    /* ---------------------------------------------------------------- orderly shutdown */
    printf("\nSupervisor shutting down...\n");

    /* Kill all still-running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            kill(c->host_pid, SIGKILL);
            c->state = CONTAINER_STOPPED;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Reap any remaining children */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* Drain and stop logging pipeline */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *node = ctx.containers;
    while (node) {
        container_record_t *next = node->next;
        free(node);
        node = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.server_fd >= 0) { close(ctx.server_fd); unlink(CONTROL_PATH); }
    if (ctx.monitor_fd >= 0)  close(ctx.monitor_fd);

    printf("Supervisor exited cleanly.\n");
    g_ctx = NULL;
    return 0;
}

/* ------------------------------------------------------------------ CLI client */

static int send_control_request(const control_request_t *req)
{
    int sockfd;
    struct sockaddr_un addr;
    control_response_t res;

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(sockfd);
        return 1;
    }
    if (write(sockfd, req, sizeof(*req)) < 0) {
        perror("write"); close(sockfd); return 1;
    }
    memset(&res, 0, sizeof(res));
    if (read(sockfd, &res, sizeof(res)) > 0)
        printf("%s\n", res.message);
    else
        perror("read response");

    close(sockfd);
    return (res.status == 0) ? 0 : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <cmd> [opts]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,        argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,       argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <cmd> [opts]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,        argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,       argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);
    usage(argv[0]);
    return 1;
}
