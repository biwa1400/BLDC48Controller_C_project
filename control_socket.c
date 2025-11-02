#include "control_socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

// ================= 全局状态 =================
static int server_fd = -1;
static volatile int keep_running = 1;
static control_callback_t command_callback = NULL;

// ================= 函数声明 =================
static int init_server(int port);
static void *client_accept_thread(void *arg);
static void *client_handle_thread(void *arg);

// ================= 函数定义 =================

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

    printf("[Control] Server started, listening on port %d\n", port);
    return fd;
}

static void *client_handle_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    printf("[Control] Client connected, waiting for commands...\n");

    while (keep_running) {
        uint8_t cmd;
        ssize_t bytes_read = recv(client_fd, &cmd, sizeof(cmd), 0);

        if (bytes_read <= 0) {
            if (bytes_read == 0)
                printf("[Control] Client disconnected.\n");
            else
                perror("[Control] recv failed");
            break;
        }

        printf("[Control] Received command: 0x%02X\n", cmd);

        uint8_t status = 0xFF; // 默认错误状态
        if (command_callback) {
            status = command_callback(cmd);  // 调用回调函数
        } else {
            printf("[Control] Warning: no callback registered.\n");
        }

        // 回传执行结果
        ssize_t sent = send(client_fd, &status, sizeof(status), 0);
        if (sent <= 0) {
            perror("[Control] send failed");
            break;
        }

        printf("[Control] Sent response: 0x%02X\n", status);
    }

    close(client_fd);
    printf("[Control] Client thread ended.\n");
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
            perror("[Control] accept failed");
            usleep(100000);
            continue;
        }

        printf("[Control] New connection from %s\n", inet_ntoa(client_addr.sin_addr));

        pthread_t handle_thread;
        if (pthread_create(&handle_thread, NULL, client_handle_thread, client_fd_ptr) != 0) {
            perror("[Control] Failed to create handle thread");
            close(*client_fd_ptr);
            free(client_fd_ptr);
        } else {
            pthread_detach(handle_thread);
        }
    }

    return NULL;
}

// ================= 公共 API =================

// 启动控制服务器（非阻塞）
void start_control_server(int port, control_callback_t callback) {
    signal(SIGPIPE, SIG_IGN);
    command_callback = callback;
    server_fd = init_server(port);

    pthread_t accept_thread;
    if (pthread_create(&accept_thread, NULL, client_accept_thread, &server_fd) != 0) {
        perror("[Control] Failed to create accept thread");
        return;
    }
    pthread_detach(accept_thread);

    printf("[Control] Server running (non-blocking), waiting for clients...\n");

}

// 停止控制服务器
void stop_control_server(void) {
    keep_running = 0;
    if (server_fd > 0) {
        close(server_fd);
        server_fd = -1;
    }
    printf("[Control] Server stopped.\n");
}
