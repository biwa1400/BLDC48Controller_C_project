#include "control_socket.h"
#include "sram_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>

#define LUT_LOAD_COMMAND 0x10
#define LUT_FILENAME_MAX 127
static int server_fd = -1;
static volatile int keep_running = 1;
static control_callback_t command_callback = NULL;
static lut_load_callback_t lut_callback = NULL;
static pthread_mutex_t lut_request_mutex = PTHREAD_MUTEX_INITIALIZER;

void register_lut_load_callback(lut_load_callback_t callback) {
    lut_callback = callback;
}

static int send_byte(int fd, uint8_t value) {
    for (;;) {
        ssize_t n = send(fd, &value, 1, MSG_NOSIGNAL);
        if (n == 1) return 0;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int recv_exact(int fd, void *buffer, size_t size) {
    size_t received = 0;
    while (received < size && keep_running) {
        ssize_t n = recv(fd, (char *)buffer + received, size - received, 0);
        if (n > 0) { received += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return received == size ? 0 : -1;
}

static int valid_filename(const char *name, size_t len) {
    if (!len || len > LUT_FILENAME_MAX || name[0] == '.') return 0;
    size_t i;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return len >= 4 && strcmp(name + len - 4, ".bin") == 0;
}

static uint8_t load_lut_and_wait(const char *filename) {
    uint8_t result = 0xFF;
    if (pthread_mutex_trylock(&lut_request_mutex) != 0) {
        fprintf(stderr, "[Control] LUT_LOAD rejected: another request is awaiting completion.\n");
        return result;
    }
    const struct timespec interval = { 0, 20000000L };
    if (!lut_callback || lut_callback(filename) != 0x00) {
        fprintf(stderr, "[Control] LUT_LOAD rejected: %s\n", filename);
        goto done;
    }
    while (keep_running) {
        sram_worker_status_t status;
        if (sram_worker_get_status(&status) != 0) {
            fprintf(stderr, "[Control] LUT_LOAD status read failed.\n");
            break;
        }
        if (status.state == SRAM_WORKER_SUCCESS) {
            result = 0x00;
            printf("[Control] LUT_LOAD PASS: %s, active bank %u\n", filename, status.active_bank);
            break;
        }
        if (status.state == SRAM_WORKER_ERROR) {
            fprintf(stderr, "[Control] LUT_LOAD FAIL: %s, errors %u\n", filename, status.error_count);
            break;
        }
        if (status.state != SRAM_WORKER_LOADING) {
            fprintf(stderr, "[Control] LUT_LOAD unexpected worker state: %d\n", (int)status.state);
            break;
        }
        nanosleep(&interval, NULL);
    }
done:
    pthread_mutex_unlock(&lut_request_mutex);
    return result;
}

static int init_server(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    struct sockaddr_in addr = {0};
    if (fd < 0) { perror("[Control] socket"); exit(EXIT_FAILURE); }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 5) < 0) {
        perror("[Control] bind/listen"); close(fd); exit(EXIT_FAILURE);
    }
    printf("[Control] Server started, listening on port %d\n", port);
    return fd;
}

static void *client_handle_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    printf("[Control] Client connected.\n");
    while (keep_running) {
        uint8_t cmd;
        uint8_t status = 0xFF;
        if (recv_exact(fd, &cmd, 1) != 0) break;
        if (cmd == LUT_LOAD_COMMAND) {
            uint8_t length;
            char filename[LUT_FILENAME_MAX + 1];
            if (recv_exact(fd, &length, 1) != 0) break;
            if (length == 0 || length > LUT_FILENAME_MAX) {
                fprintf(stderr, "[Control] Invalid LUT filename length: %u\n", length);
                break;
            }
            if (recv_exact(fd, filename, length) != 0) break;
            filename[length] = '\0';
            if (valid_filename(filename, length)) {
                status = load_lut_and_wait(filename);
            } else {
                fprintf(stderr, "[Control] Rejected LUT filename.\n");
            }
            printf("[Control] LUT_LOAD final response: 0x%02X\n", status);
        } else {
            printf("[Control] Received command: 0x%02X\n", cmd);
            if (command_callback) status = command_callback(cmd);
        }
        if (send_byte(fd, status) != 0) break;
    }
    close(fd);
    printf("[Control] Client disconnected.\n");
    return NULL;
}

static void *client_accept_thread(void *arg) {
    int listening_fd = *(int *)arg;
    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int *fd = malloc(sizeof(*fd));
        if (!fd) { perror("[Control] malloc"); break; }
        *fd = accept(listening_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (*fd < 0) {
            free(fd);
            if (!keep_running || errno == EBADF || errno == EINVAL) break;
            if (errno == EINTR) continue;
            perror("[Control] accept");
            continue;
        }
        pthread_t thread;
        printf("[Control] New connection from %s\n", inet_ntoa(client_addr.sin_addr));
        if (pthread_create(&thread, NULL, client_handle_thread, fd) != 0) {
            perror("[Control] pthread_create"); close(*fd); free(fd);
        } else pthread_detach(thread);
    }
    return NULL;
}

void start_control_server(int port, control_callback_t callback) {
    pthread_t thread;
    signal(SIGPIPE, SIG_IGN);
    command_callback = callback;
    keep_running = 1;
    server_fd = init_server(port);
    if (pthread_create(&thread, NULL, client_accept_thread, &server_fd) != 0) {
        perror("[Control] accept thread"); close(server_fd); server_fd = -1; return;
    }
    pthread_detach(thread);
    printf("[Control] Server running (non-blocking).\n");
}

void stop_control_server(void) {
    keep_running = 0;
    if (server_fd >= 0) { shutdown(server_fd, SHUT_RDWR); close(server_fd); server_fd = -1; }
    printf("[Control] Server stopped.\n");
}
