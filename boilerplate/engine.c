/* engine.c - Multi-Container Runtime Supervisor */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <stdarg.h>

#include "monitor_ioctl.h"

/* ── constants ────────────────────────────────────────────── */
#define MAX_CONTAINERS   32
#define LOG_BUF_SLOTS    512
#define LOG_LINE_MAX     1024
#define SOCK_PATH        "/tmp/engine.sock"
#define LOG_DIR          "/tmp/engine-logs"
#define STACK_SIZE       (1 << 20)   /* 1 MiB clone stack */
#define DEFAULT_SOFT_MIB 40
#define DEFAULT_HARD_MIB 64

/* ── container states ─────────────────────────────────────── */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_EXITED,
} ContainerState;

static const char *state_str[] = {
    "starting", "running", "stopped", "killed(hard-limit)", "exited"
};

/* ── container metadata ───────────────────────────────────── */
typedef struct {
    char           id[64];
    pid_t          pid;            /* host PID of container init */
    time_t         start_time;
    ContainerState state;
    long           soft_mib;
    long           hard_mib;
    char           log_path[256];
    int            exit_code;
    int            exit_signal;
    int            stop_requested; /* set before SIGTERM/SIGKILL */
    int            in_use;
    /* pipe fds read by log producer thread */
    int            pipe_stdout[2];
    int            pipe_stderr[2];
} Container;

static Container containers[MAX_CONTAINERS];
static pthread_mutex_t containers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── bounded log buffer ───────────────────────────────────── */
typedef struct {
    char    container_id[64];
    char    line[LOG_LINE_MAX];
} LogEntry;

static LogEntry  log_buf[LOG_BUF_SLOTS];
static int       log_head = 0, log_tail = 0, log_count = 0;
static pthread_mutex_t    log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t     log_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t     log_not_full  = PTHREAD_COND_INITIALIZER;
static volatile int       log_shutdown  = 0;

/* ── kernel monitor fd ────────────────────────────────────── */
static int monitor_fd = -1;

/* ── forward declarations ─────────────────────────────────── */
static void supervisor_loop(const char *base_rootfs);

/* ═══════════════════════════════════════════════════════════
 *  BOUNDED BUFFER HELPERS
 * ═══════════════════════════════════════════════════════════ */

static void log_push(const char *id, const char *line)
{
    pthread_mutex_lock(&log_mutex);
    while (log_count == LOG_BUF_SLOTS && !log_shutdown)
        pthread_cond_wait(&log_not_full, &log_mutex);

    if (!log_shutdown) {
        LogEntry *e = &log_buf[log_tail];
        strncpy(e->container_id, id, sizeof(e->container_id) - 1);
        strncpy(e->line, line, sizeof(e->line) - 1);
        log_tail = (log_tail + 1) % LOG_BUF_SLOTS;
        log_count++;
        pthread_cond_signal(&log_not_empty);
    }
    pthread_mutex_unlock(&log_mutex);
}

static int log_pop(LogEntry *out)
{
    pthread_mutex_lock(&log_mutex);
    while (log_count == 0 && !log_shutdown)
        pthread_cond_wait(&log_not_empty, &log_mutex);

    if (log_count == 0) {
        pthread_mutex_unlock(&log_mutex);
        return 0; /* shutdown with empty buffer */
    }
    *out = log_buf[log_head];
    log_head = (log_head + 1) % LOG_BUF_SLOTS;
    log_count--;
    pthread_cond_signal(&log_not_full);
    pthread_mutex_unlock(&log_mutex);
    return 1;
}

/* ═══════════════════════════════════════════════════════════
 *  CONSUMER THREAD — writes log entries to files
 * ═══════════════════════════════════════════════════════════ */

static void *consumer_thread(void *arg)
{
    (void)arg;
    LogEntry entry;

    while (log_pop(&entry)) {
        /* find log path */
        char log_path[256] = "";
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].in_use &&
                strcmp(containers[i].id, entry.container_id) == 0) {
                strncpy(log_path, containers[i].log_path, sizeof(log_path)-1);
                break;
            }
        }
        pthread_mutex_unlock(&containers_mutex);

        if (log_path[0]) {
            FILE *f = fopen(log_path, "a");
            if (f) {
                fprintf(f, "%s\n", entry.line);
                fclose(f);
            }
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  PRODUCER THREAD — reads pipe from one container
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    int  fd;
    char id[64];
} ProducerArg;

