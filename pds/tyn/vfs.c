#include "vfs.h"
#include <microkit.h>

// newc cpio magic
#define CPIO_MAGIC "070701"

// newc cpio header — all fields are ASCII hex, 8 chars each, big-endian
typedef struct {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
} cpio_newc_header_t;

#define VFS_MAX_FILES 256
static vfs_file_t vfs_index[VFS_MAX_FILES];
static int vfs_file_count = 0;

// Base address of the OTP rootfs cpio (mapped via tyn-beam.system otp_rootfs region)
static const uint8_t *cpio_base = (const uint8_t *)0x54000000;
// 16 MB limit matching the memory_region size
static const size_t cpio_limit = 0x1000000;

static uint32_t hex8_to_u32(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

// Align offset up to 4 bytes
static size_t align4(size_t n) {
    return (n + 3) & ~(size_t)3;
}

void vfs_init(void) {
    vfs_file_count = 0;

    // Check if cpio is present (QEMU may not have loaded it yet if beam_image is absent)
    if (cpio_base[0] != '0' || cpio_base[1] != '7' || cpio_base[2] != '0' ||
        cpio_base[3] != '7' || cpio_base[4] != '0' || cpio_base[5] != '1') {
        microkit_dbg_puts("[tyn] VFS: no valid cpio at 0x54000000 (load via QEMU -device loader)\n");
        return;
    }

    const uint8_t *hdr_ptr = cpio_base;
    const uint8_t *end = cpio_base + cpio_limit;

    while (hdr_ptr + sizeof(cpio_newc_header_t) <= end) {
        const cpio_newc_header_t *hdr = (const cpio_newc_header_t *)hdr_ptr;

        // Validate magic
        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            break;
        }

        uint32_t namesize = hex8_to_u32(hdr->c_namesize);
        uint32_t filesize = hex8_to_u32(hdr->c_filesize);
        if (hdr_ptr + sizeof(cpio_newc_header_t) + namesize > end) break;

        const char *name = (const char *)(hdr_ptr + sizeof(cpio_newc_header_t));

        // TRAILER marks end of archive
        if (namesize >= 6 &&
            name[0] == 'T' && name[1] == 'R' && name[2] == 'A' &&
            name[3] == 'I' && name[4] == 'L' && name[5] == 'E') {
            break;
        }

        // Data starts at header + align4(110 + namesize)
        const uint8_t *data = hdr_ptr + align4(sizeof(cpio_newc_header_t) + namesize);
        if (data + filesize > end) break;

        // Index regular files (mode bit 0x8000 = regular file)
        uint32_t mode = hex8_to_u32(hdr->c_mode);
        if (filesize > 0 && (mode & 0xF000) == 0x8000) {
            if (vfs_file_count < VFS_MAX_FILES) {
                vfs_index[vfs_file_count].path = name;
                vfs_index[vfs_file_count].data = data;
                vfs_index[vfs_file_count].size = (size_t)filesize;
                vfs_file_count++;
            }
        }

        // Next header starts at data + align4(filesize)
        hdr_ptr = data + align4(filesize);
    }

    // Print count
    microkit_dbg_puts("[tyn] VFS: cpio parsed — ");
    // Print vfs_file_count as decimal
    char num[16];
    int n = vfs_file_count, pos = 0;
    if (n == 0) {
        num[pos++] = '0';
    } else {
        char tmp[16]; int t = 0;
        while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) { num[pos++] = tmp[--t]; }
    }
    num[pos] = '\0';
    microkit_dbg_puts(num);
    microkit_dbg_puts(" files indexed\n");
}

const vfs_file_t *vfs_lookup(const char *path) {
    if (!path) return NULL;
    size_t plen = str_len(path);
    for (int i = 0; i < vfs_file_count; i++) {
        const char *p = vfs_index[i].path;
        // cpio paths may be relative (no leading /), normalise
        if (p[0] != '/' && path[0] == '/') {
            // compare path+1 with p
            if (str_eq(path + 1, p)) return &vfs_index[i];
        } else if (str_eq(path, p)) {
            return &vfs_index[i];
        }
        (void)plen;
    }
    return NULL;
}
