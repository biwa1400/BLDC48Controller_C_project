#ifndef DATA_WRITER_H
#define DATA_WRITER_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

// ===== Helper: generate timestamped filename =====
void get_timestamp_filename(char *buffer, size_t size);

// ===== File operations =====
FILE* open_file(void);
void write_uint8_array(FILE *fp, const uint8_t *arr, size_t size);
void close_file(FILE *fp);

// ===== Writer process control =====
int start_writer_process(void);


#endif // DATA_WRITER_H
