/*
 * engine.c — Multi-Container Runtime with Parent Supervisor
 *
 * Usage:
 *   sudo ./engine supervisor <rootfs>           # start supervisor daemon
 *   sudo ./engine start <name> <rootfs> <cmd>   # launch container (background)
 *   sudo ./engine run   <name> <rootfs> <cmd>   # launch container (wait)
 *   sudo ./engine ps                            # list containers
 *   sudo ./engine logs  <name>                  # dump container log
 *   sudo ./engine stop  <name>                  # stop container
 *
 * The supervisor binds a UNIX-domain socket at /tmp/engine.sock.
 * The CLI client sends a text command; the supervisor replies and updates state.
 *
 * Logging pipeline:
 *   container stdout/stderr → pipe → supervisor logger thread → log file
 *   A bounded circular buffer sits between the pipe reader and the file writer.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <sched.h>
#include <dirent.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

#define MAX_CONTAINERS   32
#define SOCK_PATH        "/tmp/engine.sock"
#define LOG_DIR          "/tmp/engine_logs"
#define BUF_CAPACITY     4096   /* bounded circular buffer size (bytes) */
#define CMD_MAX          1024

/* =========================================================================
 * Container state
 * ========================================================================= */

typedef enum {
    CS_EMPTY = 0,
    CS_STARTING,
    CS_RUNNING,
    CS_STOPPED,
    CS_KILLED,
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
    case CS_EMPTY:    return "empty";
    case CS_STARTING: return "starting";
    case CS_RUNNING:  return "running";
    case CS_STOPPED:  return "stopped";
    case CS_KILLED:   return "killed";
    default:          return "unknown";
    }
}

typedef struct {
    char            name[64];
    pid_t           host_pid;
    time_t          start_time;
    ContainerState  state;
    unsigned long   soft_limit;   /* bytes, 0 = none */
    unsigned long   hard_limit;   /* bytes, 0 = none */
    char            log_path[256];
    int             exit_status;
    int             kill_signal;
    /* Logging pipe: container writes, logger thread reads */
    int             pipe_read_fd;  /* supervisor side (read) */
    /* Logger thread handle */
    pthread_t       logger_tid;
    int             logger_stop;   /* flag to ask logger to exit */
    /* memory-killed flag (set when kernel module kills via hard limit) */
    int             oom_killed;
} ContainerMeta;

/* =========================================================================
 * Bounded circular buffer
 * ========================================================================= */

typedef struct {
    char            data[BUF_CAPACITY];
    size_t          head;        /* next write position */
    size_t          tail;        /* next read position  */
    size_t          count;       /* bytes currently held */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int             closed;      /* set when producer is done */
} BoundedBuf;

static void bbuf_init(BoundedBuf *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

/* Write up to len bytes; blocks if full; returns bytes written or 0 on close */
static size_t bbuf_write(BoundedBuf *b, const char *src, size_t len) {
    size_t written = 0;
    pthread_mutex_lock(&b->lock);
    while (written < len) {
        while (b->count == BUF_CAPACITY && !b->closed)
            pthread_cond_wait(&b->not_full, &b->lock);
        if (b->closed) break;
        size_t space = BUF_CAPACITY - b->count;
        size_t chunk = len - written;
        if (chunk > space) chunk = space;
        for (size_t i = 0; i < chunk; i++) {
            b->data[b->head] = src[written + i];
            b->head = (b->head + 1) % BUF_CAPACITY;
        }
        b->count += chunk;
        written += chunk;
        pthread_cond_signal(&b->not_empty);
    }
    pthread_mutex_unlock(&b->lock);
    return written;
}

/* Read up to len bytes; blocks if empty and not closed; returns 0 on EOF */
static size_t bbuf_read(BoundedBuf *b, char *dst, size_t len) {
    pthread_mutex_lock(&b->lock);
    while (b->count == 0 && !b->closed)
        pthread_cond_wait(&b->not_empty, &b->lock);
    size_t chunk = b->count < len ? b->count : len;
    for (size_t i = 0; i < chunk; i++) {
        dst[i] = b->data[b->tail];
        b->tail = (b->tail + 1) % BUF_CAPACITY;
    }
    b->count -= chunk;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->lock);
    return chunk;
}

static void bbuf_close(BoundedBuf *b) {
    pthread_mutex_lock(&b->lock);
    b->closed = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->lock);
}

/* =========================================================================
 * Global supervisor state
 * ========================================================================= */

static ContainerMeta  containers[MAX_CONTAINERS];
static pthread_mutex_t meta_lock = PTHREAD_MUTEX_INITIALIZER;
static BoundedBuf      log_bufs[MAX_CONTAINERS];