static void *producer_thread(void *arg)
{
    ProducerArg *pa = (ProducerArg *)arg;
    char  buf[LOG_LINE_MAX];
    char  line[LOG_LINE_MAX];
    int   llen = 0;
    ssize_t n;

    while ((n = read(pa->fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || llen == LOG_LINE_MAX - 1) {
                line[llen] = '\0';
                log_push(pa->id, line);
                llen = 0;
            } else {
                line[llen++] = buf[i];
            }
        }
    }
    /* flush partial line */
    if (llen > 0) {
        line[llen] = '\0';
        log_push(pa->id, line);
    }

    close(pa->fd);
    free(pa);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  CONTAINER CHILD ENTRY POINT
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    char  rootfs[256];
    char  cmd[256];
    char  args[8][256];
    int   argc;
    int   nice_val;
    /* pipes already set up by parent */
    int   pipe_stdout_w;
    int   pipe_stderr_w;
} ContainerArgs;

static int container_main(void *arg)
{
    ContainerArgs *ca = (ContainerArgs *)arg;

    /* redirect stdout/stderr to pipes */
    dup2(ca->pipe_stdout_w, STDOUT_FILENO);
    dup2(ca->pipe_stderr_w, STDERR_FILENO);
    close(ca->pipe_stdout_w);
    close(ca->pipe_stderr_w);

    /* mount /proc */
    if (chroot(ca->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    chdir("/");

    mkdir("/proc", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc (non-fatal)");

    /* set hostname to container id for UTS namespace */
    /* already isolated by CLONE_NEWUTS */

    /* apply nice */
    if (ca->nice_val != 0)
        nice(ca->nice_val);

    /* build argv */
    char *argv[10];
    argv[0] = ca->cmd;
    for (int i = 0; i < ca->argc; i++)
        argv[i+1] = ca->args[i];
    argv[ca->argc + 1] = NULL;

    execvp(ca->cmd, argv);
    perror("execvp");
    return 127;
}

/* ═══════════════════════════════════════════════════════════
 *  FIND / ALLOC CONTAINER SLOT
 * ═══════════════════════════════════════════════════════════ */

static Container *find_container(const char *id)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].in_use && strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

static Container *alloc_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!containers[i].in_use)
            return &containers[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  LAUNCH A CONTAINER
 * ═══════════════════════════════════════════════════════════ */

static int launch_container(const char *id, const char *rootfs,
                             const char *cmd, long soft_mib, long hard_mib,
                             int nice_val, char *errbuf, size_t errlen)
{
    pthread_mutex_lock(&containers_mutex);

    if (find_container(id)) {
        snprintf(errbuf, errlen, "container '%s' already exists", id);
        pthread_mutex_unlock(&containers_mutex);
        return -1;
    }

    Container *c = alloc_slot();
    if (!c) {
        snprintf(errbuf, errlen, "too many containers");
        pthread_mutex_unlock(&containers_mutex);
        return -1;
    }

    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, sizeof(c->id)-1);
    c->soft_mib = soft_mib;
    c->hard_mib = hard_mib;
    c->start_time = time(NULL);
    c->state = STATE_STARTING;
    c->in_use = 1;
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, id);

    /* create pipes */
    if (pipe(c->pipe_stdout) < 0 || pipe(c->pipe_stderr) < 0) {
        snprintf(errbuf, errlen, "pipe() failed: %s", strerror(errno));
        c->in_use = 0;
        pthread_mutex_unlock(&containers_mutex);
        return -1;
    }

    pthread_mutex_unlock(&containers_mutex);

    /* prepare clone args on heap (child stack must be heap-allocated) */
    ContainerArgs *ca = malloc(sizeof(*ca));
    if (!ca) return -1;
    memset(ca, 0, sizeof(*ca));
    strncpy(ca->rootfs, rootfs, sizeof(ca->rootfs)-1);
    strncpy(ca->cmd, cmd, sizeof(ca->cmd)-1);
    ca->argc = 0;
    ca->nice_val = nice_val;
    ca->pipe_stdout_w = c->pipe_stdout[1];
    ca->pipe_stderr_w = c->pipe_stderr[1];

    char *stack = malloc(STACK_SIZE);
    if (!stack) { free(ca); return -1; }

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(container_main, stack + STACK_SIZE, flags, ca);

    free(stack);  /* parent doesn't need stack after clone */

    if (pid < 0) {
        snprintf(errbuf, errlen, "clone() failed: %s", strerror(errno));
        pthread_mutex_lock(&containers_mutex);
        c->in_use = 0;
        pthread_mutex_unlock(&containers_mutex);
        free(ca);
        return -1;
    }

    /* close write ends in parent */
    close(c->pipe_stdout[1]);
    close(c->pipe_stderr[1]);

    pthread_mutex_lock(&containers_mutex);
    c->pid   = pid;
    c->state = STATE_RUNNING;
    pthread_mutex_unlock(&containers_mutex);

    /* start producer threads */
    pthread_t pt1, pt2;
    ProducerArg *pa1 = malloc(sizeof(*pa1));
    ProducerArg *pa2 = malloc(sizeof(*pa2));
    pa1->fd = c->pipe_stdout[0];
    pa2->fd = c->pipe_stderr[0];
    strncpy(pa1->id, id, sizeof(pa1->id)-1);
    strncpy(pa2->id, id, sizeof(pa2->id)-1);
    pthread_create(&pt1, NULL, producer_thread, pa1);
    pthread_create(&pt2, NULL, producer_thread, pa2);
    pthread_detach(pt1);
    pthread_detach(pt2);

    /* register with kernel monitor */
    if (monitor_fd >= 0) {
        struct container_reg reg;
        memset(&reg, 0, sizeof(reg));
        reg.pid              = pid;
        reg.soft_limit_bytes = soft_mib * 1024 * 1024;
        reg.hard_limit_bytes = hard_mib * 1024 * 1024;
        strncpy(reg.id, id, sizeof(reg.id)-1);
        if (ioctl(monitor_fd, MONITOR_REGISTER, &reg) < 0)
            fprintf(stderr, "[supervisor] ioctl REGISTER failed: %s\n",
                    strerror(errno));
    }

    free(ca);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  SIGCHLD HANDLER — reap children
 * ═══════════════════════════════════════════════════════════ */

static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].in_use && containers[i].pid == pid) {
                if (WIFEXITED(status)) {
                    containers[i].exit_code = WEXITSTATUS(status);
                    containers[i].state     = STATE_EXITED;
                } else if (WIFSIGNALED(status)) {
                    int sig2 = WTERMSIG(status);
                    containers[i].exit_signal = sig2;
                    if (sig2 == SIGKILL && !containers[i].stop_requested)
                        containers[i].state = STATE_KILLED;
                    else
                        containers[i].state = STATE_STOPPED;
                }
                /* unregister from kernel monitor */
                if (monitor_fd >= 0)
                    ioctl(monitor_fd, MONITOR_UNREGISTER, &pid);
                break;
            }
        }
        pthread_mutex_unlock(&containers_mutex);
    }
}

