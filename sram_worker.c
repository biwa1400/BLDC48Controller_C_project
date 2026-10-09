#include "sram_worker.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#define LUT_DIR "/mnt/lut/"
#define LUT_COUNT 4096u
#define LUT_BYTES (LUT_COUNT * 4u)
#define LUT_VALUE_MAX 0x1FFFu
#define FPGA_BASE 0xFF210000u
#define FPGA_SPAN 0x8000u
#define REG_CONTROL 0x0000u
#define REG_STATUS 0x0001u
#define REG_LUT 0x1000u
#define STATUS_ACTIVE 0x01u
#define STATUS_ERROR 0x04u
#define STATUS_LOCK 0x08u
#define CTRL_COMMIT 0x01u
#define CTRL_LOCK 0x02u
#define COMMIT_TIMEOUT_MS 5000u
#define CTRL_CLEAR_ERROR 0x04u
#define FILENAME_MAX_LEN 127u

static pthread_t worker_thread;
static pthread_mutex_t worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_cond = PTHREAD_COND_INITIALIZER;
static sram_worker_status_t worker_status;
static char pending_filename[FILENAME_MAX_LEN + 1];
static int worker_started;
static int worker_stopping;
static int worker_pending;

static int valid_filename(const char *name) {
    size_t n = name ? strnlen(name, FILENAME_MAX_LEN + 1) : 0;
    if (n < 5 || n > FILENAME_MAX_LEN || name[0] == '.' || strcmp(name + n - 4, ".bin")) return 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static void set_progress(uint32_t progress, uint32_t errors, uint32_t active, uint32_t target) {
    pthread_mutex_lock(&worker_mutex);
    worker_status.progress = progress;
    worker_status.error_count = errors;
    worker_status.active_bank = active;
    worker_status.target_bank = target;
    pthread_mutex_unlock(&worker_mutex);
}

static uint32_t reg_read(volatile uint32_t *regs, uint32_t word_offset) {
    return regs[word_offset];
}

static void reg_write(volatile uint32_t *regs, uint32_t word_offset, uint32_t value) {
    regs[word_offset] = value;
    __sync_synchronize();
}

static int load_file(const char *filename, uint32_t *values) {
    char path[sizeof(LUT_DIR) + FILENAME_MAX_LEN + 1];
    snprintf(path, sizeof(path), "%s%s", LUT_DIR, filename);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { perror("[SRAM] open BIN"); return -1; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (off_t)LUT_BYTES) {
        fprintf(stderr, "[SRAM] invalid BIN size/type: %s\n", path);
        close(fd);
        return -1;
    }
    uint8_t raw[LUT_BYTES];
    size_t done = 0;
    while (done < sizeof(raw)) {
        ssize_t n = read(fd, raw + done, sizeof(raw) - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { perror("[SRAM] read BIN"); close(fd); return -1; }
        done += (size_t)n;
    }
    close(fd);
    uint32_t i;
    for (i = 0; i < LUT_COUNT; ++i) {
        uint32_t p = i * 4u;
        uint32_t v = (uint32_t)raw[p] | ((uint32_t)raw[p + 1] << 8) |
                     ((uint32_t)raw[p + 2] << 16) | ((uint32_t)raw[p + 3] << 24);
        if (v > LUT_VALUE_MAX) {
            fprintf(stderr, "[SRAM] invalid 13-bit value at %u: 0x%08X\n", i, v);
            return -1;
        }
        values[i] = v;
    }
    return 0;
}

static uint64_t current_time_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

static int perform_load(const char *filename) {
    uint32_t values[LUT_COUNT];
    uint32_t i;
    if (load_file(filename, values)) return -1;
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) { perror("[SRAM] /dev/mem"); return -1; }
    void *mapped = mmap(NULL, FPGA_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FPGA_BASE);
    close(fd);
    if (mapped == MAP_FAILED) { perror("[SRAM] mmap"); return -1; }
    volatile uint32_t *regs = (volatile uint32_t *)mapped;
    int result = -1;
    int locked = 0;
    int verified = 0;
    uint32_t errors = 0;
    uint32_t status = reg_read(regs, REG_STATUS);
    uint32_t active = status & STATUS_ACTIVE;
    uint32_t target = active ^ 1u;
    set_progress(0, 0, active, target);
    if (status & STATUS_LOCK) {
        fprintf(stderr, "[SRAM] FPGA update lock already held; refusing to alter it\n");
        goto cleanup;
    }
    reg_write(regs, REG_CONTROL, CTRL_CLEAR_ERROR);
    status = reg_read(regs, REG_STATUS);
    if (status & (STATUS_ERROR | STATUS_LOCK)) {
        fprintf(stderr, "[SRAM] FPGA status not ready: 0x%08X\n", status);
        goto cleanup;
    }
    reg_write(regs, REG_CONTROL, CTRL_LOCK);
    status = reg_read(regs, REG_STATUS);
    if (!(status & STATUS_LOCK)) {
        fprintf(stderr, "[SRAM] FPGA lock acquisition failed: 0x%08X\n", status);
        goto cleanup;
    }
    locked = 1;
    if ((status & STATUS_ACTIVE) != active || (status & STATUS_ERROR)) {
        fprintf(stderr, "[SRAM] bank changed/error during lock: 0x%08X\n", status);
        goto cleanup;
    }
    for (i = 0; i < LUT_COUNT; ++i) {
        reg_write(regs, REG_LUT + i, values[i]);
        if ((i & 255u) == 255u) set_progress(i + 1, 0, active, target);
    }
    status = reg_read(regs, REG_STATUS);
    if (status & STATUS_ERROR) {
        fprintf(stderr, "[SRAM] FPGA write error: 0x%08X\n", status);
        goto cleanup;
    }
    set_progress(0, 0, active, target);
    for (i = 0; i < LUT_COUNT; ++i) {
        uint32_t actual = reg_read(regs, REG_LUT + i);
        if (actual != values[i]) {
            if (errors < 8) fprintf(stderr, "[SRAM] mismatch [%u]: expected=%u actual=%u\n", i, values[i], actual);
            ++errors;
        }
        if ((i & 255u) == 255u) set_progress(i + 1, errors, active, target);
    }
    status = reg_read(regs, REG_STATUS);
    if (errors || (status & STATUS_ERROR) || !(status & STATUS_LOCK) || (status & STATUS_ACTIVE) != active) {
        fprintf(stderr, "[SRAM] verification failed: errors=%u status=0x%08X\n", errors, status);
        goto cleanup;
    }
    verified = 1;
cleanup:
    if (locked) {
        reg_write(regs, REG_CONTROL, 0);
        status = reg_read(regs, REG_STATUS);
        if ((status & STATUS_LOCK) || (status & STATUS_ERROR)) {
            fprintf(stderr, "[SRAM] unlock/status failure: 0x%08X\n", status);
            result = -1;
        }
    }
    if (verified && locked && !(status & (STATUS_LOCK | STATUS_ERROR)) &&
        (status & STATUS_ACTIVE) == active) {
        printf("[SRAM] verified %u words in bank %u; requesting COMMIT: %s\n", LUT_COUNT, target, filename);
        reg_write(regs, REG_CONTROL, CTRL_COMMIT);
        const uint64_t start = current_time_ms();
        if (start == 0) {
            fprintf(stderr, "[SRAM] cannot read system clock after COMMIT; bank state uncertain\n");
        } else {
            for (;;) {
                status = reg_read(regs, REG_STATUS);
                if (status & STATUS_ERROR) {
                    fprintf(stderr, "[SRAM] FPGA error after COMMIT: 0x%08X\n", status);
                    break;
                }
                if (status & STATUS_LOCK) {
                    fprintf(stderr, "[SRAM] unexpected LOCK after COMMIT: 0x%08X\n", status);
                    break;
                }
                if ((status & STATUS_ACTIVE) == target) {
                    result = 0;
                    set_progress(LUT_COUNT, 0, target, target);
                    printf("[SRAM] PASS: COMMIT switched active bank %u -> %u: %s\n", active, target, filename);
                    break;
                }
                uint64_t now = current_time_ms();
                if (now == 0 || now < start || now - start >= COMMIT_TIMEOUT_MS) {
                    fprintf(stderr, "[SRAM] COMMIT not confirmed within %u ms; no retry, pending switch may still occur\n", COMMIT_TIMEOUT_MS);
                    break;
                }
                struct timespec pause;
                pause.tv_sec = 0;
                pause.tv_nsec = 1000000L;
                nanosleep(&pause, NULL);
            }
        }
    }
    munmap(mapped, FPGA_SPAN);
    return result;
}

static void *worker_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&worker_mutex);
    for (;;) {
        while (!worker_pending && !worker_stopping) pthread_cond_wait(&worker_cond, &worker_mutex);
        if (worker_stopping) break;
        char filename[sizeof(pending_filename)];
        memcpy(filename, pending_filename, sizeof(filename));
        worker_pending = 0;
        pthread_mutex_unlock(&worker_mutex);
        int result = perform_load(filename);
        pthread_mutex_lock(&worker_mutex);
        worker_status.state = result == 0 ? SRAM_WORKER_SUCCESS : SRAM_WORKER_ERROR;
        if (result != 0 && worker_status.error_count == 0) worker_status.error_count = 1;
    }
    pthread_mutex_unlock(&worker_mutex);
    return NULL;
}

