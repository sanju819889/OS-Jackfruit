#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define STACK_SIZE (1024 * 1024)
#define MAX_ID 32

static char stack[STACK_SIZE];

typedef struct container {
    char id[MAX_ID];
    pid_t pid;
    char state[16];
    int nice;
    int soft;
    int hard;
    struct container *next;
} container_t;

container_t *head = NULL;

/* ================= KERNEL REGISTER ================= */
void register_kernel(const char *id, pid_t pid, int soft, int hard)
{
    int fd = open("/dev/container_monitor", O_WRONLY);
    if (fd < 0) {
        perror("open failed");
        return;
    }

    char buf[256];

    /* IMPORTANT: convert MiB → bytes */
    snprintf(buf, sizeof(buf),
        "register:%s:%d:%d:%d\n",
        id,
        pid,
        soft * 1024 * 1024,
        hard * 1024 * 1024);

    write(fd, buf, strlen(buf));
    close(fd);
}

/* ================= SIGCHLD ================= */
void sigchld_handler(int sig)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_t *c = head;
        while (c) {
            if (c->pid == pid) {
                strcpy(c->state, "exited");
                break;
            }
            c = c->next;
        }
    }
}

/* ================= HELPERS ================= */
container_t* find(const char *id)
{
    container_t *c = head;
    while (c) {
        if (!strcmp(c->id, id)) return c;
        c = c->next;
    }
    return NULL;
}

void add(const char *id, pid_t pid, int nice, int soft, int hard)
{
    container_t *c = malloc(sizeof(container_t));
    strcpy(c->id, id);
    c->pid = pid;
    strcpy(c->state, "running");
    c->nice = nice;
    c->soft = soft;
    c->hard = hard;

    c->next = head;
    head = c;
}

/* ================= CHILD ================= */
typedef struct {
    char rootfs[256];
    char cmd[256];
    int nice;
    int fd;
} args_t;

int child_fn(void *arg)
{
    args_t *a = arg;

    if (a->nice)
        nice(a->nice);

    dup2(a->fd, STDOUT_FILENO);
    dup2(a->fd, STDERR_FILENO);
    close(a->fd);

    chroot(a->rootfs);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    /* FIX: use busybox shell */
    execl("/bin/sh", "sh", "-c", a->cmd, NULL);

    perror("exec failed");
    return 1;
}

/* ================= START ================= */
void start_cmd(int fd)
{
    char *id = strtok(NULL, " ");
    char *root = strtok(NULL, " ");
    char *cmd = strtok(NULL, "");

    int nice_val = 0, soft = 0, hard = 0;

    if (strstr(cmd, "--nice"))
        sscanf(strstr(cmd, "--nice"), "--nice %d", &nice_val);

    if (strstr(cmd, "--soft-mib"))
        sscanf(strstr(cmd, "--soft-mib"), "--soft-mib %d", &soft);

    if (strstr(cmd, "--hard-mib"))
        sscanf(strstr(cmd, "--hard-mib"), "--hard-mib %d", &hard);

    if (cmd[0] == '"') {
        cmd++;
        char *e = strrchr(cmd, '"');
        if (e) *e = 0;
    }

    if (find(id)) {
        write(fd, "ID exists\n", 10);
        return;
    }

    int pipefd[2];
    pipe(pipefd);

    args_t *a = malloc(sizeof(args_t));
    strcpy(a->rootfs, root);
    strcpy(a->cmd, cmd);
    a->nice = nice_val;
    a->fd = pipefd[1];

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, a);

    close(pipefd[1]);

    if (pid > 0) {
        add(id, pid, nice_val, soft, hard);

        /* 🔥 REGISTER TO KERNEL */
        if (soft && hard)
            register_kernel(id, pid, soft, hard);

        char file[64];
        sprintf(file, "%s.log", id);

        int log = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);

        if (!fork()) {
            char buf[512];
            int n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
                write(log, buf, n);
            exit(0);
        }

        close(pipefd[0]);
    }

    write(fd, "Started\n", 8);
}

/* ================= STOP ================= */
void stop_cmd(char *id, int fd)
{
    container_t *c = find(id);
    if (!c) {
        write(fd, "Not found\n", 10);
        return;
    }

    kill(c->pid, SIGKILL);
    strcpy(c->state, "killed");

    write(fd, "Stopped\n", 8);
}

/* ================= PS ================= */
void ps_cmd(int fd)
{
    char buf[1024] = "ID\tPID\tSTATE\tNICE\tSOFT\tHARD\n";

    container_t *c = head;
    while (c) {
        char line[256];
        sprintf(line, "%s\t%d\t%s\t%d\t%d\t%d\n",
            c->id, c->pid, c->state, c->nice, c->soft, c->hard);
        strcat(buf, line);
        c = c->next;
    }

    write(fd, buf, strlen(buf));
}

/* ================= SUPERVISOR ================= */
int supervisor()
{
    signal(SIGCHLD, sigchld_handler);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH);
    bind(s, (struct sockaddr*)&addr, sizeof(addr));
    listen(s, 5);

    printf("Supervisor running\n");

    while (1) {
        int c = accept(s, NULL, NULL);

        char buf[512] = {0};
        read(c, buf, sizeof(buf));

        char *cmd = strtok(buf, " ");

        if (!strcmp(cmd, "start") || !strcmp(cmd, "run"))
            start_cmd(c);

        else if (!strcmp(cmd, "ps"))
            ps_cmd(c);

        else if (!strcmp(cmd, "stop")) {
            char *id = strtok(NULL, " ");
            stop_cmd(id, c);
        }

        else if (!strcmp(cmd, "logs")) {
            char *id = strtok(NULL, " ");
            char file[64];
            sprintf(file, "%s.log", id);

            int fd2 = open(file, O_RDONLY);
            char b[512];
            int n;

            while ((n = read(fd2, b, sizeof(b))) > 0)
                write(c, b, n);
        }

        close(c);
    }
}

/* ================= CLIENT ================= */
int client(int argc, char *argv[])
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(s, (struct sockaddr*)&addr, sizeof(addr));

    char buf[512] = {0};
    for (int i = 1; i < argc; i++) {
        strcat(buf, argv[i]);
        strcat(buf, " ");
    }

    write(s, buf, strlen(buf));

    int n = read(s, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("%s", buf);
    }

    close(s);
    return 0;
}

/* ================= MAIN ================= */
int main(int argc, char *argv[])
{
    if (!strcmp(argv[1], "supervisor"))
        return supervisor();

    return client(argc, argv);
}