/* ═══════════════════════════════════════════════════════════
 *  COMMAND HANDLERS
 * ═══════════════════════════════════════════════════════════ */

/* Parse: "start id rootfs cmd [--soft-mib N] [--hard-mib N] [--nice N]" */
static void handle_start(char *args, char *resp, size_t rlen)
{
    char id[64]="", rootfs[256]="", cmd[256]="";
    long soft=DEFAULT_SOFT_MIB, hard=DEFAULT_HARD_MIB;
    int  nice_val=0;
    char errbuf[256]="";

    /* parse positional args then flags */
    char *tok = strtok(args, " \t");
    if (!tok) { snprintf(resp,rlen,"ERR missing id"); return; }
    strncpy(id, tok, sizeof(id)-1);

    tok = strtok(NULL, " \t");
    if (!tok) { snprintf(resp,rlen,"ERR missing rootfs"); return; }
    strncpy(rootfs, tok, sizeof(rootfs)-1);

    tok = strtok(NULL, " \t");
    if (!tok) { snprintf(resp,rlen,"ERR missing cmd"); return; }
    strncpy(cmd, tok, sizeof(cmd)-1);

    while ((tok = strtok(NULL, " \t")) != NULL) {
        if (strcmp(tok,"--soft-mib")==0) {
            char *v = strtok(NULL," \t");
            if (v) soft = atol(v);
        } else if (strcmp(tok,"--hard-mib")==0) {
            char *v = strtok(NULL," \t");
            if (v) hard = atol(v);
        } else if (strcmp(tok,"--nice")==0) {
            char *v = strtok(NULL," \t");
            if (v) nice_val = atoi(v);
        }
    }

    mkdir(LOG_DIR, 0755);

    if (launch_container(id, rootfs, cmd, soft, hard, nice_val,
                         errbuf, sizeof(errbuf)) == 0)
        snprintf(resp, rlen, "OK started %s", id);
    else
        snprintf(resp, rlen, "ERR %s", errbuf);
}

