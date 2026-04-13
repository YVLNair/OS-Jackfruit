#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <limits.h>

#include "monitor_ioctl.h"

#define MAX_CONTAINERS 50
#define SOCKET_PATH "/tmp/container_runtime.sock"
#define LOG_DIR "/tmp/container_logs"
#define CONTAINER_STACK_SIZE (1024 * 1024)

typedef enum {
    CONTAINER_STARTING,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED
} container_state_t;

typedef struct {
    char id[256];
    pid_t host_pid;
    time_t start_time;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char log_path[512];
    int exit_code;
    char exit_reason[256];
    int stop_requested;
} container_metadata_t;

typedef struct {
    container_metadata_t containers[MAX_CONTAINERS];
    int count;
    pthread_mutex_t lock;
} container_list_t;

static container_list_t global_containers;
static int supervisor_socket = -1;
static volatile int stop_supervisor = 0;

/* Signal handlers */
static void handle_sigchld(int sig) {
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&global_containers.lock);
        for (int i = 0; i < global_containers.count; i++) {
            if (global_containers.containers[i].host_pid == pid) {
                global_containers.containers[i].state = CONTAINER_STOPPED;
                if (WIFEXITED(status)) {
                    global_containers.containers[i].exit_code = WEXITSTATUS(status);
                    snprintf(global_containers.containers[i].exit_reason, 256,
                            "exited with code %d", WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    global_containers.containers[i].exit_code = 128 + sig;
                    if (global_containers.containers[i].stop_requested) {
                        snprintf(global_containers.containers[i].exit_reason, 256,
                                "stopped");
                    } else if (sig == SIGKILL) {
                        snprintf(global_containers.containers[i].exit_reason, 256,
                                "hard_limit_killed");
                    } else {
                        snprintf(global_containers.containers[i].exit_reason, 256,
                                "killed by signal %d", sig);
                    }
                }
                printf("[supervisor] Container %s exited: %s\n",
                       global_containers.containers[i].id,
                       global_containers.containers[i].exit_reason);
                break;
            }
        }
        pthread_mutex_unlock(&global_containers.lock);
    }
}

static void handle_sigterm(int sig) {
    printf("[supervisor] Received SIGTERM, shutting down...\n");
    stop_supervisor = 1;
}

static void handle_sigint(int sig) {
    printf("[supervisor] Received SIGINT, shutting down...\n");
    stop_supervisor = 1;
}

/* Container entry point (runs inside child) */
static int container_main(void *arg) {
    const char *rootfs = (const char *)arg;
    
    /* Mount proc */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }
    
    /* Change root */
    if (chroot(rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }
    
    /* Execute shell */
    execl("/bin/sh", "sh", NULL);
    perror("execl");
    return 1;
}

/* Register container with kernel module */
static void register_with_monitor(const char *container_id, pid_t pid,
                                  unsigned long soft_bytes, unsigned long hard_bytes) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[monitor] Warning: cannot open /dev/container_monitor\n");
        return;
    }
    
    struct monitor_request req = {
        .pid = pid,
        .soft_limit_bytes = soft_bytes,
        .hard_limit_bytes = hard_bytes
    };
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    
    if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        fprintf(stderr, "[monitor] Warning: ioctl MONITOR_REGISTER failed\n");
    } else {
        printf("[monitor] Registered container %s (PID %d) with soft=%lu hard=%lu\n",
               container_id, pid, soft_bytes, hard_bytes);
    }
    close(fd);
}

/* Unregister container from kernel module */
static void unregister_from_monitor(const char *container_id, pid_t pid) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        return;
    }
    
    struct monitor_request req = {
        .pid = pid
    };
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    
    if (ioctl(fd, MONITOR_UNREGISTER, &req) < 0) {
        fprintf(stderr, "[monitor] Warning: ioctl MONITOR_UNREGISTER failed\n");
    }
    close(fd);
}

