#include "data_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include "dma_sdram.h"

#define SEND_INTERVAL_USEC 1000  // 0.001 秒

// ================= Global state =================
static int server_fd = -1;
static volatile int keep_running = 1;

// ================= Function declarations =================
static int init_server(int port);
static void *client_accept_thread(void *arg);
static void *client_send_thread(void *arg);
static void prepare_data(uint8_t *buf, size_t buf_size);

// ================= Function definitions =================

static int init_server(int port) {
    int fd;
    struct sockaddr_in addr;

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(fd);
        exit(EXIT_FAILURE);
    }

    if (listen(fd, 5) < 0) {
        perror("listen failed");
        close(fd);
        exit(EXIT_FAILURE);
    }

    printf("Server started, listening on port %d\n", port);
    return fd;
}

static void prepare_data(uint8_t *buf, size_t buf_size) {
    if (buf_size < FRAME_SIZE + 4) return;

    uint8_t data[FRAME_SIZE];
    dma_read_values(data);  // 从 DMA SDRAM 读取 52 字节数据
    //print_signed16(data, 0, 28);

    memcpy(buf, "bg", 2);
    memcpy(buf + 2, data, FRAME_SIZE);
    memcpy(buf + 2 + FRAME_SIZE, "ed", 2);
}

static void *client_send_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    uint8_t send_buf[FRAME_SIZE + 4];
    printf("[Thread] Client send loop started.\n");

    while (keep_running) {
        prepare_data(send_buf, sizeof(send_buf));
        ssize_t sent = send(client_fd, send_buf, sizeof(send_buf), 0);
        if (sent <= 0) {
            perror("[Thread] Send failed or client disconnected");
            close(client_fd);
            printf("[Thread] Client connection closed.\n");
            return NULL;
        }
        usleep(SEND_INTERVAL_USEC);
    }

    close(client_fd);
    return NULL;
}

static void *client_accept_thread(void *arg) {
    int server_fd = *(int *)arg;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (keep_running) {
        int *client_fd_ptr = malloc(sizeof(int));
        if (!client_fd_ptr) continue;

        *client_fd_ptr = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (*client_fd_ptr < 0) {
            free(client_fd_ptr);
            perror("[Accept] Failed");
            usleep(100000);
            continue;
        }

        printf("Client connected: %s\n", inet_ntoa(client_addr.sin_addr));

        pthread_t send_thread;
        if (pthread_create(&send_thread, NULL, client_send_thread, client_fd_ptr) != 0) {
            perror("[Accept] Failed to create send thread");
            close(*client_fd_ptr);
            free(client_fd_ptr);
        } else {
            pthread_detach(send_thread);
        }
    }

    return NULL;
}

// ================= Public API =================

// Start server (non-blocking, returns immediately)
void start_data_server(int port) {
    srand(time(NULL));
    signal(SIGPIPE, SIG_IGN);

    server_fd = init_server(port);

    pthread_t accept_thread;
    if (pthread_create(&accept_thread, NULL, client_accept_thread, &server_fd) != 0) {
        perror("Failed to create accept thread");
        close(server_fd);
        return;
    }
    pthread_detach(accept_thread);

    printf("Server running (non-blocking), waiting for clients...\n");
}

// Stop server manually
void stop_data_server(void) {
    keep_running = 0;
    if (server_fd > 0) {
        close(server_fd);
        server_fd = -1;
    }
    printf("Server stopped.\n");
}
