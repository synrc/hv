#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *path;
    const uint8_t *data;
    size_t size;
} vfs_file_t;

void vfs_init(void);
const vfs_file_t *vfs_lookup(const char *path);
int vfs_getdents(const char *dir_path, uint8_t *buf, size_t count, size_t *offset_ptr);