int sram_worker_init(void) {
    pthread_mutex_lock(&worker_mutex);
    if (worker_started) { pthread_mutex_unlock(&worker_mutex); return 0; }
    worker_stopping = 0;
    worker_pending = 0;
    memset(&worker_status, 0, sizeof(worker_status));
    worker_status.state = SRAM_WORKER_IDLE;
    int rc = pthread_create(&worker_thread, NULL, worker_main, NULL);
    if (rc == 0) worker_started = 1;
    pthread_mutex_unlock(&worker_mutex);
    if (rc) fprintf(stderr, "[SRAM] pthread_create: %s\n", strerror(rc));
    return rc ? -1 : 0;
}

void sram_worker_shutdown(void) {
    pthread_mutex_lock(&worker_mutex);
    if (!worker_started) { pthread_mutex_unlock(&worker_mutex); return; }
    worker_stopping = 1;
    pthread_cond_signal(&worker_cond);
    pthread_mutex_unlock(&worker_mutex);
    pthread_join(worker_thread, NULL);
    pthread_mutex_lock(&worker_mutex);
    worker_started = 0;
    worker_pending = 0;
    pthread_mutex_unlock(&worker_mutex);
}

uint8_t sram_worker_submit(const char *filename) {
    if (!valid_filename(filename)) return 0xFF;
    pthread_mutex_lock(&worker_mutex);
    if (!worker_started || worker_stopping || worker_pending || worker_status.state == SRAM_WORKER_LOADING) {
        pthread_mutex_unlock(&worker_mutex);
        return 0xFF;
    }
    memset(pending_filename, 0, sizeof(pending_filename));
    strcpy(pending_filename, filename);
    worker_status.state = SRAM_WORKER_LOADING;
    worker_status.progress = 0;
    worker_status.total = LUT_COUNT;
    worker_status.error_count = 0;
    worker_status.active_bank = 0;
    worker_status.target_bank = 0;
    worker_pending = 1;
    pthread_cond_signal(&worker_cond);
    pthread_mutex_unlock(&worker_mutex);
    return 0x00;
}

int sram_worker_get_status(sram_worker_status_t *status) {
    if (!status) return -1;
    pthread_mutex_lock(&worker_mutex);
    *status = worker_status;
    pthread_mutex_unlock(&worker_mutex);
    return 0;
}
