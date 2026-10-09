#include "data_socket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "dma_sdram.h"

#define SEND_INTERVAL_USEC 200
#define TCP_FRAME_SIZE (FRAME_SIZE + 4u)

static int server_fd = -1;
static volatile int keep_running = 1;

static int init_server(int port) {
    int fd;
    int opt;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("Data socket: socket");
        return -1;
    }
    opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        perror("Data socket: setsockopt");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Data socket: bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 5) < 0) {
        perror("Data socket: listen");
        close(fd);
        return -1;
    }
    printf("Data server listening on port %d\n", port);
    return fd;
}

static int prepare_data(uint8_t *buf, size_t buf_size) {
    if (buf == NULL || buf_size < TCP_FRAME_SIZE) {
        errno = EINVAL;
        return -1;
    }
    if (dma_read_live_frame(buf + 2) != 0)
        return -1;
    buf[0] = 'b';
    buf[1] = 'g';
    buf[FRAME_SIZE + 2u] = 'e';
    buf[FRAME_SIZE + 3u] = 'd';
    return 0;
}

static int send_all(int fd, const uint8_t *data, size_t length) {
    size_t offset;
    ssize_t sent;

    offset = 0;
    while (offset < length && keep_running) {
        sent = send(fd, data + offset, length - offset, 0);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent == 0)
            errno = EPIPE;
        return -1;
    }
    return offset == length ? 0 : -1;
}

static void *client_send_thread(void *arg) {
    int client_fd;
    uint8_t send_buf[TCP_FRAME_SIZE];
    unsigned int failed_reads;

    client_fd = *(int *)arg;
    free(arg);
    failed_reads = 0u;
    printf("[Data] Client send loop started\n");
    while (keep_running) {
        if (prepare_data(send_buf, sizeof(send_buf)) != 0) {
            ++failed_reads;
            if (failed_reads == 1u || failed_reads % 1000u == 0u)
                fprintf(stderr, "[Data] Live frame unavailable (errno=%d, count=%u)\n", errno, failed_reads);
            usleep(SEND_INTERVAL_USEC);
            continue;
        }
        failed_reads = 0u;
        if (send_all(client_fd, send_buf, sizeof(send_buf)) != 0) {
            if (keep_running)
                perror("[Data] Send failed or client disconnected");
            break;
        }
        usleep(SEND_INTERVAL_USEC);
    }
    close(client_fd);
    printf("[Data] Client connection closed\n");
    return NULL;
}

static void *client_accept_thread(void *arg) {
    int listen_fd;
    int *client_fd_ptr;
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    pthread_t send_thread;
    int err;

    listen_fd = *(int *)arg;
    while (keep_running) {
        client_fd_ptr = (int *)malloc(sizeof(*client_fd_ptr));
        if (client_fd_ptr == NULL) {
            usleep(100000);
            continue;
        }
        addr_len = sizeof(client_addr);
        *client_fd_ptr = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (*client_fd_ptr < 0) {
            err = errno;
            free(client_fd_ptr);
            if (!keep_running || err == EBADF || err == EINVAL)
                break;
            if (err != EINTR)
                perror("[Data] accept");
            usleep(100000);
            continue;
        }
        printf("[Data] Client connected: %s\n", inet_ntoa(client_addr.sin_addr));
        err = pthread_create(&send_thread, NULL, client_send_thread, client_fd_ptr);
        if (err != 0) {
            fprintf(stderr, "[Data] pthread_create failed: %s\n", strerror(err));
            close(*client_fd_ptr);
            free(client_fd_ptr);
        } else {
            pthread_detach(send_thread);
        }
    }
    return NULL;
}

void start_data_server(int port) {
    pthread_t accept_thread;
    int err;

    if (server_fd >= 0)
        return;
    signal(SIGPIPE, SIG_IGN);
    keep_running = 1;
    server_fd = init_server(port);
    if (server_fd < 0)
        return;
    err = pthread_create(&accept_thread, NULL, client_accept_thread, &server_fd);
    if (err != 0) {
        fprintf(stderr, "[Data] Accept thread creation failed: %s\n", strerror(err));
        close(server_fd);
        server_fd = -1;
        return;
    }
    pthread_detach(accept_thread);
    printf("Data server running (non-blocking)\n");
}

void stop_data_server(void) {
    keep_running = 0;
    if (server_fd >= 0) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        server_fd = -1;
    }
    printf("Data server stopped\n");
}