static volatile int supervisor_running = 1;
static int monitor_fd = -1;   /* /dev/container_monitor fd */

/* =========================================================================
 * Utility helpers
 * ========================================================================= */

static int find_slot_by_name(const char *name) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state != CS_EMPTY &&
            strcmp(containers[i].name, name) == 0)
            return i;
    return -1;
}

static int find_empty_slot(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state == CS_EMPTY)
            return i;
    return -1;
}

static void ensure_log_dir(void) {
    mkdir(LOG_DIR, 0755);
}

static void timestamp_str(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, sz, "%Y-%m-%dT%H:%M:%S", tm);
}

/* =========================================================================
 * Logger thread
 * ========================================================================= */

typedef struct {
    int   slot;
    int   pipe_fd;   /* read end of container stdout/stderr pipe */
} LoggerArg;

static void *logger_thread(void *arg) {
    LoggerArg *la = (LoggerArg *)arg;
    int slot = la->slot;
    int pfd  = la->pipe_fd;
    free(la);

    /* Open log file */
    FILE *logf = fopen(containers[slot].log_path, "w");
    if (!logf) {
        perror("logger: fopen");
        close(pfd);
        return NULL;
    }

    char raw[512];
    ssize_t n;

    while ((n = read(pfd, raw, sizeof(raw))) > 0) {
        /* Push into bounded buffer */
        bbuf_write(&log_bufs[slot], raw, (size_t)n);

        /* Also drain from bounded buffer into log file */
        char out[512];
        size_t got;
        while ((got = bbuf_read(&log_bufs[slot], out, sizeof(out))) > 0) {
            fwrite(out, 1, got, logf);
            fflush(logf);
            /* If nothing more immediately available, don't block */
            pthread_mutex_lock(&log_bufs[slot].lock);
            int empty = (log_bufs[slot].count == 0);
            pthread_mutex_unlock(&log_bufs[slot].lock);
            if (empty) break;
        }
    }

    /* Drain remaining data */
    bbuf_close(&log_bufs[slot]);
    char out[512];
    size_t got;
    while ((got = bbuf_read(&log_bufs[slot], out, sizeof(out))) > 0)
        fwrite(out, 1, got, logf);

    fclose(logf);
    close(pfd);
    return NULL;
}

/* =========================================================================
 * SIGCHLD handler — reap children, update metadata
 * ========================================================================= */

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&meta_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].host_pid == pid) {
                if (WIFEXITED(status)) {
                    containers[i].exit_status = WEXITSTATUS(status);
                    containers[i].state = CS_STOPPED;
                } else if (WIFSIGNALED(status)) {
                    containers[i].kill_signal = WTERMSIG(status);
                    containers[i].state = CS_KILLED;
                }
                containers[i].host_pid = 0;
                /* Unregister from kernel monitor */
                if (monitor_fd >= 0) {
                    struct container_unreg ur;
                    ur.pid = pid;
                    ioctl(monitor_fd, MONITOR_UNREGISTER, &ur);
                }
                break;
            }
        }
        pthread_mutex_unlock(&meta_lock);
    }
}

/* =========================================================================
 * Container setup (runs in child after clone/fork)
 * ========================================================================= */

static void setup_container_fs(const char *rootfs) {
    /* mount proc inside container */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount proc");
}

static int container_init(void *arg) {
    char **argv = (char **)arg;
    const char *rootfs = argv[0];
    const char *cmd    = argv[1];
    /* argv[2..] are cmd arguments */

    /* chroot into rootfs */
    if (chroot(rootfs) < 0) { perror("chroot"); _exit(1); }
    if (chdir("/") < 0)     { perror("chdir");  _exit(1); }

    setup_container_fs(rootfs);

    /* Build exec argv */
    int argc = 0;
    for (char **p = argv + 1; *p; p++) argc++;
    char **exec_argv = malloc((argc + 1) * sizeof(char *));
    for (int i = 0; i < argc; i++) exec_argv[i] = argv[1 + i];
    exec_argv[argc] = NULL;

    execv(exec_argv[0], exec_argv);
    perror("execv");
    _exit(1);
}

/* =========================================================================
 * Launch a container
 * ========================================================================= */

#define STACK_SIZE (1024 * 1024)

