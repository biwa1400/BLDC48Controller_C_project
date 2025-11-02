#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <sys/file.h>
#include <errno.h>

#include "dma_sdram.h"

#define FRAME_WITH_MARKERS (FRAME_SIZE + 4) // "bg" + 52 bytes + "ed"
#define LOCK_FILE "/tmp/dma_writer.lock"

// ===== Helper: generate filename by current time =====
static void get_timestamp_filename(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) {
        snprintf(buffer, size, "output_unknown.bin");
        return;
    }
    strftime(buffer, size, "/mnt/data/output_%Y%m%d_%H%M%S.bin", t);
}

// Open file for append binary
FILE* open_file() {
    char filename[64];
    get_timestamp_filename(filename, sizeof(filename));

    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }

    printf("File created: %s\n", filename);
    return fp;
}

// Write uint8_t array to file
void write_uint8_array(FILE *fp, const uint8_t *arr, size_t size) {
    if (!fp || !arr || size == 0) return;

    size_t written = fwrite(arr, sizeof(uint8_t), size, fp);
    if (written != size) {
        perror("Failed to write file");
    }

    fflush(fp);
}

// Close file safely
void close_file(FILE *fp) {
    if (fp) fclose(fp);
}

// ===== Writer process management =====
static pid_t writer_pid = 0;

// SIGCHLD handler: 子进程退出时更新 writer_pid
static void sigchld_handler(int sig) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == writer_pid) {
            writer_pid = 0;
            printf("Writer process finished (PID=%d)\n", pid);
        }
    }
}

// Start DMA writer process (safe: SIGCHLD + kill check + file lock)
int start_writer_process() {
    // ===== 1. 注册 SIGCHLD，只注册一次 =====
    static bool sigchld_registered = false;
    if (!sigchld_registered) {
        struct sigaction sa;
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        if (sigaction(SIGCHLD, &sa, NULL) != 0) {
            perror("Failed to register SIGCHLD handler");
            return -1;
        }
        sigchld_registered = true;
    }

    // ===== 2. 检查 writer_pid 是否存在 =====
    if (writer_pid != 0) {
        if (kill(writer_pid, 0) == 0) {
            printf("Writer process already running (PID=%d)\n", writer_pid);
            return -1;
        } else if (errno == ESRCH) {
            writer_pid = 0; // PID 不存在
        } else {
            perror("kill check failed");
            return -1;
        }
    }

    // ===== 3. 打开文件锁 =====
    int fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("Failed to open lock file");
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        printf("Writer process already running (lock held)\n");
        close(fd);
        return -1;
    }

    // ===== 4. fork 子进程 =====
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }

    if (pid == 0) {
    	size_t i = 0;
        // ===== 子进程逻辑 =====
        FILE *fp = open_file();
        printf("Writer process started (PID=%d)\n", getpid());

        // 通知 DMA buffer 完成
        trig_buffer_reading_blocked();
        printf("buffer finished!!!!!!!\n");

        volatile uint8_t *buffer = dma_get_buffer();
        if (!buffer) {
            fprintf(stderr, "Error: DMA buffer is NULL!\n");
            close_file(fp);
            flock(fd, LOCK_UN);
            close(fd);
            exit(EXIT_FAILURE);
        }

        const uint8_t prefix[2] = {'b','g'};
        const uint8_t suffix[2] = {'e','d'};
        uint8_t frame_with_markers[FRAME_WITH_MARKERS];

        printf("Start writing DMA data: %d frames, %d bytes per frame...\n",
               MAX_DMA_SEGMENTS, FRAME_SIZE);

        for (i = 0; i < MAX_DMA_SEGMENTS; i++) {
            memcpy(frame_with_markers, prefix, 2);
            memcpy(frame_with_markers + 2, (const void *)(buffer + i*FRAME_SIZE), FRAME_SIZE);
            memcpy(frame_with_markers + 2 + FRAME_SIZE, suffix, 2);
            write_uint8_array(fp, frame_with_markers, FRAME_WITH_MARKERS);

            if (i % 100000 == 0) {
                printf("Written frame %zu / %d\n", i, MAX_DMA_SEGMENTS);
            }
        }

        printf("DMA data write complete. Total size: %.2f MB\n",
               (double)(MAX_DMA_SEGMENTS * FRAME_WITH_MARKERS) / (1024.0 * 1024.0));

        close_file(fp);

        // 释放文件锁
        flock(fd, LOCK_UN);
        close(fd);

        exit(0);
    } else {
        // ===== 父进程 =====
        writer_pid = pid;
        close(fd); // 父进程不保持锁
        return pid;
    }
}
