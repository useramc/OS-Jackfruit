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

#define STACK_SIZE (1024 * 1024)
#define SOCKET_PATH "/tmp/mini_runtime.sock"
#define BUFFER_SIZE 10

// -------------------- CONTAINER LIST --------------------
typedef struct container {
    char id[50];
    pid_t pid;
    int running;

    int stop_requested;
    int exit_code;
    int exit_signal;

    struct container *next;
} container_t;

container_t *head = NULL;

// -------------------- LOG STRUCT --------------------
typedef struct {
    char id[50];
    char data[256];
} log_item_t;

// -------------------- BUFFER --------------------
log_item_t buffer[BUFFER_SIZE];
int in = 0, out = 0, count = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

// -------------------- CHILD ARG --------------------
typedef struct {
    char rootfs[100];
    int pipefd[2];
} child_args_t;

// -------------------- PRODUCER ARG --------------------
typedef struct {
    int fd;
    char id[50];
} producer_arg_t;

// -------------------- BUFFER PUSH --------------------
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

// -------------------- BUFFER POP --------------------
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

// -------------------- LOGGER --------------------
void *logger_thread(void *arg) {
    while (1) {
        log_item_t item;
        buffer_pop(&item);

        char filename[100];
        sprintf(filename, "logs/%s.log", item.id);

        FILE *fp = fopen(filename, "a");
        if (fp) {
            fprintf(fp, "%s", item.data);
            fclose(fp);
        }
    }
    return NULL;
}

// -------------------- PRODUCER --------------------
void *producer(void *arg) {
    producer_arg_t *parg = (producer_arg_t *)arg;
    char buf[256];

    while (1) {
        int n = read(parg->fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';

        log_item_t item;
        strcpy(item.id, parg->id);
        strcpy(item.data, buf);

        buffer_push(item);
    }

    close(parg->fd);
    free(parg);
    return NULL;
}

// -------------------- CHILD --------------------
int child_func(void *arg) {
    child_args_t *args = (child_args_t *)arg;

    close(args->pipefd[0]);

    dup2(args->pipefd[1], STDOUT_FILENO);
    dup2(args->pipefd[1], STDERR_FILENO);
    close(args->pipefd[1]);   // IMPORTANT

    printf("Inside container...\n");

    if (chroot(args->rootfs) != 0) {
        perror("chroot failed");
        exit(1);
    }

    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    char *cmd[] = {
        "/bin/sh",
        "-c",
        "echo Container started; while true; do echo Container running...; sleep 1; done",
        NULL
    };

    execvp(cmd[0], cmd);

    perror("exec failed");
    return 1;
}

// -------------------- SUPERVISOR --------------------
void run_supervisor() {

    printf("Supervisor started...\n");

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    unlink(SOCKET_PATH);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Listening on socket...\n");

    pthread_t logger;
    pthread_create(&logger, NULL, logger_thread, NULL);

    while (1) {

        int client_fd = accept(server_fd, NULL, NULL);

        char buffer_cmd[256] = {0};
        read(client_fd, buffer_cmd, sizeof(buffer_cmd));

        printf("Received: %s\n", buffer_cmd);

        // -------- START --------
        if (strncmp(buffer_cmd, "start", 5) == 0) {

            char id[50], rootfs[100], cmd[100];
            sscanf(buffer_cmd, "start %s %s %s", id, rootfs, cmd);

            char *stack = malloc(STACK_SIZE);
            char *stack_top = stack + STACK_SIZE;

            child_args_t *args = malloc(sizeof(child_args_t));
            strcpy(args->rootfs, rootfs);
            pipe(args->pipefd);

            pid_t pid = clone(child_func, stack_top,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                args);

            printf("Started container %s (PID %d)\n", id, pid);

            close(args->pipefd[1]);

            producer_arg_t *parg = malloc(sizeof(producer_arg_t));
            parg->fd = args->pipefd[0];
            strcpy(parg->id, id);

            pthread_t prod;
            pthread_create(&prod, NULL, producer, parg);

            container_t *newc = malloc(sizeof(container_t));
            strcpy(newc->id, id);
            newc->pid = pid;
            newc->running = 1;
            newc->stop_requested = 0;
            newc->next = head;
            head = newc;
        }

        // -------- STOP --------
        else if (strncmp(buffer_cmd, "stop", 4) == 0) {

            char id[50];
            sscanf(buffer_cmd, "stop %s", id);

            container_t *curr = head;

            while (curr) {
                if (strcmp(curr->id, id) == 0) {

                    if (!curr->running) {
                        printf("Already stopped\n");
                        break;
                    }

                    curr->stop_requested = 1;
                    kill(curr->pid, SIGKILL);

                    printf("Stopping %s...\n", id);
                    break;
                }
                curr = curr->next;
            }
        }

        // -------- PS --------
        else if (strncmp(buffer_cmd, "ps", 2) == 0) {

            container_t *curr = head;

            printf("\n--- Containers ---\n");
            while (curr) {

                char *reason = "RUNNING";

                if (!curr->running) {
                    if (curr->stop_requested)
                        reason = "STOPPED";
                    else if (curr->exit_signal == SIGKILL)
                        reason = "HARD_LIMIT_KILLED";
                    else
                        reason = "EXITED";
                }

                printf("ID: %s | PID: %d | %s\n",
                       curr->id,
                       curr->pid,
                       reason);

                curr = curr->next;
            }
            printf("------------------\n");
        }

        close(client_fd);

        // -------- WAIT + UPDATE --------
        int status;
        pid_t pid;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

            container_t *curr = head;

            while (curr) {
                if (curr->pid == pid) {

                    curr->running = 0;

                    if (WIFEXITED(status))
                        curr->exit_code = WEXITSTATUS(status);

                    if (WIFSIGNALED(status))
                        curr->exit_signal = WTERMSIG(status);

                    printf("Container %s exited\n", curr->id);
                    break;
                }
                curr = curr->next;
            }
        }
    }
}

// -------------------- CLIENT --------------------
void send_cmd(char *msg) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    write(sock, msg, strlen(msg));
    close(sock);
}

// -------------------- MAIN --------------------
int main(int argc, char *argv[]) {

    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
    }

    else if (strcmp(argv[1], "start") == 0) {
        char buf[256];
        sprintf(buf, "start %s %s %s", argv[2], argv[3], argv[4]);
        send_cmd(buf);
    }

    else if (strcmp(argv[1], "stop") == 0) {
        char buf[256];
        sprintf(buf, "stop %s", argv[2]);
        send_cmd(buf);
    }

    else if (strcmp(argv[1], "ps") == 0) {
        send_cmd("ps");
    }

    return 0;
}

