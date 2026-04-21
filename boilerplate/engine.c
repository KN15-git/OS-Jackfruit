#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include "monitor_ioctl.h"

#define MAX_CONTAINERS 100
#define SOCKET_PATH "/tmp/container_socket"
#define BUFFER_SIZE 10
#define LOG_SIZE 256

typedef struct {
    char data[LOG_SIZE];
    char id[32];
} log_entry;

log_entry buffer[BUFFER_SIZE];
int count = 0, in = 0, out = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;

// 🔥 REGISTER PID TO KERNEL
void register_pid_with_kernel(pid_t pid) {
    int fd = open("/dev/container_monitor", O_RDWR);

    if (fd < 0) {
        perror("open device failed");
        return;
    }

    if (ioctl(fd, REGISTER_PID, &pid) < 0) {
        perror("ioctl failed");
    } else {
        printf("Registered PID %d with kernel\n", pid);
    }

    close(fd);
}

typedef struct {
    char id[32];
    pid_t pid;
    int pipe_fd;
    time_t start_time;
    char state[16];
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

// Find container by PID
int find_container_by_pid(pid_t pid) {
    for (int i = 0; i < container_count; i++) {
        if (containers[i].pid == pid)
            return i;
    }
    return -1;
}

// SIGCHLD
void sigchld_handler(int sig) {
    (void)sig;

    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        int idx = find_container_by_pid(pid);
        if (idx != -1) {
            strcpy(containers[idx].state, "stopped");
            printf("Container %s exited\n", containers[idx].id);
        }
    }
}

// Add container
void add_container(const char *id, pid_t pid, int pipe_fd) {
    strcpy(containers[container_count].id, id);
    containers[container_count].pid = pid;
    containers[container_count].pipe_fd = pipe_fd;
    containers[container_count].start_time = time(NULL);
    strcpy(containers[container_count].state, "running");
    container_count++;
}

// Producer thread
void *producer(void *arg) {
    container_t *c = (container_t *)arg;
    char buf[LOG_SIZE];
    int n;

    while ((n = read(c->pipe_fd, buf, sizeof(buf))) > 0) {

        pthread_mutex_lock(&mutex);

        while (count == BUFFER_SIZE)
            pthread_cond_wait(&not_full, &mutex);

        memcpy(buffer[in].data, buf, n);
        buffer[in].data[n] = '\0';
        strcpy(buffer[in].id, c->id);

        in = (in + 1) % BUFFER_SIZE;
        count++;

        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&mutex);
    }

    close(c->pipe_fd);
    return NULL;
}

// Consumer thread
void *consumer(void *arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&mutex);

        while (count == 0)
            pthread_cond_wait(&not_empty, &mutex);

        log_entry entry = buffer[out];
        out = (out + 1) % BUFFER_SIZE;
        count--;

        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&mutex);

        char filename[128];
        snprintf(filename, sizeof(filename), "logs/%s.log", entry.id);

        int file = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
        write(file, entry.data, strlen(entry.data));
        close(file);
    }
}

// Start container
void start_container(char *id, char *cmd) {
    int pipefd[2];
    pipe(pipefd);

    pid_t pid = fork();

    if (pid == 0) {
        close(pipefd[0]);

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execlp(cmd, cmd, NULL);
        perror("exec failed");
        exit(1);
    } 
    else if (pid > 0) {
        close(pipefd[1]);

        add_container(id, pid, pipefd[0]);

        // 🔥 FIX: REGISTER PID WITH KERNEL
        register_pid_with_kernel(pid);

        printf("Started container %s (PID %d)\n", id, pid);

        pthread_t prod_thread;
        pthread_create(&prod_thread, NULL, producer, &containers[container_count - 1]);
    }
    else {
        perror("fork failed");
    }
}

// Serialize
void serialize_containers(char *output) {
    char line[128];
    output[0] = '\0';

    for (int i = 0; i < container_count; i++) {
        snprintf(line, sizeof(line),
                 "ID: %s | PID: %d | State: %s\n",
                 containers[i].id,
                 containers[i].pid,
                 containers[i].state);
        strcat(output, line);
    }
}

// Stop
void stop_container(char *id) {
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0) {
            kill(containers[i].pid, SIGTERM);
            return;
        }
    }
}

// Supervisor
void run_supervisor() {
    int server_fd, client_fd;
    struct sockaddr_un addr;
    char buffer_cmd[256];

    unlink(SOCKET_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor listening...\n");

    pthread_t cons_thread;
    pthread_create(&cons_thread, NULL, consumer, NULL);

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);

        memset(buffer_cmd, 0, sizeof(buffer_cmd));
        read(client_fd, buffer_cmd, sizeof(buffer_cmd));

        char response[1024] = {0};

        char cmd[16], id[32], exec_cmd[64];
        sscanf(buffer_cmd, "%s %s %s", cmd, id, exec_cmd);

        if (strcmp(cmd, "start") == 0) {
            start_container(id, exec_cmd);
            snprintf(response, sizeof(response), "Started %s\n", id);
        } 
        else if (strcmp(cmd, "ps") == 0) {
            serialize_containers(response);
        }
        else if (strcmp(cmd, "stop") == 0) {
            stop_container(id);
            snprintf(response, sizeof(response), "Stopped %s\n", id);
        }

        write(client_fd, response, strlen(response));
        close(client_fd);
    }
}

// Client
void send_command(char *command) {
    int sock;
    struct sockaddr_un addr;
    char buffer[1024] = {0};

    sock = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    write(sock, command, strlen(command));
    read(sock, buffer, sizeof(buffer));

    printf("%s", buffer);
    close(sock);
}

int main(int argc, char *argv[]) {

    signal(SIGCHLD, sigchld_handler);

    if (argc >= 2 && strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
        return 0;
    }

    if (argc >= 5 && strcmp(argv[1], "start") == 0) {
        char command[256];
        snprintf(command, sizeof(command), "start %s %s", argv[2], argv[4]);
        send_command(command);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "ps") == 0) {
        send_command("ps");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "stop") == 0) {
        char command[256];
        snprintf(command, sizeof(command), "stop %s", argv[2]);
        send_command(command);
        return 0;
    }

    return 1;
}
