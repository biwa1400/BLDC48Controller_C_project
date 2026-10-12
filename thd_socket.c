#include "thd_socket.h"
#include "thd_worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* THD2 wire format, 1120 bytes, little-endian IEEE-754 binary32.
 * 0       4   ASCII "THD2"
 * 4       4   uint32 sequence
 * 8      88   float fundamental_hz[22]
 * 96     88   float channel_thd_percent[22]
 * 184   880   float harmonics[22][10] (channel-major)
 * 1064    4   uint32 valid_mask (bits 0..21)
 * 1068   44   uint16 harmonic_valid_mask[22] (bits 0..9)
 * 1112    4   float total_thd_percent
 * 1116    4   float max_thd_percent
 * TCP receivers must reassemble exactly 1120 bytes per packet.
 * Frequency validity is conveyed by a positive fundamental_hz value;
 * valid_mask indicates valid THD measurements, not frequency alone.
 */
#define THD_WIRE_SIZE 1120u
#define THD_SEND_INTERVAL_US 500000u
#define THD_SLEEP_STEP_US 100000u
#define THD_SELECT_US 200000u

static pthread_t socket_thread;
static pthread_mutex_t socket_lock = PTHREAD_MUTEX_INITIALIZER;
static int socket_running = 0;
static int socket_stop_requested = 0;

static int should_stop(void)
{
    int stop;
    pthread_mutex_lock(&socket_lock);
    stop = socket_stop_requested;
    pthread_mutex_unlock(&socket_lock);
    return stop;
}

static void put_u16_le(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
}

static void put_u32_le(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
    dst[2] = (unsigned char)((value >> 16) & 0xffu);
    dst[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void put_float_le(unsigned char *dst, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32_le(dst, bits);
}

static void encode_result(unsigned char packet[THD_WIRE_SIZE],
                          const ThdResult *result)
{
    unsigned int ch;
    unsigned int harmonic;
    unsigned int offset;

    packet[0] = 'T';
    packet[1] = 'H';
    packet[2] = 'D';
    packet[3] = '2';
    put_u32_le(packet + 4u, result->sequence);

    for (ch = 0u; ch < THD_CHANNELS; ++ch)
        put_float_le(packet + 8u + ch * 4u, result->fundamental_hz[ch]);

    for (ch = 0u; ch < THD_CHANNELS; ++ch)
        put_float_le(packet + 96u + ch * 4u,
                     result->channel_thd_percent[ch]);

    offset = 184u;
    for (ch = 0u; ch < THD_CHANNELS; ++ch) {
        for (harmonic = 0u; harmonic < THD_MAX_HARMONICS; ++harmonic) {
            put_float_le(packet + offset, result->harmonics[ch][harmonic]);
            offset += 4u;
        }
    }

    put_u32_le(packet + 1064u, result->valid_mask);
    for (ch = 0u; ch < THD_CHANNELS; ++ch)
        put_u16_le(packet + 1068u + ch * 2u,
                   result->harmonic_valid_mask[ch]);
    put_float_le(packet + 1112u, result->total_thd_percent);
    put_float_le(packet + 1116u, result->max_thd_percent);
}

static int send_packet(int fd, const unsigned char *packet, unsigned int size)
{
    unsigned int offset;
    ssize_t written;
    int flags;
    offset = 0u;
    flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    while (offset < size && !should_stop()) {
        written = send(fd, packet + offset, (size_t)(size - offset), flags);
        if (written > 0) {
            offset += (unsigned int)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return offset == size ? 0 : -1;
}

static int create_listener(void)
{
    int fd;
    int opt;
    struct sockaddr_in address;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)THD_SOCKET_PORT);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void serve_client(int client_fd)
{
    ThdResult result;
    unsigned char packet[THD_WIRE_SIZE];
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    while (!should_stop()) {
        if (thd_worker_get_result(&result)) {
            encode_result(packet, &result);
            if (send_packet(client_fd, packet, THD_WIRE_SIZE) != 0)
                break;
        }
        /* Keep stop latency low while maintaining a 500 ms send interval. */
        {
            unsigned int elapsed;
            elapsed = 0u;
            while (elapsed < THD_SEND_INTERVAL_US && !should_stop()) {
                usleep(THD_SLEEP_STEP_US);
                elapsed += THD_SLEEP_STEP_US;
            }
        }
    }
}

static void *socket_main(void *unused)
{
    int listen_fd;
    int client_fd;
    int ready;
    fd_set read_set;
    struct timeval timeout;
    (void)unused;
    listen_fd = create_listener();
    if (listen_fd < 0) {
        perror("[THD] TCP listener");
        return NULL;
    }
    printf("[THD] TCP listening on port %d\n", THD_SOCKET_PORT);
    while (!should_stop()) {
        FD_ZERO(&read_set);
        FD_SET(listen_fd, &read_set);
        timeout.tv_sec = 0;
        timeout.tv_usec = THD_SELECT_US;
        ready = select(listen_fd + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("[THD] select");
            break;
        }
        if (ready == 0 || should_stop())
            continue;
        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR)
                perror("[THD] accept");
            continue;
        }
        serve_client(client_fd);
        close(client_fd);
    }
    close(listen_fd);
    return NULL;
}

int thd_socket_start(void)
{
    int err;
    pthread_mutex_lock(&socket_lock);
    if (socket_running) {
        pthread_mutex_unlock(&socket_lock);
        return -1;
    }
    socket_stop_requested = 0;
    err = pthread_create(&socket_thread, NULL, socket_main, NULL);
    if (err == 0)
        socket_running = 1;
    pthread_mutex_unlock(&socket_lock);
    return err == 0 ? 0 : -1;
}

void thd_socket_stop(void)
{
    pthread_t thread;
    pthread_mutex_lock(&socket_lock);
    if (!socket_running) {
        pthread_mutex_unlock(&socket_lock);
        return;
    }
    socket_stop_requested = 1;
    thread = socket_thread;
    pthread_mutex_unlock(&socket_lock);
    pthread_join(thread, NULL);
    pthread_mutex_lock(&socket_lock);
    socket_running = 0;
    pthread_mutex_unlock(&socket_lock);
}
