#define _POSIX_C_SOURCE 200809L
#include "data_writer.h"
#include "dma_sdram.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FRAME_WITH_MARKERS (FRAME_SIZE + 4u)
#define LOCK_FILE "/tmp/dma_writer.lock"
#define OUTPUT_DIRECTORY "/mnt/data"
#define RECORD_POLL_NS 10000000L
#define RECORD_TIMEOUT_SECONDS 1800u
#define FILE_BUFFER_SIZE (1024u * 1024u)

static volatile sig_atomic_t writer_pid = 0;

void get_timestamp_filename(char *buffer, size_t size) {
    time_t now;
    struct tm result;
    now = time(NULL);
    if (localtime_r(&now, &result) == NULL ||
        strftime(buffer, size, OUTPUT_DIRECTORY "/output_%Y%m%d_%H%M%S.bin", &result) == 0) {
        if (size > 0) snprintf(buffer, size, OUTPUT_DIRECTORY "/output_unknown.bin");
    }
}

FILE *open_file(void) {
    char filename[128];
    FILE *fp;
    get_timestamp_filename(filename, sizeof(filename));
    fp = fopen(filename, "wb");
    if (fp == NULL) perror("DMA writer: fopen");
    else printf("DMA writer: output=%s\n", filename);
    return fp;
}

void write_uint8_array(FILE *fp, const uint8_t *arr, size_t size) {
    if (fp == NULL || arr == NULL || size == 0) return;
    if (fwrite(arr, 1, size, fp) != size) perror("DMA writer: fwrite");
}

void close_file(FILE *fp) {
    if (fp != NULL && fclose(fp) != 0) perror("DMA writer: fclose");
}

static void sigchld_handler(int sig) {
    int saved_errno;
    int status;
    pid_t pid;
    (void)sig;
    saved_errno = errno;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == (pid_t)writer_pid) writer_pid = 0;
    }
    errno = saved_errno;
}

static int wait_for_recording(uint32_t expected, uint32_t *recorded) {
    struct timespec pause_time;
    struct timeval start_time;
    struct timeval current_time;
    uint32_t status;
    uint32_t count;
    int observed_new_recording;
    long elapsed_seconds;
    long elapsed_microseconds;

    pause_time.tv_sec = 0;
    pause_time.tv_nsec = RECORD_POLL_NS;
    observed_new_recording = 0;
    if (gettimeofday(&start_time, NULL) != 0) return -1;
    for (;;) {
        status = dma_get_status();
        count = dma_get_recorded_count();
        if ((status & DMA_STATUS_RECORDING_ACTIVE) != 0 ||
            (status & DMA_STATUS_RECORDING_DONE) == 0)
            observed_new_recording = 1;
        if (observed_new_recording && (status & DMA_STATUS_RECORDING_DONE) != 0 &&
            (status & DMA_STATUS_RECORDING_ACTIVE) == 0) {
            *recorded = count;
            if (count != expected) {
                fprintf(stderr, "DMA writer: expected %u frames, got %u\n",
                        (unsigned)expected, (unsigned)count);
                errno = EIO;
                return -1;
            }
            return 0;
        }
        if (gettimeofday(&current_time, NULL) != 0) return -1;
        elapsed_seconds = (long)(current_time.tv_sec - start_time.tv_sec);
        elapsed_microseconds = (long)(current_time.tv_usec - start_time.tv_usec);
        if (elapsed_microseconds < 0) {
            --elapsed_seconds;
            elapsed_microseconds += 1000000L;
        }
        if (elapsed_seconds >= (long)RECORD_TIMEOUT_SECONDS) {
            fprintf(stderr, "DMA writer: recording timed out (count=%u, status=0x%08X)\n",
                    (unsigned)count, (unsigned)status);
            errno = ETIMEDOUT;
            return -1;
        }
        while (nanosleep(&pause_time, &pause_time) != 0) {
            if (errno != EINTR) return -1;
        }
        pause_time.tv_sec = 0;
        pause_time.tv_nsec = RECORD_POLL_NS;
    }
}

static int export_history(uint32_t frame_count) {
    volatile uint8_t *buffer;
    uint8_t *block;
    FILE *fp;
    size_t frames_per_block;
    size_t block_frames;
    size_t pos;
    size_t j;
    size_t k;
    uint32_t frame_index;
    size_t total_bytes;
    int result;

    buffer = dma_get_buffer();
    if (buffer == NULL) { errno = ENODEV; return -1; }
    block = (uint8_t *)malloc(FILE_BUFFER_SIZE);
    if (block == NULL) return -1;
    fp = open_file();
    if (fp == NULL) { free(block); return -1; }
    frames_per_block = FILE_BUFFER_SIZE / FRAME_WITH_MARKERS;
    frame_index = 0;
    result = 0;
    while (frame_index < frame_count) {
        block_frames = (size_t)(frame_count - frame_index);
        if (block_frames > frames_per_block) block_frames = frames_per_block;
        pos = 0;
        for (j = 0; j < block_frames; ++j) {
            size_t source_offset;
            source_offset = ((size_t)frame_index + j + 1u) * FRAME_SIZE;
            block[pos++] = 'b';
            block[pos++] = 'g';
            for (k = 0; k < FRAME_SIZE; ++k)
                block[pos++] = buffer[source_offset + k];
            block[pos++] = 'e';
            block[pos++] = 'd';
        }
        total_bytes = block_frames * FRAME_WITH_MARKERS;
        if (fwrite(block, 1, total_bytes, fp) != total_bytes) {
            perror("DMA writer: fwrite");
            result = -1;
            break;
        }
        frame_index += (uint32_t)block_frames;
        printf("DMA writer: exported %u / %u frames\n",
               (unsigned)frame_index, (unsigned)frame_count);
    }
    if (fclose(fp) != 0) { perror("DMA writer: fclose"); result = -1; }
    free(block);
    return result;
}

static int writer_child(void) {
    uint32_t limit;
    uint32_t recorded;
    uint32_t status;
    if (!dma_get_buffer()) { fprintf(stderr, "DMA writer: DMA not initialized\n"); return -1; }
    status = dma_get_status();
    if ((status & DMA_STATUS_RECORDING_ACTIVE) != 0) {
        fprintf(stderr, "DMA writer: recording already active\n");
        return -1;
    }
    limit = MAX_DMA_SEGMENTS;
    if (dma_set_record_limit(limit) != 0) { perror("DMA writer: set limit"); return -1; }
    if (dma_start_recording() != 0) { perror("DMA writer: start"); return -1; }
    printf("DMA writer: recording started, target=%u frames\n", (unsigned)limit);
    if (wait_for_recording(limit, &recorded) != 0) return -1;
    printf("DMA writer: recording complete, exporting %u frames\n", (unsigned)recorded);
    return export_history(recorded);
}

int start_writer_process(void) {
    static int sigchld_registered = 0;
    struct sigaction sa;
    pid_t pid;
    int fd;
    int result;
    if (!sigchld_registered) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        if (sigaction(SIGCHLD, &sa, NULL) != 0) { perror("DMA writer: sigaction"); return -1; }
        sigchld_registered = 1;
    }
    if (writer_pid != 0) {
        fprintf(stderr, "DMA writer: writer already running (PID=%ld)\n", (long)writer_pid);
        return -1;
    }
    fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("DMA writer: lock open"); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "DMA writer: another writer holds lock\n");
        close(fd);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        perror("DMA writer: fork");
        close(fd);
        return -1;
    }
    if (pid == 0) {
        result = writer_child();
        close(fd);
        _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    writer_pid = (sig_atomic_t)pid;
    close(fd);
    return (int)pid;
}