static void handle_ps(char *resp, size_t rlen)
{
    char line[256];
    snprintf(resp, rlen, "%-16s %-8s %-12s %-10s %-8s %-8s\n",
             "ID","PID","STATE","EXIT","SOFT","HARD");

    pthread_mutex_lock(&containers_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].in_use) continue;
        Container *c = &containers[i];
        char exit_info[16];
        if (c->state==STATE_EXITED)
            snprintf(exit_info,sizeof(exit_info),"code=%d",c->exit_code);
        else if (c->state==STATE_STOPPED||c->state==STATE_KILLED)
            snprintf(exit_info,sizeof(exit_info),"sig=%d",c->exit_signal);
        else
            strncpy(exit_info,"—",sizeof(exit_info));

        snprintf(line, sizeof(line), "%-16s %-8d %-12s %-10s %-8ldM %-8ldM\n",
                 c->id, c->pid, state_str[c->state], exit_info,
                 c->soft_mib, c->hard_mib);
        strncat(resp, line, rlen - strlen(resp) - 1);
    }
    pthread_mutex_unlock(&containers_mutex);
}

static void handle_logs(const char *id, char *resp, size_t rlen)
{
    pthread_mutex_lock(&containers_mutex);
    Container *c = find_container(id);
    char log_path[256]="";
    if (c) strncpy(log_path, c->log_path, sizeof(log_path)-1);
    pthread_mutex_unlock(&containers_mutex);

    if (!log_path[0]) {
        snprintf(resp, rlen, "ERR no container '%s'", id);
        return;
    }

    FILE *f = fopen(log_path, "r");
    if (!f) {
        snprintf(resp, rlen, "(log file empty or not yet created)");
        return;
    }
    size_t used = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && used < rlen - 2) {
        size_t ll = strlen(line);
        if (used + ll < rlen - 1) {
            memcpy(resp + used, line, ll);
            used += ll;
        }
    }
    resp[used] = '\0';
    fclose(f);
    if (used == 0) snprintf(resp, rlen, "(no output yet)");
}

static void handle_stop(const char *id, char *resp, size_t rlen)
{
    pthread_mutex_lock(&containers_mutex);
    Container *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&containers_mutex);
        snprintf(resp, rlen, "ERR no container '%s'", id);
        return;
    }
    if (c->state != STATE_RUNNING) {
        pthread_mutex_unlock(&containers_mutex);
        snprintf(resp, rlen, "ERR container '%s' not running", id);
        return;
    }
    c->stop_requested = 1;
    pid_t pid = c->pid;
    pthread_mutex_unlock(&containers_mutex);

    kill(pid, SIGTERM);
    /* give it 3 seconds, then SIGKILL */
    for (int i = 0; i < 30; i++) {
        usleep(100000);
        pthread_mutex_lock(&containers_mutex);
        int still_running = (c->state == STATE_RUNNING);
        pthread_mutex_unlock(&containers_mutex);
        if (!still_running) break;
    }
    pthread_mutex_lock(&containers_mutex);
    if (c->state == STATE_RUNNING) {
        kill(pid, SIGKILL);
    }
    pthread_mutex_unlock(&containers_mutex);

    snprintf(resp, rlen, "OK stopped %s", id);
}

