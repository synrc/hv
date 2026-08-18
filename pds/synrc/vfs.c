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

#define VFS_MAX_FILES 1024
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
        microkit_dbg_puts("[synrc] VFS: no valid cpio at 0x54000000 (load via QEMU -device loader)\n");
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
    microkit_dbg_puts("[synrc] VFS: cpio parsed — ");
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

static vfs_file_t vfs_dir_dummy = { .path = "DIR", .data = 0, .size = 0 };

const vfs_file_t *vfs_lookup(const char *path) {
    if (!path) return NULL;
    // Remove leading slash for matching against cpio paths
    const char *match_path = (path[0] == '/') ? path + 1 : path;
    size_t match_len = str_len(match_path);
    if (match_path[0] == '\0') {
        return &vfs_dir_dummy;
    }

    int is_dir = 0;
    for (int i = 0; i < vfs_file_count; i++) {
        const char *p = vfs_index[i].path;
        if (str_eq(match_path, p)) {
            return &vfs_index[i];
        }
        // Check if match_path is a directory prefix
        // i.e. p starts with match_path + "/"
        int prefix_match = 1;
        for (size_t j = 0; j < match_len; j++) {
            if (p[j] != match_path[j]) { prefix_match = 0; break; }
        }
        if (prefix_match && p[match_len] == '/') {
            is_dir = 1;
        }
    }
    
    if (is_dir) return &vfs_dir_dummy;
    return NULL;
}

int vfs_getdents(const char *dir_path, uint8_t *buf, size_t count, size_t *offset_ptr) {
    if (!dir_path || dir_path[0] == '\0') return -1;
    const char *match_path = (dir_path[0] == '/') ? dir_path + 1 : dir_path;
    size_t match_len = str_len(match_path);
    if (match_len > 0 && match_path[match_len - 1] == '/') {
        match_len--; // strip trailing slash if any
    }
    
    size_t written = 0;
    size_t vfs_idx = *offset_ptr;
    
    while (vfs_idx < (size_t)vfs_file_count) {
        const char *p = vfs_index[vfs_idx].path;
        
        int prefix_match = 1;
        if (match_len > 0) {
            for (size_t j = 0; j < match_len; j++) {
                if (p[j] != match_path[j]) { prefix_match = 0; break; }
            }
        }
        
        if (prefix_match && (match_len == 0 || p[match_len] == '/')) {
            const char *seg_start = p + (match_len == 0 ? 0 : match_len + 1);
            const char *seg_end = seg_start;
            while (*seg_end && *seg_end != '/') seg_end++;
            
            size_t seg_len = seg_end - seg_start;
            
            int is_dup = 0;
            for (size_t k = 0; k < vfs_idx; k++) {
                const char *prev_p = vfs_index[k].path;
                int p_match = 1;
                if (match_len > 0) {
                    for (size_t j = 0; j < match_len; j++) {
                        if (prev_p[j] != match_path[j]) { p_match = 0; break; }
                    }
                }
                if (p_match && (match_len == 0 || prev_p[match_len] == '/')) {
                    const char *prev_seg_start = prev_p + (match_len == 0 ? 0 : match_len + 1);
                    const char *prev_seg_end = prev_seg_start;
                    while (*prev_seg_end && *prev_seg_end != '/') prev_seg_end++;
                    size_t prev_seg_len = prev_seg_end - prev_seg_start;
                    
                    if (seg_len == prev_seg_len) {
                        int seg_eq = 1;
                        for (size_t x = 0; x < seg_len; x++) {
                            if (seg_start[x] != prev_seg_start[x]) { seg_eq = 0; break; }
                        }
                        if (seg_eq) {
                            is_dup = 1;
                            break;
                        }
                    }
                }
            }
            
            if (!is_dup && seg_len > 0) {
                uint16_t reclen = (uint16_t)((19 + seg_len + 1 + 7) & ~7);
                if (written + reclen > count) break;
                
                uint8_t *ent = buf + written;
                for (int i = 0; i < 19; i++) ent[i] = 0;
                
                ent[0] = (uint8_t)((vfs_idx + 1) & 0xFF);
                ent[8] = (uint8_t)((vfs_idx + 1) & 0xFF);
                ent[16] = (uint8_t)(reclen & 0xFF);
                ent[17] = (uint8_t)(reclen >> 8);
                ent[18] = (*seg_end == '/') ? 4 : 8; // 4=DT_DIR, 8=DT_REG
                
                for (size_t x = 0; x < seg_len; x++) {
                    ent[19 + x] = seg_start[x];
                }
                ent[19 + seg_len] = '\0';
                
                // padding zeroed out
                for (size_t x = 19 + seg_len + 1; x < reclen; x++) {
                    ent[x] = 0;
                }
                
                written += reclen;
            }
        }
        vfs_idx++;
    }
    
    *offset_ptr = vfs_idx;
    return (int)written;
}