/* Handle CLI commands */
static void handle_command(int client_sock, const char *cmd) {
    char response[4096] = {0};
    
    printf("[cli] Received command: %s\n", cmd);
    
    if (strncmp(cmd, "ps", 2) == 0) {
        pthread_mutex_lock(&global_containers.lock);
        int written = 0;
        written += snprintf(response + written, sizeof(response) - written,
                "CONTAINER_ID\tPID\tSTATE\t\tSOFT_MIB\tHARD_MIB\tREASON\n");
        
        for (int i = 0; i < global_containers.count; i++) {
            const char *state_str = "unknown";
            if (global_containers.containers[i].state == CONTAINER_RUNNING)
                state_str = "running";
            else if (global_containers.containers[i].state == CONTAINER_STOPPED)
                state_str = "stopped";
            else if (global_containers.containers[i].state == CONTAINER_STARTING)
                state_str = "starting";
            
            written += snprintf(response + written, sizeof(response) - written,
                    "%s\t%d\t%s\t%lu\t%lu\t%s\n",
                    global_containers.containers[i].id,
                    global_containers.containers[i].host_pid,
                    state_str,
                    global_containers.containers[i].soft_limit_bytes / (1024 * 1024),
                    global_containers.containers[i].hard_limit_bytes / (1024 * 1024),
                    global_containers.containers[i].exit_reason);
        }
        pthread_mutex_unlock(&global_containers.lock);
        
    } else if (strncmp(cmd, "start ", 6) == 0) {
        char id[256] = {0}, rootfs[512] = {0}, shell[256] = {0};
        char abs_rootfs[1024] = {0};
        unsigned long soft_mib = 40, hard_mib = 64;
        int nice_val = 0;
        
        sscanf(cmd + 6, "%255s %511s %255s", id, rootfs, shell);
        
        /* Convert to absolute path */
        if (rootfs[0] != '/') {
            char cwd[512];
            if (getcwd(cwd, sizeof(cwd)) == NULL) {
                snprintf(response, sizeof(response), "ERROR: getcwd failed\n");
                goto send_response;
            }
            snprintf(abs_rootfs, sizeof(abs_rootfs), "%s/%s", cwd, rootfs);
        } else {
            strncpy(abs_rootfs, rootfs, sizeof(abs_rootfs) - 1);
        }
        
        /* Verify the rootfs exists */
        if (access(abs_rootfs, F_OK) < 0) {
            snprintf(response, sizeof(response), "ERROR: rootfs directory not found: %s\n", abs_rootfs);
            goto send_response;
        }
        
        /* Parse optional limits */
        const char *soft_ptr = strstr(cmd, "--soft-mib");
        if (soft_ptr) sscanf(soft_ptr, "--soft-mib %lu", &soft_mib);
        
        const char *hard_ptr = strstr(cmd, "--hard-mib");
        if (hard_ptr) sscanf(hard_ptr, "--hard-mib %lu", &hard_mib);
        
        const char *nice_ptr = strstr(cmd, "--nice");
        if (nice_ptr) sscanf(nice_ptr, "--nice %d", &nice_val);
        
        unsigned long soft_bytes = soft_mib * 1024 * 1024;
        unsigned long hard_bytes = hard_mib * 1024 * 1024;
        
        printf("[supervisor] Starting container %s at %s\n", id, abs_rootfs);
        
        /* Create container */
        char *stack = malloc(CONTAINER_STACK_SIZE);
        if (!stack) {
            snprintf(response, sizeof(response), "ERROR: malloc failed\n");
            goto send_response;
        }
        
        pid_t pid = clone(container_main, stack + CONTAINER_STACK_SIZE,
                         CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                         (void *)abs_rootfs);
        
        if (pid < 0) {
            perror("clone");
            snprintf(response, sizeof(response), "ERROR: clone failed\n");
            free(stack);
            goto send_response;
        }
        
        pthread_mutex_lock(&global_containers.lock);
        if (global_containers.count < MAX_CONTAINERS) {
            container_metadata_t *c = &global_containers.containers[global_containers.count];
            strncpy(c->id, id, sizeof(c->id) - 1);
            c->host_pid = pid;
            c->start_time = time(NULL);
            c->state = CONTAINER_RUNNING;
            c->soft_limit_bytes = soft_bytes;
            c->hard_limit_bytes = hard_bytes;
            c->stop_requested = 0;
            snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, id);
            c->exit_code = -1;
            c->exit_reason[0] = '\0';
            
            global_containers.count++;
            
            printf("[supervisor] Container %s started (PID %d)\n", id, pid);
            
            /* Register with monitor */
            register_with_monitor(id, pid, soft_bytes, hard_bytes);
            
            snprintf(response, sizeof(response), 
                    "OK: container %s started (PID %d) with soft=%lu MiB hard=%lu MiB\n",
                    id, pid, soft_mib, hard_mib);
        } else {
            snprintf(response, sizeof(response), "ERROR: max containers reached\n");
        }
        pthread_mutex_unlock(&global_containers.lock);
        free(stack);
        
    } else if (strncmp(cmd, "stop ", 5) == 0) {
        char id[256] = {0};
        sscanf(cmd + 5, "%255s", id);
        
        pthread_mutex_lock(&global_containers.lock);
        int found = 0;
        for (int i = 0; i < global_containers.count; i++) {
            if (strcmp(global_containers.containers[i].id, id) == 0) {
                printf("[supervisor] Stopping container %s (PID %d)\n", 
                       id, global_containers.containers[i].host_pid);
                global_containers.containers[i].stop_requested = 1;
                kill(global_containers.containers[i].host_pid, SIGTERM);
                snprintf(response, sizeof(response), "OK: sent SIGTERM to %s\n", id);
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&global_containers.lock);
        
        if (!found) {
            snprintf(response, sizeof(response), "ERROR: container %s not found\n", id);
        }
        
    } else if (strncmp(cmd, "logs ", 5) == 0) {
        char id[256] = {0};
        sscanf(cmd + 5, "%255s", id);
        
        pthread_mutex_lock(&global_containers.lock);
        int found = 0;
        for (int i = 0; i < global_containers.count; i++) {
            if (strcmp(global_containers.containers[i].id, id) == 0) {
                FILE *f = fopen(global_containers.containers[i].log_path, "r");
                if (f) {
                    char line[1024];
                    int written = 0;
                    while (fgets(line, sizeof(line), f) && written < sizeof(response) - 1) {
                        int len = strlen(line);
                        if (written + len < sizeof(response)) {
                            strcpy(response + written, line);
                            written += len;
                        }
                    }
                    fclose(f);
                    if (written == 0) {
                        strcpy(response, "(log file empty)\n");
                    }
                } else {
                    snprintf(response, sizeof(response), "(no logs for %s)\n", id);
                }
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&global_containers.lock);
        
        if (!found) {
            snprintf(response, sizeof(response), "ERROR: container %s not found\n", id);
        }
        
    } else {
        snprintf(response, sizeof(response), "ERROR: unknown command: %s\n", cmd);
    }

send_response:
    send(client_sock, response, strlen(response), 0);
}

/* Accept and handle CLI connections */
static void *cli_listener_thread(void *arg) {
    int client_sock;
    char cmd_buf[1024];
    int len;
    
    printf("[supervisor] CLI listener thread started\n");
    
    while (!stop_supervisor) {
        client_sock = accept(supervisor_socket, NULL, NULL);
        if (client_sock < 0) {
            if (!stop_supervisor) {
                perror("accept");
            }
            break;
        }
        
        memset(cmd_buf, 0, sizeof(cmd_buf));
        len = recv(client_sock, cmd_buf, sizeof(cmd_buf) - 1, 0);
        if (len > 0) {
            cmd_buf[len] = '\0';
            handle_command(client_sock, cmd_buf);
        }
        close(client_sock);
    }
    
    printf("[supervisor] CLI listener thread exiting\n");
    return NULL;
}

/* Supervisor mode */
static void supervisor_mode(const char *rootfs_base) {
    struct sockaddr_un addr;
    pthread_t listener;
    
    printf("[supervisor] Starting supervisor with base rootfs: %s\n", rootfs_base);
    
    /* Initialize */
    mkdir(LOG_DIR, 0755);
    pthread_mutex_init(&global_containers.lock, NULL);
    global_containers.count = 0;
    
    /* Signal handlers */
    signal(SIGCHLD, handle_sigchld);
    signal(SIGTERM, handle_sigterm);
    signal(SIGINT, handle_sigint);
    
    /* Setup socket */
    supervisor_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (supervisor_socket < 0) {
        perror("socket");
        return;
    }
    
    unlink(SOCKET_PATH);
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(supervisor_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(supervisor_socket);
        return;
    }
    
    if (listen(supervisor_socket, 5) < 0) {
        perror("listen");
        close(supervisor_socket);
        return;
    }
    
    printf("[supervisor] Listening on %s\n", SOCKET_PATH);
    printf("[supervisor] Ready to accept commands\n");
    
    /* Start CLI listener thread */
    pthread_create(&listener, NULL, cli_listener_thread, NULL);
    
    /* Main loop */
    while (!stop_supervisor) {
        sleep(1);
    }
    
    printf("[supervisor] Shutting down...\n");
    
    /* Kill all remaining containers */
    pthread_mutex_lock(&global_containers.lock);
    for (int i = 0; i < global_containers.count; i++) {
        if (global_containers.containers[i].state == CONTAINER_RUNNING) {
            printf("[supervisor] Killing container %s (PID %d)\n",
                   global_containers.containers[i].id,
                   global_containers.containers[i].host_pid);
            kill(global_containers.containers[i].host_pid, SIGKILL);
            unregister_from_monitor(global_containers.containers[i].id,
                                   global_containers.containers[i].host_pid);
        }
    }
    pthread_mutex_unlock(&global_containers.lock);
    
    /* Clean shutdown */
    close(supervisor_socket);
    unlink(SOCKET_PATH);
    pthread_join(listener, NULL);
    
    printf("[supervisor] Shutdown complete\n");
}

/* CLI mode */
static void cli_mode(int argc, char *argv[]) {
    struct sockaddr_un addr;
    int sock;
    char cmd[1024] = {0};
    char response[4096] = {0};
    int len;
    
    /* Build command string */
    for (int i = 0; i < argc; i++) {
        if (strlen(cmd) + strlen(argv[i]) + 1 < sizeof(cmd)) {
            if (i > 0) strcat(cmd, " ");
            strcat(cmd, argv[i]);
        }
    }
    
    /* Connect to supervisor */
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        fprintf(stderr, "ERROR: Cannot create socket\n");
        exit(1);
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        fprintf(stderr, "ERROR: Cannot connect to supervisor at %s\n", SOCKET_PATH);
        fprintf(stderr, "ERROR: Is the supervisor running? Start it with:\n");
        fprintf(stderr, "       sudo ./engine supervisor ./rootfs-base\n");
        close(sock);
        exit(1);
    }
    
    /* Send command */
    if (send(sock, cmd, strlen(cmd), 0) < 0) {
        perror("send");
        close(sock);
        exit(1);
    }
    
    /* Receive response */
    memset(response, 0, sizeof(response));
    len = recv(sock, response, sizeof(response) - 1, 0);
    if (len > 0) {
        response[len] = '\0';
        printf("%s", response);
    }
    
    close(sock);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: engine <command> [args...]\n");
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  engine supervisor <base-rootfs>\n");
        fprintf(stderr, "  engine start <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n");
        fprintf(stderr, "  engine run <id> <rootfs> <command> [--soft-mib N] [--hard-mib N]\n");
        fprintf(stderr, "  engine ps\n");
        fprintf(stderr, "  engine logs <id>\n");
        fprintf(stderr, "  engine stop <id>\n");
        return 1;
    }
    
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: engine supervisor <base-rootfs>\n");
            return 1;
        }
        supervisor_mode(argv[2]);
    } else {
        cli_mode(argc - 1, argv + 1);
    }
    
    return 0;
}
