#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define SOCKET_PATH "/tmp/mini_runtime.sock"
#define BUFFER_SIZE 10

// -------------------- EXIT POLICY --------------------
typedef enum {
    RUNNING,
    STOPPED,
    HARD_LIMIT_KILLED,
    EXITED
} exit_reason_t;

// -------------------- CONTAINER --------------------
typedef struct container {
    char id[50];
    pid_t pid;
    int running;
    int stop_requested;

    exit_reason_t reason;
    int exit_code;

    struct container *next;
} container_t;

container_t *head = NULL;

// -------------------- LOG BUFFER (UNCHANGED) --------------------
typedef struct {
    char id[50];
    char data[256];
} log_item_t;

log_item_t buffer[BUFFER_SIZE];
int in = 0, out = 0, count = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

// -------------------- BUFFER OPS --------------------
void buffer_push(log_item_t item) {
    pthread_mutex_lock(&mutex);

    while (count == BUFFER_SIZE)
        pthread_cond_wait(&not_full, &mutex);

    buffer[in] = item;
    in = (in + 1) % BUFFER_SIZE;
    count++;

    pthread_cond_signal(&not_empty);
    pthread_mutex_unlock(&mutex);
}

void buffer_pop(log_item_t *item) {
    pthread_mutex_lock(&mutex);

    while (count == 0)
        pthread_cond_wait(&not_empty, &mutex);

    *item = buffer[out];
    out = (out + 1) % BUFFER_SIZE;
    count--;

    pthread_cond_signal(&not_full);
    pthread_mutex_unlock(&mutex);
}

// -------------------- LOGGER THREAD --------------------
void *logger_thread(void *arg) {
    (void)arg;
    mkdir("logs", 0777);

    while (1) {
        log_item_t item;
        buffer_pop(&item);

        char path[128];
        snprintf(path, sizeof(path), "logs/%s.log", item.id);

        FILE *fp = fopen(path, "a");
        if (fp) {
            fprintf(fp, "%s", item.data);
            fclose(fp);
        }
    }
    return NULL;
}

// -------------------- PRODUCER THREAD --------------------
typedef struct {
    int fd;
    char id[50];
} producer_arg_t;

void *producer(void *arg) {
    producer_arg_t *p = (producer_arg_t *)arg;
    char buf[256];

    while (1) {
        int n = read(p->fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';

        log_item_t item;
        strcpy(item.id, p->id);
        strcpy(item.data, buf);

        buffer_push(item);
    }

    close(p->fd);
    free(p);
    return NULL;
}

// -------------------- KERNEL REGISTRATION --------------------
void register_to_kernel(char *id, pid_t pid) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;

    struct monitor_request req;
    memset(&req, 0, sizeof(req));

    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    req.pid = pid;

    req.soft_limit_bytes = 50 * 1024 * 1024;
    req.hard_limit_bytes = 80 * 1024 * 1024;

    ioctl(fd, MONITOR_REGISTER, &req);
    close(fd);
}

// -------------------- CHILD --------------------
int child_func(void *arg)
{
    char **a = (char **)arg;

    printf("[child] starting...\n");

    if (chroot(a[0]) != 0) {
        perror("chroot failed");
        exit(1);
    }

    if (chdir("/") != 0) {
        perror("chdir failed");
        exit(1);
    }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("proc mount failed");
    }

    printf("[child] inside container rootfs OK\n");

    char *cmd[] = {
        "/bin/sh",
        "-c",
        "while true; do echo running; sleep 1; done",
        NULL
    };

    execvp(cmd[0], cmd);

    perror("exec failed");
    return 1;
}


// -------------------- SUPERVISOR --------------------
void run_supervisor() {
    printf("Supervisor started...\n");

    int server = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    unlink(SOCKET_PATH);
    bind(server, (struct sockaddr*)&addr, sizeof(addr));
    listen(server, 5);

    pthread_t logt;
    pthread_create(&logt, NULL, logger_thread, NULL);

    while (1) {
        int cfd = accept(server, NULL, NULL);
        char buf[256];
memset(buf, 0, sizeof(buf));

int n = read(cfd, buf, sizeof(buf) - 1);

if (n <= 0) {
    close(cfd);
    continue;
}

buf[n] = '\0';

printf("Received: %s\n", buf);


        // ---------------- START ----------------
        if (strncmp(buf, "start", 5) == 0) {

            char id[50], rootfs[100], cmd[100];
            sscanf(buf, "start %s %s %s", id, rootfs, cmd);

            char *stack = malloc(STACK_SIZE);
            char **args = malloc(sizeof(char*) * 2);
            args[0] = strdup(rootfs);

            pid_t pid = clone(child_func,
                              stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              args);

            container_t *c = malloc(sizeof(container_t));
            strcpy(c->id, id);
            c->pid = pid;
            c->running = 1;
            c->stop_requested = 0;
            c->reason = RUNNING;
            c->next = head;
            head = c;

            register_to_kernel(id, pid);
            printf("Started %s (%d)\n", id, pid);
        }

        // ---------------- STOP ----------------
        else if (strncmp(buf, "stop", 4) == 0) {

            char id[50];
            sscanf(buf, "stop %s", id);

            for (container_t *c = head; c; c = c->next) {
                if (!strcmp(c->id, id)) {
                    c->stop_requested = 1;
                    c->reason = STOPPED;
                    kill(c->pid, SIGKILL);
                    break;
                }
            }
        }

        // ---------------- PS + CLEANUP ----------------
        else if (strncmp(buf, "ps", 2) == 0) {

            int status;
            pid_t pid;

            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

                for (container_t *c = head; c; c = c->next) {
                    if (c->pid == pid) {

                        c->running = 0;

                        if (WIFSIGNALED(status) &&
                            WTERMSIG(status) == SIGKILL &&
                            !c->stop_requested) {
                            c->reason = HARD_LIMIT_KILLED;
                        } else if (!c->stop_requested) {
                            c->reason = EXITED;
                        }
                    }
                }
            }

            printf("\n--- CONTAINERS ---\n");
            for (container_t *c = head; c; c = c->next) {

                char *r =
                    c->reason == RUNNING ? "RUNNING" :
                    c->reason == STOPPED ? "STOPPED" :
                    c->reason == HARD_LIMIT_KILLED ? "HARD_KILL" :
                    "EXITED";

                printf("%s | %d | %s\n", c->id, c->pid, r);
            }
            printf("------------------\n");
        }

        close(cfd);
    }
}

// -------------------- CLIENT --------------------
void send_cmd(char *msg) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    connect(s, (struct sockaddr*)&addr, sizeof(addr));
    write(s, msg, strlen(msg));
    close(s);
}

// -------------------- MAIN --------------------
int main(int argc, char *argv[]) {

    if (argc < 2) return 0;

    if (!strcmp(argv[1], "supervisor"))
        run_supervisor();

    else if (!strcmp(argv[1], "start")) {
        char b[256];
        sprintf(b, "start %s %s %s", argv[2], argv[3], argv[4]);
        send_cmd(b);
    }

    else if (!strcmp(argv[1], "stop")) {
        char b[256];
        sprintf(b, "stop %s", argv[2]);
        send_cmd(b);
    }

    else if (!strcmp(argv[1], "ps")) {
        send_cmd("ps");
    }

    return 0;
}