/* ═══════════════════════════════════════════════════════════
 *  SUPERVISOR: UNIX SOCKET COMMAND LOOP
 * ═══════════════════════════════════════════════════════════ */

static volatile int supervisor_running = 1;

static void sigterm_handler(int sig)
{
    (void)sig;
    supervisor_running = 0;
}

static void supervisor_loop(const char *base_rootfs)
{
    (void)base_rootfs;

    /* open kernel monitor device */
    monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "[supervisor] /dev/container_monitor not available "
                "(kernel module not loaded?)\n");
    else
        fprintf(stderr, "[supervisor] kernel monitor connected\n");

    /* signal handlers */
    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* start consumer thread */
    pthread_t consumer;
    pthread_create(&consumer, NULL, consumer_thread, NULL);

    /* create UNIX domain socket */
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    unlink(SOCK_PATH);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(srv, 16) < 0) { perror("listen"); exit(1); }

    fprintf(stderr, "[supervisor] listening on %s\n", SOCK_PATH);

    /* make accept non-blocking so we can poll supervisor_running */
    fcntl(srv, F_SETFL, O_NONBLOCK);

    char resp[8192];

    while (supervisor_running) {
        int client = accept(srv, NULL, NULL);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char cmd[1024] = {0};
        ssize_t n = read(client, cmd, sizeof(cmd)-1);
        if (n <= 0) { close(client); continue; }
        cmd[n] = '\0';
        /* strip trailing newline */
        cmd[strcspn(cmd, "\n")] = '\0';

        memset(resp, 0, sizeof(resp));

        if (strncmp(cmd, "start ", 6) == 0)
            handle_start(cmd+6, resp, sizeof(resp));
        else if (strncmp(cmd, "run ", 4) == 0) {
            /* run: same as start but we tell client to wait */
            handle_start(cmd+4, resp, sizeof(resp));
            /* append pid for client to poll */
            if (strncmp(resp,"OK",2)==0) {
                char *sp = strchr(resp, '\0');
                char tok2[256];
                /* find the id that was just started */
                sscanf(cmd+4, "%255s", tok2);
                pthread_mutex_lock(&containers_mutex);
                Container *c = find_container(tok2);
                pid_t pid = c ? c->pid : -1;
                pthread_mutex_unlock(&containers_mutex);
                snprintf(sp, sizeof(resp)-(sp-resp), " pid=%d", pid);
            }
        }
        else if (strncmp(cmd, "ps", 2) == 0)
            handle_ps(resp, sizeof(resp));
        else if (strncmp(cmd, "logs ", 5) == 0)
            handle_logs(cmd+5, resp, sizeof(resp));
        else if (strncmp(cmd, "stop ", 5) == 0)
            handle_stop(cmd+5, resp, sizeof(resp));
        else
            snprintf(resp, sizeof(resp), "ERR unknown command: %s", cmd);

        write(client, resp, strlen(resp));
        close(client);
    }

    /* orderly shutdown */
    fprintf(stderr, "[supervisor] shutting down...\n");

    /* stop all running containers */
    pthread_mutex_lock(&containers_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].in_use && containers[i].state == STATE_RUNNING) {
            containers[i].stop_requested = 1;
            kill(containers[i].pid, SIGKILL);
        }
    }
    pthread_mutex_unlock(&containers_mutex);

    sleep(1);

    /* stop consumer */
    pthread_mutex_lock(&log_mutex);
    log_shutdown = 1;
    pthread_cond_broadcast(&log_not_empty);
    pthread_cond_broadcast(&log_not_full);
    pthread_mutex_unlock(&log_mutex);

    pthread_join(consumer, NULL);

    close(srv);
    unlink(SOCK_PATH);

    if (monitor_fd >= 0) close(monitor_fd);

    fprintf(stderr, "[supervisor] exited cleanly\n");
}

/* ═══════════════════════════════════════════════════════════
 *  CLI CLIENT — sends command to supervisor
 * ═══════════════════════════════════════════════════════════ */