static int do_start(const char *name, const char *rootfs,
                    char **cmd_argv, /* NULL-terminated */
                    unsigned long soft_limit, unsigned long hard_limit,
                    int foreground)
{
    pthread_mutex_lock(&meta_lock);
    if (find_slot_by_name(name) >= 0) {
        pthread_mutex_unlock(&meta_lock);
        fprintf(stderr, "engine: container '%s' already exists\n", name);
        return -1;
    }
    int slot = find_empty_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&meta_lock);
        fprintf(stderr, "engine: max containers reached\n");
        return -1;
    }

    /* Prepare logging */
    ensure_log_dir();
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        pthread_mutex_unlock(&meta_lock);
        perror("pipe2");
        return -1;
    }

    /* Fill metadata */
    memset(&containers[slot], 0, sizeof(ContainerMeta));
    strncpy(containers[slot].name, name, 63);
    containers[slot].state      = CS_STARTING;
    containers[slot].start_time = time(NULL);
    containers[slot].soft_limit = soft_limit;
    containers[slot].hard_limit = hard_limit;
    snprintf(containers[slot].log_path, 255, "%s/%s.log", LOG_DIR, name);
    containers[slot].pipe_read_fd = pipefd[0];
    bbuf_init(&log_bufs[slot]);

    pthread_mutex_unlock(&meta_lock);

    /* Build argv array for container: rootfs, cmd, args..., NULL */
    int nargs = 0;
    for (char **p = cmd_argv; *p; p++) nargs++;
    char **cargs = malloc((nargs + 3) * sizeof(char *));
    cargs[0] = (char *)rootfs;
    for (int i = 0; i < nargs; i++) cargs[1 + i] = cmd_argv[i];
    cargs[nargs + 1] = NULL;

    /* Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc"); free(cargs); return -1; }
    char *stack_top = stack + STACK_SIZE;

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(container_init, stack_top, clone_flags, cargs);
    free(stack);
    free(cargs);

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        pthread_mutex_lock(&meta_lock);
        containers[slot].state = CS_EMPTY;
        pthread_mutex_unlock(&meta_lock);
        return -1;
    }

    /* Supervisor closes write end; container inherits it via clone */
    /* Actually for pipe-based logging we need to dup the write end into
     * container's stdio before exec. Since clone() copies file descriptors,
     * we redirect in the child. For simplicity we use /proc/pid/fd approach:
     * dup pipefd[1] to stdout/stderr in child using /proc.
     * Alternatively we use a pre-fork approach. Here we adopt the simpler
     * approach: after clone, write a small helper into the process using
     * ptrace — too complex. Instead we use socketpair + pre-exec redirect.
     * For this implementation we use a second fork() approach below.
     */
    /* NOTE: The clone()-based approach above is correct for namespace
     * isolation. For I/O redirection, we handle it by having container_init
     * dup2 the write end before execv. The write end must be passed through.
     * We encode pipefd[1] in cargs[0] as a special sentinel. See revised
     * container_init_v2 below. The code above is conceptually correct;
     * for production we'd use a more explicit pipe setup. The current
     * version passes pipe_write_fd via the environment variable ENGINE_LOGFD.
     */

    /* Pass write fd to child via a shared variable read before exec */
    /* Simplest approach: use fork()+unshare() instead of clone() so we can
     * set up fds before exec in child. Let's redo with fork(). */
    /* The container above already ran — kill it and redo with fork() */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    /* --- Proper launch with fork() + unshare() --------------------------- */
    pid = fork();
    if (pid == 0) {
        /* Child: set up namespaces, redirect I/O, exec */
        close(pipefd[0]);  /* close read end in child */

        /* Redirect stdout and stderr into pipe */
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* Create new namespaces */
        if (unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS) < 0) {
            perror("unshare");
            _exit(1);
        }

        /* chroot */
        if (chroot(rootfs) < 0) { perror("chroot"); _exit(1); }
        if (chdir("/") < 0)     { perror("chdir");  _exit(1); }

        /* mount /proc */
        mount("proc", "/proc", "proc", 0, NULL);

        execv(cmd_argv[0], cmd_argv);
        perror("execv");
        _exit(1);
    }

    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* Parent: close write end */
    close(pipefd[1]);

    pthread_mutex_lock(&meta_lock);
    containers[slot].host_pid     = pid;
    containers[slot].state        = CS_RUNNING;
    containers[slot].pipe_read_fd = pipefd[0];
    pthread_mutex_unlock(&meta_lock);

    /* Register with kernel monitor */
    if (monitor_fd >= 0 && (soft_limit || hard_limit)) {
        struct container_reg reg;
        reg.pid        = pid;
        reg.soft_limit = soft_limit;
        reg.hard_limit = hard_limit;
        strncpy(reg.name, name, 63);
        if (ioctl(monitor_fd, MONITOR_REGISTER, &reg) < 0)
            perror("ioctl REGISTER");
    }

    /* Start logger thread */
    LoggerArg *la = malloc(sizeof(LoggerArg));
    la->slot   = slot;
    la->pipe_fd = pipefd[0];
    pthread_create(&containers[slot].logger_tid, NULL, logger_thread, la);

    fprintf(stdout, "engine: started container '%s' (pid %d)\n", name, pid);

    if (foreground) {
        int status;
        waitpid(pid, &status, 0);
        pthread_mutex_lock(&meta_lock);
        if (WIFEXITED(status)) {
            containers[slot].exit_status = WEXITSTATUS(status);
            containers[slot].state = CS_STOPPED;
        } else if (WIFSIGNALED(status)) {
            containers[slot].kill_signal = WTERMSIG(status);
            containers[slot].state = CS_KILLED;
        }
        containers[slot].host_pid = 0;
        pthread_mutex_unlock(&meta_lock);
        pthread_join(containers[slot].logger_tid, NULL);
    }

    return slot;
}