static int run_id_global[1] = {-1};
static char run_id_str[64]  = "";

static void run_sigint_handler(int sig)
{
    (void)sig;
    if (run_id_str[0]) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = {0};
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, SOCK_PATH, sizeof(a.sun_path)-1);
        if (connect(s,(struct sockaddr*)&a,sizeof(a))==0) {
            char msg[128];
            snprintf(msg,sizeof(msg),"stop %s",run_id_str);
            write(s,msg,strlen(msg));
            char buf[256]; read(s,buf,sizeof(buf));
            close(s);
        }
    }
}

static int cli_send(const char *cmd, char *resp_buf, size_t rlen)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor. Is it running?\n");
        close(s);
        return -1;
    }
    write(s, cmd, strlen(cmd));
    ssize_t n = read(s, resp_buf, (int)rlen - 1);
    if (n > 0) resp_buf[n] = '\0';
    else resp_buf[0] = '\0';
    close(s);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  engine supervisor <base-rootfs>\n"
            "  engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine ps\n"
            "  engine logs  <id>\n"
            "  engine stop  <id>\n");
        return 1;
    }

    const char *subcmd = argv[1];

    /* ── SUPERVISOR mode ──────────────────────────────────── */
    if (strcmp(subcmd, "supervisor") == 0) {
        const char *base = (argc >= 3) ? argv[2] : ".";
        supervisor_loop(base);
        return 0;
    }

    /* ── CLI modes ────────────────────────────────────────── */
    char cmd_buf[1024] = {0};
    char resp[8192]    = {0};

    if (strcmp(subcmd, "ps") == 0) {
        if (cli_send("ps", resp, sizeof(resp)) == 0)
            printf("%s\n", resp);
        return 0;
    }

    if (strcmp(subcmd, "logs") == 0 && argc >= 3) {
        snprintf(cmd_buf, sizeof(cmd_buf), "logs %s", argv[2]);
        if (cli_send(cmd_buf, resp, sizeof(resp)) == 0)
            printf("%s\n", resp);
        return 0;
    }

    if (strcmp(subcmd, "stop") == 0 && argc >= 3) {
        snprintf(cmd_buf, sizeof(cmd_buf), "stop %s", argv[2]);
        if (cli_send(cmd_buf, resp, sizeof(resp)) == 0)
            printf("%s\n", resp);
        return 0;
    }

    if (strcmp(subcmd, "start") == 0 && argc >= 5) {
        /* build "start id rootfs cmd [flags]" */
        snprintf(cmd_buf, sizeof(cmd_buf), "start");
        for (int i = 2; i < argc; i++) {
            strncat(cmd_buf, " ", sizeof(cmd_buf)-strlen(cmd_buf)-1);
            strncat(cmd_buf, argv[i], sizeof(cmd_buf)-strlen(cmd_buf)-1);
        }
        if (cli_send(cmd_buf, resp, sizeof(resp)) == 0)
            printf("%s\n", resp);
        return 0;
    }

    if (strcmp(subcmd, "run") == 0 && argc >= 5) {
        const char *id = argv[2];
        strncpy(run_id_str, id, sizeof(run_id_str)-1);

        signal(SIGINT,  run_sigint_handler);
        signal(SIGTERM, run_sigint_handler);

        snprintf(cmd_buf, sizeof(cmd_buf), "run");
        for (int i = 2; i < argc; i++) {
            strncat(cmd_buf, " ", sizeof(cmd_buf)-strlen(cmd_buf)-1);
            strncat(cmd_buf, argv[i], sizeof(cmd_buf)-strlen(cmd_buf)-1);
        }
        if (cli_send(cmd_buf, resp, sizeof(resp)) < 0) return 1;
        printf("%s\n", resp);

        /* extract pid from response and poll */
        pid_t wait_pid = -1;
        char *pp = strstr(resp, "pid=");
        if (pp) wait_pid = atoi(pp+4);

        if (wait_pid > 0) {
            int status;
            waitpid(wait_pid, &status, 0);
            if (WIFEXITED(status))   return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        }
        return 0;
    }

    fprintf(stderr, "Unknown subcommand: %s\n", subcmd);
    return 1;
}