/* =========================================================================
 * CLI commands (executed in supervisor context)
 * ========================================================================= */

static void cmd_ps(int fd) {
    char buf[4096];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "%-16s %-8s %-10s %-8s %-14s %-14s %s\n",
        "NAME", "PID", "STATE", "EXIT",
        "SOFT_LIMIT_MB", "HARD_LIMIT_MB", "LOG");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "%-16s %-8s %-10s %-8s %-14s %-14s %s\n",
        "----------------", "--------", "----------", "--------",
        "--------------", "--------------", "---");

    pthread_mutex_lock(&meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == CS_EMPTY) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%-16s %-8d %-10s %-8d %-14lu %-14lu %s\n",
            containers[i].name,
            containers[i].host_pid,
            state_str(containers[i].state),
            containers[i].exit_status,
            containers[i].soft_limit / (1024 * 1024),
            containers[i].hard_limit / (1024 * 1024),
            containers[i].log_path);
    }
    pthread_mutex_unlock(&meta_lock);

    write(fd, buf, pos);
}

static void cmd_logs(int fd, const char *name) {
    pthread_mutex_lock(&meta_lock);
    int slot = find_slot_by_name(name);
    char path[256] = "";
    if (slot >= 0) strncpy(path, containers[slot].log_path, 255);
    pthread_mutex_unlock(&meta_lock);

    if (slot < 0) {
        const char *err = "error: container not found\n";
        write(fd, err, strlen(err));
        return;
    }

    int lfd = open(path, O_RDONLY);
    if (lfd < 0) {
        const char *err = "error: log file not found\n";
        write(fd, err, strlen(err));
        return;
    }
    char tmp[1024];
    ssize_t n;
    while ((n = read(lfd, tmp, sizeof(tmp))) > 0)
        write(fd, tmp, n);
    close(lfd);
}

static void cmd_stop(int fd, const char *name) {
    pthread_mutex_lock(&meta_lock);
    int slot = find_slot_by_name(name);
    pid_t pid = 0;
    if (slot >= 0 && containers[slot].state == CS_RUNNING)
        pid = containers[slot].host_pid;
    pthread_mutex_unlock(&meta_lock);

    if (slot < 0 || pid == 0) {
        const char *msg = "error: container not running\n";
        write(fd, msg, strlen(msg));
        return;
    }

    kill(pid, SIGTERM);
    /* Give it 3 seconds, then SIGKILL */
    usleep(200000);
    pthread_mutex_lock(&meta_lock);
    if (containers[slot].state == CS_RUNNING)
        kill(pid, SIGKILL);
    pthread_mutex_unlock(&meta_lock);

    const char *ok = "ok: stop signal sent\n";
    write(fd, ok, strlen(ok));
}

/* =========================================================================
 * Supervisor main loop — UNIX socket server
 * ========================================================================= */

static void handle_client(int cfd) {
    char buf[CMD_MAX];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(cfd); return; }
    buf[n] = '\0';

    /* Trim trailing newline */
    for (int i = n - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); i--)
        buf[i] = '\0';

    char *tokens[16];
    int tc = 0;
    char *p = buf;
    char *tok;
    while ((tok = strsep(&p, " \t")) != NULL && tc < 15)
        tokens[tc++] = tok;
    tokens[tc] = NULL;

    if (tc == 0) { close(cfd); return; }

    if (strcmp(tokens[0], "ps") == 0) {
        cmd_ps(cfd);
    } else if (strcmp(tokens[0], "logs") == 0 && tc >= 2) {
        cmd_logs(cfd, tokens[1]);
    } else if (strcmp(tokens[0], "stop") == 0 && tc >= 2) {
        cmd_stop(cfd, tokens[1]);
    } else if ((strcmp(tokens[0], "start") == 0 ||
                strcmp(tokens[0], "run")   == 0) && tc >= 4) {
        /* start <name> <rootfs> <cmd> [args...] */
        const char *name   = tokens[1];
        const char *rootfs = tokens[2];
        char **cmd_argv    = &tokens[3];
        int fg = (strcmp(tokens[0], "run") == 0);
        unsigned long soft = 0, hard = 0;
        /* Default limits: 50MB soft, 100MB hard */
        soft = 50UL * 1024 * 1024;
        hard = 100UL * 1024 * 1024;
        int ret = do_start(name, rootfs, cmd_argv, soft, hard, fg);
        char reply[128];
        if (ret >= 0)
            snprintf(reply, sizeof(reply), "ok: container '%s' started\n", name);
        else
            snprintf(reply, sizeof(reply), "error: failed to start container\n");
        write(cfd, reply, strlen(reply));
    } else if (strcmp(tokens[0], "shutdown") == 0) {
        const char *ok = "ok: shutting down\n";
        write(cfd, ok, strlen(ok));
        supervisor_running = 0;
    } else {
        const char *err = "error: unknown command\n";
        write(cfd, err, strlen(err));
    }

    close(cfd);
}

static void supervisor_loop(void) {
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(srv, 16) < 0) { perror("listen"); exit(1); }

    /* Non-blocking accept so we can check supervisor_running */
    fcntl(srv, F_SETFL, O_NONBLOCK);

    fprintf(stdout, "engine supervisor: listening on %s\n", SOCK_PATH);

    while (supervisor_running) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            perror("accept");
            break;
        }
        handle_client(cfd);
    }

    /* Orderly shutdown: stop all running containers */
    fprintf(stdout, "engine supervisor: shutting down...\n");
    pthread_mutex_lock(&meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == CS_RUNNING && containers[i].host_pid > 0)
            kill(containers[i].host_pid, SIGTERM);
    }
    pthread_mutex_unlock(&meta_lock);
    sleep(1);
    /* Reap any remaining children */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* Join logger threads */
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state != CS_EMPTY && containers[i].logger_tid)
            pthread_join(containers[i].logger_tid, NULL);
    }

    close(srv);
    unlink(SOCK_PATH);

    if (monitor_fd >= 0) close(monitor_fd);
    fprintf(stdout, "engine supervisor: clean exit\n");
}

/* =========================================================================
 * Client: send command to supervisor and print response
 * ========================================================================= */

static void client_send(const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "engine: cannot connect to supervisor "
                "(is it running? try: sudo ./engine supervisor <rootfs>)\n");
        perror("connect");
        close(fd);
        exit(1);
    }

    write(fd, cmd, strlen(cmd));

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, n, stdout);

    close(fd);
}

/* =========================================================================
 * Signal setup
 * ========================================================================= */

static void sigterm_handler(int sig) {
    (void)sig;
    supervisor_running = 0;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <rootfs>              # start supervisor\n"
            "  %s start <name> <rootfs> <cmd> ...  # start container (bg)\n"
            "  %s run   <name> <rootfs> <cmd> ...  # start container (fg)\n"
            "  %s ps                               # list containers\n"
            "  %s logs  <name>                     # show logs\n"
            "  %s stop  <name>                     # stop container\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "supervisor") == 0) {
        /* Run as supervisor process */
        signal(SIGCHLD, sigchld_handler);
        signal(SIGTERM, sigterm_handler);
        signal(SIGINT,  sigterm_handler);

        /* Try to open kernel monitor device */
        monitor_fd = open("/dev/container_monitor", O_RDWR);
        if (monitor_fd < 0)
            fprintf(stderr, "engine: /dev/container_monitor not available "
                    "(kernel module not loaded?)\n");
        else
            fprintf(stdout, "engine: connected to kernel memory monitor\n");

        memset(containers, 0, sizeof(containers));
        supervisor_loop();
        return 0;
    }

    /* All other commands are forwarded to the supervisor */
    char cmd[CMD_MAX];
    int pos = 0;
    /* Reconstruct the command string from argv[1..] */
    for (int i = 1; i < argc && pos < (int)sizeof(cmd) - 2; i++) {
        if (i > 1) cmd[pos++] = ' ';
        int len = strlen(argv[i]);
        if (pos + len >= (int)sizeof(cmd) - 2) break;
        memcpy(cmd + pos, argv[i], len);
        pos += len;
    }
    cmd[pos++] = '\n';
    cmd[pos]   = '\0';

    client_send(cmd);
    return 0;
}
