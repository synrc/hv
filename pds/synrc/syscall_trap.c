#include "syscall_trap.h"
#include <microkit.h>
#include <stdint.h>
#include "console.h"
#include "vfs.h"

// AArch64 Syscall Wrapper:
// BEAM's 'svc #0' instructions will be patched at runtime to 'bl tyn_syscall_entry'.
// The 'bl' instruction overwrites lr (x30). We save it, arrange arguments for C,
// and restore it to seamlessly return back to BEAM's execution flow.
__asm__(
".global tyn_syscall_entry\n"
"tyn_syscall_entry:\n"
"    stp x30, x8, [sp, #-16]!\n"
"    stp x1, x2, [sp, #-16]!\n"
"    stp x3, x4, [sp, #-16]!\n"
"    stp x5, x6, [sp, #-16]!\n"
"    stp x7, x9, [sp, #-16]!\n"
"    stp x10, x11, [sp, #-16]!\n"
"    stp x12, x13, [sp, #-16]!\n"
"    stp x14, x15, [sp, #-16]!\n"
"    stp x16, x17, [sp, #-16]!\n"
"    stp x18, x19, [sp, #-16]!\n"
"    mov x6, x5\n"
"    mov x5, x4\n"
"    mov x4, x3\n"
"    mov x3, x2\n"
"    mov x2, x1\n"
"    mov x1, x0\n"
"    mov x0, x8\n" // syscall number is in x8 in musl AArch64
"    bl tyn_syscall_dispatch\n"
"    ldp x18, x19, [sp], #16\n"
"    ldp x16, x17, [sp], #16\n"
"    ldp x14, x15, [sp], #16\n"
"    ldp x12, x13, [sp], #16\n"
"    ldp x10, x11, [sp], #16\n"
"    ldp x7, x9, [sp], #16\n"
"    ldp x5, x6, [sp], #16\n"
"    ldp x3, x4, [sp], #16\n"
"    ldp x1, x2, [sp], #16\n"
"    ldp x30, x8, [sp], #16\n"
"    ret\n"
);

static console_ring_t *console_ring = (console_ring_t *)0x10000000;
static inline void sel4_yield(void) {
    asm volatile("yield" ::: "memory");
}

static inline void sys_lock(void) {
    while (__atomic_test_and_set(&sys_state->lock, __ATOMIC_ACQUIRE)) {
        // yield to other threads while waiting
        sel4_yield();
    }
}

static inline void sys_unlock(void) {
    __atomic_clear(&sys_state->lock, __ATOMIC_RELEASE);
}

static inline void __attribute__((unused)) synrc_syscall_lock(void) {
    sys_lock();
}

static inline void __attribute__((unused)) synrc_syscall_unlock(void) {
    sys_unlock();
}

static void get_fake_time(int64_t *sec, long *nsec) {
    sys_lock();
    sys_state->fake_timer_nsec += 100000; // increment 100us per call
    *sec = (int64_t)(sys_state->fake_timer_nsec / 1000000000ULL);
    *nsec = (long)(sys_state->fake_timer_nsec % 1000000000ULL);
    sys_unlock();
}

typedef struct {
    int64_t tv_sec;
    long    tv_nsec;
} tyn_timespec_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} tyn_timeval_t;

// ---------------------------------------------------------------------------
// File descriptor table
// ---------------------------------------------------------------------------


static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void tyn_memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

static void tyn_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

static void tyn_strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && src[i] != '\0'; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static inline void patch_svc_in_region(uintptr_t start, size_t len) {
    if (!start || len < 4) return;
    uint32_t *trampoline_base = (uint32_t *)0x6F0000;
    uintptr_t end_addr = start + len - 3;
    for (uintptr_t addr = start; addr < end_addr; addr += 4) {
        uint32_t *inst = (uint32_t *)addr;
        if (*inst == 0xd4000001) { // svc #0
            uint32_t idx = sys_state->trampoline_index++;
            uint32_t *t = &trampoline_base[idx * 6];
            
            // 1. stp x30, x18, [sp, #-16]!
            t[0] = 0xa9bf4bfe;
            // 2. mov x16, #0x08000000 (encoded as movz x16, #0x0800, lsl #16)
            t[1] = 0xd2a10010;
            // 3. ldr x16, [x16]
            t[2] = 0xf9400210;
            // 4. blr x16
            t[3] = 0xd63f0200;
            // 5. ldp x30, x18, [sp], #16
            t[4] = 0xa8c14bfe;
            // 6. b (addr + 4)
            int64_t offset = (int64_t)(addr + 4) - (int64_t)&t[5];
            uint32_t imm26 = (offset >> 2) & 0x03FFFFFF;
            t[5] = 0x14000000 | imm26;
            
            // Replace svc #0 with b to trampoline
            offset = (int64_t)t - (int64_t)addr;
            imm26 = (offset >> 2) & 0x03FFFFFF;
            *inst = 0x14000000 | imm26;
        }
    }
}

static inline void clear_cache(void *start, void *end) {
    if ((uintptr_t)start >= 0x400000 && (uintptr_t)start < 0xB0000000UL) {
        patch_svc_in_region((uintptr_t)start, (uintptr_t)end - (uintptr_t)start);
    }
    uintptr_t align = 64;
    uintptr_t addr = (uintptr_t)start & ~(align - 1);
    uintptr_t end_addr = (uintptr_t)end;
    for (; addr < end_addr; addr += align) {
        __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    for (addr = (uintptr_t)start & ~(align - 1); addr < end_addr; addr += align) {
        __asm__ volatile("ic ivau, %0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");
}

static int fd_alloc(const vfs_file_t *f, const char *dir_path) {
    for (int i = 3; i < FD_MAX; i++) {
        if (!sys_state->fd_table[i].used) {
            sys_state->fd_table[i].used = 1;
            sys_state->fd_table[i].file = f;
            sys_state->fd_table[i].offset = 0;
            sys_state->fd_table[i].nonblock = 0;
            if (dir_path) {
                tyn_strncpy((char *)sys_state->fd_table[i].dir_path, dir_path, 128);
            } else {
                sys_state->fd_table[i].dir_path[0] = '\0';
            }
            return i;
        }
    }
    return -1;
}

void tyn_syscall_init(void) {
    microkit_dbg_puts("[synrc] Synrc host trap dispatcher online (~50 musl syscall handlers ready)\n");
    sys_state->lock = 0;
    sys_state->heap_curr = 0x30000000;
    sys_state->mmap_curr = 0x90000000;
    sys_state->mmap_jit_curr = 0xB0000000UL;
    sys_state->fake_timer_nsec = 1000000000ULL;
    sys_state->dev_null_file.path = "/dev/null";
    sys_state->dev_null_file.data = 0;
    sys_state->dev_null_file.size = 0;
    sys_state->dev_dir_file.path = "DIR";
    sys_state->dev_dir_file.data = 0;
    sys_state->dev_dir_file.size = 0;
    sys_state->dev_pipe_file.path = "PIPE";
    sys_state->dev_pipe_file.data = 0;
    sys_state->dev_pipe_file.size = 0;
    for (int i = 0; i < FD_MAX; i++) {
        sys_state->fd_table[i].file   = 0;
        sys_state->fd_table[i].offset = 0;
        sys_state->fd_table[i].used   = 0;
        sys_state->fd_table[i].nonblock = 0;
    }
}

// ---------------------------------------------------------------------------
// Syscall dispatcher
// ---------------------------------------------------------------------------


static int next_thread_slot = 0;
static struct mailbox_slot *const mailbox = (struct mailbox_slot *)0x21000000;

static void puthex64(uint64_t val) {
    char buf[17];
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        int nibble = val & 0xf;
        buf[i] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
        val >>= 4;
    }
    microkit_dbg_puts("0x");
    microkit_dbg_puts(buf);
}

static long do_syscall(long sysno, long a1, long a2, long a3, long a4, long a5, long a6) {

/*
    if (microkit_name[0] == 's' || microkit_name[0] == 'b') { // synrc or beam_*
        if (sysno != 98 && sysno != 63 && sysno != 124 && sysno != 72 && sysno != 113) {
            microkit_dbg_puts("[synrc] syscall ");
            puthex64(sysno);
            microkit_dbg_puts(" a1=");
            puthex64(a1);
            microkit_dbg_puts("\n");
        }
    }
*/

    (void)a6;
    if (!sys_state->fd_table[0].used) {
        // Initialize stdio on first use if needed
    }

    switch (sysno) {

        // ----------------------------------------------------------------
        // I/O
        // ----------------------------------------------------------------

        case SYS_getcwd: {
            char *buf = (char *)a1;
            size_t size = (size_t)a2;
            if (!buf || size < 2) return -34; // ERANGE
            buf[0] = '/';
            buf[1] = '\0';
            return 1; // returns length including NUL
        }

        case SYS_chdir:
        case SYS_fchdir:
            return 0;

        case SYS_statfs:
        case SYS_fstatfs:
            if (a2) tyn_memset((void *)a2, 0, 128);
            return 0;

        case SYS_pipe2: {
            int *pipefd = (int *)a1;
            int flags = (int)a2; // O_NONBLOCK is 04000
            int fd0 = fd_alloc((const vfs_file_t *)&sys_state->dev_pipe_file, 0);
            int fd1 = fd_alloc((const vfs_file_t *)&sys_state->dev_pipe_file, 0);
            if (fd0 < 0 || fd1 < 0) return -1;
            sys_state->fd_table[fd0].nonblock = (flags & 04000) ? 1 : 0;
            sys_state->fd_table[fd1].nonblock = (flags & 04000) ? 1 : 0;
            pipefd[0] = fd0;
            pipefd[1] = fd1;
            return 0;
        }

        case SYS_fcntl: {
            int fd = (int)a1;
            int cmd = (int)a2;
            long arg = a3;
            if (fd == 0 || fd == 1 || fd == 2) {
                return 0; // Fake success for stdin/stdout/stderr
            }
            if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                if (cmd == 3) { // F_GETFL
                    return sys_state->fd_table[fd].nonblock ? 04000 : 0;
                }
                if (cmd == 4) { // F_SETFL
                    sys_state->fd_table[fd].nonblock = (arg & 04000) ? 1 : 0;
                    return 0;
                }
                return 0; // Fake success for other commands
            }
            return -9; // EBADF
        }

        case SYS_ioctl: {
            int fd = (int)a1;
            long req = a2;
            if (fd == 0 || fd == 1 || fd == 2) {
                if (req == 0x5401) { // TCGETS
                    struct termios_ptr {
                        uint32_t c_iflag;
                        uint32_t c_oflag;
                        uint32_t c_cflag;
                        uint32_t c_lflag;
                        uint8_t c_line;
                        uint8_t c_cc[32];
                        uint32_t c_ispeed;
                        uint32_t c_ospeed;
                    } *t = (void*)a3;
                    if (t) {
                        for (size_t i = 0; i < sizeof(*t); i++) ((char *)t)[i] = 0;
                        t->c_iflag = 0x500;
                        t->c_oflag = 0x5;
                        t->c_cflag = 0xbf;
                        t->c_lflag = 0x8a3b;
                    }
                    return 0;
                }
                return 0; // fake success for other terminal ioctls
            }

            if (a2 == 0x5413) { // TIOCGWINSZ
                uint16_t *ws = (uint16_t *)a3;
                if (ws) {
                    ws[0] = 24; // rows
                    ws[1] = 80; // cols
                    ws[2] = 0;
                    ws[3] = 0;
                }
                return 0;
            }
            return -25; // ENOTTY
        }
        case SYS_futex: {
            int *uaddr = (int *)a1;
            int futex_op = (int)a2;
            int val = (int)a3;

            int cmd = futex_op & 127;
            if (cmd == 0) { // FUTEX_WAIT
                if (*uaddr == val) {
                    sel4_yield();
                }
                return 0;
            } else if (cmd == 1) { // FUTEX_WAKE
                // Thread wakeups handled via seL4 scheduler if we were natively mapping them
                // But for now, we just pretend it woke up
                return 1; // Fake 1 thread woke up
            }
            return 0;
        }

        case SYS_read: {
            int fd = (int)a1;
            char *buf = (char *)a2;
            size_t count = (size_t)a3;
            if (fd == 0) {
                size_t c = console_ring_read_rx(console_ring, buf, count);
                if (c == 0) return -11; // EAGAIN
                return (long)c;
            }
            if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                fd_entry_t *e = (fd_entry_t *)&sys_state->fd_table[fd];
                if (e->file == &sys_state->dev_pipe_file) {
                    if (e->nonblock) {
                        return -11; // EAGAIN
                    } else {
                        sys_unlock();
                        sel4_yield();
                        sys_lock();
                        return -4; // EINTR to retry read
                    }
                }
                if (e->file == &sys_state->dev_null_file) {
                    return -11; // EAGAIN instead of EOF so Erlang doesn't crash thinking the port closed
                }
                size_t avail = e->file->size - e->offset;
                if (count > avail) count = avail;
                const uint8_t *src = e->file->data + e->offset;
                for (size_t i = 0; i < count; i++) buf[i] = (char)src[i];
                e->offset += count;
                return (long)count;
            }
            microkit_dbg_puts("[synrc] SYS_read: unknown fd\n");
            return -9; // EBADF
        }


        case SYS_write: {
            int fd = (int)a1;
            if (fd == 0 || fd == 1 || fd == 2) {
                const char *buf = (const char *)a2;
                size_t len = (size_t)a3;
                for (size_t j = 0; j < len; j++) {
                    microkit_dbg_putc(buf[j]);
                }
                return a3;
            }
            if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                if (sys_state->fd_table[fd].file == &sys_state->dev_pipe_file) {
                    asm volatile("sev");
                    return a3;
                }
                if (sys_state->fd_table[fd].file == &sys_state->dev_null_file) return a3;
            }
            return -1;
        }

        case SYS_writev: {
            int fd = (int)a1;
            if (fd == 0 || fd == 1 || fd == 2) {
                struct iovec {
                    void *iov_base;
                    size_t iov_len;
                };
                const struct iovec *iov = (const struct iovec *)a2;
                int iovcnt = (int)a3;
                long total = 0;
                for (int i = 0; i < iovcnt; i++) {
                    const char *buf = (const char *)iov[i].iov_base;
                    size_t len = iov[i].iov_len;
                    for (size_t j = 0; j < len; j++) {
                        microkit_dbg_putc(buf[j]);
                    }
                    total += len;
                }
                return total;
            }
            if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                if (sys_state->fd_table[fd].file == &sys_state->dev_pipe_file) {
                    asm volatile("sev");
                    struct iovec { void *iov_base; size_t iov_len; };
                    const struct iovec *iov = (const struct iovec *)a2;
                    long total = 0;
                    for (int i = 0; i < (int)a3; i++) total += iov[i].iov_len;
                    return total;
                }
                if (sys_state->fd_table[fd].file == &sys_state->dev_null_file) {
                    struct iovec { void *iov_base; size_t iov_len; };
                    const struct iovec *iov = (const struct iovec *)a2;
                    long total = 0;
                    for (int i = 0; i < (int)a3; i++) total += iov[i].iov_len;
                    return total;
                }
            }
            return -9; // EBADF
        }
        // ----------------------------------------------------------------
        // File system
        // ----------------------------------------------------------------
        case SYS_openat: {
            const char *path = (const char *)a2;
            const vfs_file_t *f = 0;
            if (streq(path, "/dev/null")) {
                f = (const vfs_file_t *)&sys_state->dev_null_file;
            } else {
                f = vfs_lookup(path);
            }
            if (!f) return -2; // ENOENT
            const char *dp = (f == &sys_state->dev_dir_file) ? path : 0;
            int fd = fd_alloc(f, dp);
            return fd < 0 ? -24 : (long)fd; // EMFILE
        }

        case SYS_close:
            if ((int)a1 >= 3 && (int)a1 < FD_MAX) {
                sys_state->fd_table[(int)a1].used = 0;
            }
            return 0;

        case SYS_lseek: {
            int fd = (int)a1;
            long offset = a2;
            int whence = (int)a3;
            if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                fd_entry_t *e = (fd_entry_t *)&sys_state->fd_table[fd];
                size_t new_off;
                if (whence == 0 /* SEEK_SET */) new_off = (size_t)offset;
                else if (whence == 1 /* SEEK_CUR */) new_off = e->offset + (size_t)offset;
                else if (whence == 2 /* SEEK_END */) new_off = e->file->size + (size_t)offset;
                else return -1;
                if (new_off > e->file->size) return -1;
                e->offset = new_off;
                return (long)new_off;
            }
            return -1;
        }

        case SYS_fstat: {
            int fd = (int)a1;
            if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                uint8_t *st = (uint8_t *)a2;
                if (!st) return -14; // EFAULT
                for (int i = 0; i < 128; i++) st[i] = 0;

                if (sys_state->fd_table[fd].file == &sys_state->dev_dir_file) {
                    // st_mode at offset 16: set S_IFDIR | 0755 = 0x41ED
                    st[16] = 0xED; st[17] = 0x41;
                    st[20] = 2; // nlink
                } else {
                    // st_mode at offset 16: set S_IFREG | 0644 = 0x81A4
                    st[16] = 0xA4; st[17] = 0x81;
                    st[20] = 1;
                    // st_size at offset 48
                    size_t sz = sys_state->fd_table[fd].file->size;
                    uint8_t *p = st + 48;
                    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(sz & 0xFF); sz >>= 8; }
                }
                return 0;
            }
            microkit_dbg_puts("[synrc] SYS_fstat: failure\n");
            return -9; // EBADF
        }

        case SYS_lstat:   // 1039 — unique on aarch64
            return 0;

        case SYS_fstatat: { // newfstatat (dirfd, path, stat, flags)
            const char *path = (const char *)a2;
            uint8_t *st = (uint8_t *)a3;
            if (!st) return -14; // EFAULT
            // zero the entire stat buffer (128 bytes)
            for (int i = 0; i < 128; i++) st[i] = 0;

            const vfs_file_t *f = vfs_lookup(path);
            if (f) {
                if (f == &sys_state->dev_dir_file) {
                    st[16] = 0xED; st[17] = 0x41; // S_IFDIR|0755
                    st[20] = 2;
                } else {
                    st[16] = 0xA4; st[17] = 0x81; // S_IFREG|0644
                    st[20] = 1;
                    // st_size at offset 48
                    size_t sz = f->size;
                    uint8_t *p = st + 48;
                    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(sz & 0xFF); sz >>= 8; }
                }
            } else {
                return -2; // ENOENT
            }
            return 0;
        }

        case SYS_faccessat:  // 48 — covers access() alias too
            return 0;

        case SYS_getdents64: {
            int fd = (int)a1;
            uint8_t *buf = (uint8_t *)a2;
            size_t count = (size_t)a3;

            if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                if (sys_state->fd_table[fd].dir_path[0] != '\0') {
                    int res = vfs_getdents((const char *)sys_state->fd_table[fd].dir_path, buf, count, (size_t *)&sys_state->fd_table[fd].offset);
                    if (res >= 0) return res;
                }
                return 0; // Not a directory or empty
            }
            return -9; // EBADF
        }

        case SYS_readlink: {
            const char *buf = (char *)a2;
            size_t bufsiz = (size_t)a3;
            const char *exe = "/otp/erts-9.3/bin/beam";
            size_t len = 0;
            while (exe[len]) len++;
            if (len >= bufsiz) len = bufsiz - 1;
            char *out = (char *)buf;
            for (size_t i = 0; i < len; i++) out[i] = exe[i];
            out[len] = '\0';
            return (long)len;
        }

        // ----------------------------------------------------------------
        // Memory
        // ----------------------------------------------------------------
        case SYS_brk:
            if (a1 == 0) return (long)sys_state->heap_curr;
            if ((uintptr_t)a1 > 0x31000000) return -12; // ENOMEM
            sys_state->heap_curr = (uintptr_t)a1;
            return (long)sys_state->heap_curr;

        case SYS_mmap: {
            size_t size    = (size_t)a2;
            int    flags   = (int)a4;
            int    prot    = (int)a3;
            int    MAP_FIXED_FLAG = 0x10;
            int    PROT_EXEC_FLAG = 0x4;

            uintptr_t aligned_size;

            // MAP_FIXED: ERTS commits pages within a previously reserved range.
            if (flags & MAP_FIXED_FLAG) {
                return (long)(uintptr_t)a1;
            }

            // HACK: Intercept the 1GB super carrier allocation (size == 0x40000000)
            if (size == 0x40000000) {
                return (long)0x80000000;
            }

            // PROT_EXEC allocations (JIT code pages): allocate from high end downward.
            if (prot & PROT_EXEC_FLAG) {
                aligned_size = (size + 0x1FFFFF) & ~(uintptr_t)0x1FFFFF; // 2MB align
                if (sys_state->mmap_jit_curr < aligned_size || sys_state->mmap_jit_curr - aligned_size < 0x80000000UL) {
                    microkit_dbg_puts("[synrc] SYS_mmap JIT ENOMEM! size=");
                    puthex64(size);
                    microkit_dbg_puts(" jit_curr=");
                    puthex64(sys_state->mmap_jit_curr);
                    microkit_dbg_puts("\n");
                    return -12;
                }
                sys_state->mmap_jit_curr -= aligned_size;
                tyn_memset((void *)sys_state->mmap_jit_curr, 0, aligned_size);
                clear_cache((void *)sys_state->mmap_jit_curr, (void *)(sys_state->mmap_jit_curr + aligned_size));
                microkit_dbg_puts("[synrc] SYS_mmap JIT ret=");
                puthex64(sys_state->mmap_jit_curr);
                microkit_dbg_puts(" sz=");
                puthex64(aligned_size);
                microkit_dbg_puts("\n");
                return (long)sys_state->mmap_jit_curr;
            }

            // Normal (non-exec) allocations: 4KB-align, bump from low end.
            aligned_size = (size + 0xFFF) & ~(uintptr_t)0xFFF;
            if (size == 0x54000 || size == 0x104000) {
                aligned_size = 0x200000; // Expand thread stacks to 2MB to prevent stack overflow
            }
            if (sys_state->mmap_curr + aligned_size > 0xAF000000UL) {
                microkit_dbg_puts("[synrc] SYS_mmap ENOMEM! size=");
                puthex64(size);
                microkit_dbg_puts(" sys_state->mmap_curr=");
                puthex64(sys_state->mmap_curr);
                microkit_dbg_puts("\n");
                return -12;
            }
            uintptr_t addr = sys_state->mmap_curr;
            sys_state->mmap_curr += aligned_size;
            tyn_memset((void *)addr, 0, aligned_size);
            clear_cache((void *)addr, (void *)(addr + aligned_size));
            microkit_dbg_puts("[synrc] SYS_mmap ret=");
            puthex64(addr);
            microkit_dbg_puts(" sz=");
            puthex64(aligned_size);
            microkit_dbg_puts("\n");
            return (long)addr;
        }

        case SYS_mprotect: {
            uintptr_t addr = (uintptr_t)a1;
            size_t len = (size_t)a2;
            int prot = (int)a3;
            if ((prot & 4) && addr && len) { // PROT_EXEC
                clear_cache((void *)addr, (void *)(addr + len));
            }
            return 0;
        }
        case SYS_munmap:
        case 233: // SYS_madvise — ignore
            return 0;

        case 279: // SYS_membarrier — BEAM SMP uses this
            __asm__ volatile("dsb ish; isb" ::: "memory");
            return 0;

        // ----------------------------------------------------------------
        // Process / Thread
        // ----------------------------------------------------------------
        case SYS_getpid:
            return 1;

        case SYS_getppid:
            return 0;

        case SYS_gettid:
            return 1;

        case SYS_sched_yield:
            sel4_yield();
            return 0;

        case SYS_sched_setaffinity:
            return 0;

        case SYS_sched_getaffinity: {
            if (a3 && (size_t)a2 >= sizeof(unsigned long)) {
                unsigned long *mask = (unsigned long *)a3;
                *mask = 1UL;
            }
            return 0;
        }

        case SYS_uname: {
            struct utsname_t {
                char sysname[65];
                char nodename[65];
                char release[65];
                char version[65];
                char machine[65];
                char domainname[65];
            } *u = (struct utsname_t *)a1;
            if (u) {
                tyn_memset(u, 0, sizeof(*u));
                tyn_strcpy(u->sysname, "sel4");
                tyn_strcpy(u->nodename, "synrc");
                tyn_strcpy(u->release, "0.7.0");
                tyn_strcpy(u->version, "#1 SMP");
                tyn_strcpy(u->machine, "aarch64");
            }
            return 0;
        }

        case SYS_set_tid_address:
            return 1;

        case SYS_prlimit64: {
            if (a4) {
                uint64_t *rlimit = (uint64_t *)a4;
                if (a2 == 7 /* RLIMIT_NOFILE */) {
                    rlimit[0] = 1024; // rlim_cur
                    rlimit[1] = 1024; // rlim_max
                } else {
                    rlimit[0] = ~(uint64_t)0; // RLIM_INFINITY
                    rlimit[1] = ~(uint64_t)0;
                }
            }
            return 0;
        }

        case SYS_getrlimit: {
            if (a2) {
                uint64_t *rlimit = (uint64_t *)a2;
                if (a1 == 7 /* RLIMIT_NOFILE */) {
                    rlimit[0] = 1024;
                    rlimit[1] = 1024;
                } else {
                    rlimit[0] = ~(uint64_t)0;
                    rlimit[1] = ~(uint64_t)0;
                }
            }
            return 0;
        }



        // ----------------------------------------------------------------
        // Signals
        // ----------------------------------------------------------------
        case SYS_rt_sigprocmask: {
            uint64_t *oldset = (uint64_t *)a3;
            if (oldset) *oldset = 0;
            return 0;
        }
        case SYS_rt_sigaction: {
            struct {
                void *sa_handler;
                unsigned long sa_flags;
                void *sa_restorer;
                uint64_t sa_mask;
            } *oldact = (void *)a3;
            if (oldact) {
                oldact->sa_handler = 0;
                oldact->sa_flags = 0;
                oldact->sa_restorer = 0;
                oldact->sa_mask = 0;
            }
            return 0;
        }
        case SYS_tkill:
        case SYS_tgkill: {
            int sig = (sysno == SYS_tkill) ? (int)a2 : (int)a3;
            if (sig == 6 || sig == 9 || sig == 15) {
                microkit_dbg_puts("[synrc] Process/thread received termination signal, shutting down cleanly.\n");
                for (;;) { sel4_yield(); }
            }
            return 0;
        }

        case SYS_signalfd4: {
            int fd = (int)a1;
            int flags = (int)a4;
            if (fd == -1) {
                int new_fd = fd_alloc((const vfs_file_t *)&sys_state->dev_pipe_file, 0);
                if (new_fd >= 0) sys_state->fd_table[new_fd].nonblock = (flags & 04000) ? 1 : 0;
                return new_fd;
            }
            return fd;
        }

        // ----------------------------------------------------------------
        // Time
        // ----------------------------------------------------------------
        case SYS_clock_gettime: {
            struct timespec { long tv_sec; long tv_nsec; } *ts = (void *)a2;
            if (ts) {
                ts->tv_sec = 1600000000;
                ts->tv_nsec = 0;
            }
            return 0;
        }

        case 114: { // SYS_clock_getres
            struct timespec { long tv_sec; long tv_nsec; } *res = (void *)a2;
            if (res) {
                res->tv_sec = 0;
                res->tv_nsec = 1; // 1 ns resolution
            }
            return 0;
        }

        case SYS_timerfd_create: {
            int flags = (int)a2;
            int fd = fd_alloc((const vfs_file_t *)&sys_state->dev_pipe_file, 0);
            if (fd >= 0) sys_state->fd_table[fd].nonblock = (flags & 04000) ? 1 : 0;
            return fd;
        }

        case SYS_gettimeofday: {
            tyn_timeval_t *tv = (tyn_timeval_t *)a1;
            if (tv) {
                int64_t sec;
                long nsec;
                get_fake_time(&sec, &nsec);
                tv->tv_sec = sec;
                tv->tv_usec = nsec / 1000;
            }
            return 0;
        }

        case SYS_nanosleep:
            return 0;

        // ----------------------------------------------------------------
        // Randomness
        // ----------------------------------------------------------------
        case SYS_getrandom: {
            uint8_t *buf = (uint8_t *)a1;
            size_t len = (size_t)a2;
            for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(i ^ 0xA5);
            return (long)len;
        }

        // ----------------------------------------------------------------
        // Socket family
        // ----------------------------------------------------------------
        case SYS_socketpair: {
            int *fds = (int *)a4;
            if (!fds) return -22; // EINVAL
            int fd0 = fd_alloc((const vfs_file_t *)&sys_state->dev_null_file, 0);
            int fd1 = fd_alloc((const vfs_file_t *)&sys_state->dev_null_file, 0);
            if (fd0 < 0 || fd1 < 0) return -24; // EMFILE
            fds[0] = fd0;
            fds[1] = fd1;
            return 0;
        }

        case SYS_socket:
            return fd_alloc((const vfs_file_t *)&sys_state->dev_null_file, 0);

        case SYS_bind:
        case SYS_listen:
        case SYS_connect:
        case SYS_setsockopt:
        case SYS_getsockopt:
            return 0;

        case 46: // SYS_ftruncate
        case 47: // SYS_fallocate
            return 0;

        case 88: // SYS_utimensat
        case 89: // SYS_acct
        case 90: // SYS_capget
        case 91: // SYS_capset
            return 0;

        case SYS_accept:
        case SYS_accept4:
            return -11; // EAGAIN

        case SYS_sendto:
            return (long)a3;

        case SYS_recvfrom:
            return -11; // EAGAIN

        // ----------------------------------------------------------------
        // I/O multiplexing
        // ----------------------------------------------------------------
        case SYS_epoll_create1:
            return fd_alloc((const vfs_file_t *)&sys_state->dev_null_file, 0);

        case SYS_epoll_ctl:
            return 0;

        case SYS_epoll_wait: {
            int timeout = (int)a4;
            if (timeout != 0) {
                sys_unlock();
                sel4_yield();
                sys_lock();
            }
            return 0;
        }

        case SYS_pselect6: {
            int nfds = (int)a1;
            if (nfds == 0) {
                // Sleep forever (or until interrupted)
                sys_unlock();
                sel4_yield();
                sys_lock();
                return -4; // EINTR
            }
            uint8_t *readfds = (uint8_t *)a2;
            uint8_t *writefds = (uint8_t *)a3;
            int ready = 0;
            if (nfds > 0) {
                int has_data = console_ring && (console_ring->rx_tail != console_ring->rx_head);
                for (int i = 0; i < nfds; i++) {
                    int byte_idx = i / 8;
                    int bit_mask = 1 << (i % 8);
                    int r_ready = 0, w_ready = 0;

                    if (readfds && (readfds[byte_idx] & bit_mask)) {
                        if (i == 0 && has_data) r_ready = 1;
                        else readfds[byte_idx] &= ~bit_mask;
                    }
                    if (writefds && (writefds[byte_idx] & bit_mask)) {
                        if (i == 1 || i == 2) w_ready = 1;
                        else writefds[byte_idx] &= ~bit_mask;
                    }

                    if (r_ready || w_ready) ready++;
                }
            }
            return ready;
        }

        case SYS_eventfd2: {
            int flags = (int)a2;
            int fd = fd_alloc((const vfs_file_t *)&sys_state->dev_pipe_file, 0);
            if (fd >= 0) sys_state->fd_table[fd].nonblock = (flags & 04000) ? 1 : 0;
            return fd;
        }

        case SYS_poll: {
            struct pollfd_ptr { int fd; short events; short revents; } *fds = (void *)a1;
            size_t nfds = (size_t)a2;
            int ready = 0;
            if (fds && nfds > 0) {
                int has_data = console_ring && (console_ring->rx_tail != console_ring->rx_head);
                for (size_t i = 0; i < nfds; i++) {
                    if (fds[i].fd == 0 && has_data) {
                        fds[i].revents = fds[i].events; // POLLIN
                        if (fds[i].revents) ready++;
                    } else if (fds[i].fd == 1 || fds[i].fd == 2) {
                        fds[i].revents = fds[i].events & 0x0004; // POLLOUT is 4 in Linux
                        if (fds[i].revents) ready++;
                    } else {
                        fds[i].revents = 0;
                    }
                }
            }
            return ready;
        }

        // ----------------------------------------------------------------
        // Process management — no real fork/exec support
        // ----------------------------------------------------------------
        case SYS_clone: {
            long flags = a1;
            uint64_t *child_stack = (uint64_t *)a2;
            long ptid = a3;
            long tls = a4;
            long ctid = a5;

            // fork() call (child_stack == NULL): fake it — return a synthetic PID
            // to the parent. We never actually duplicate execution.
            if (child_stack == 0) {
                microkit_dbg_puts("[synrc] SYS_clone: fork() -> returning fake child PID\n");
                return 1001; // fake child PID — child branch never runs
            }

            // In AArch64 Musl __clone, func and arg are saved on the child stack
            uint64_t func = child_stack[0];
            uint64_t arg = child_stack[1];

            microkit_dbg_puts("[synrc] SYS_clone: child_stack=");
            puthex64((uint64_t)child_stack);
            microkit_dbg_puts(" func=");
            puthex64(func);
            microkit_dbg_puts(" arg=");
            puthex64(arg);
            microkit_dbg_puts("\n");


            int slot = next_thread_slot;
            if (slot >= 8) {
                microkit_dbg_puts("[synrc] SYS_clone: ERROR - out of thread PDs (max 8)!\n");
                return -11; // -EAGAIN
            }
            next_thread_slot++;
            
            // Musl allocated the TCB, mapped the stack, and copied TLS! We just pass the pointers!
            mailbox[slot].tls = tls;
            mailbox[slot].child_stack = (void *)child_stack;
            mailbox[slot].start_routine = (void *(*)(void *))func;
            mailbox[slot].arg = (void *)arg;
            mailbox[slot].active = 1;
            
            long tid = slot + 1000;
            if (flags & 0x00080000) { // CLONE_CHILD_SETTID
                if (ctid) *(int *)ctid = tid;
            }
            if (flags & 0x00000100) { // CLONE_PARENT_SETTID
                if (ptid) *(int *)ptid = tid;
            }

            microkit_notify(slot + 3); // Channels 3 to 18 map to slots 0 to 15

            return tid; // Return Thread ID to parent
        }

        case SYS_dup: //
        case SYS_dup3: //
            return -38; // ENOSYS

        case SYS_getsockname: //
        case SYS_sendmsg: //
        case SYS_recvmsg: //
            return -38; // ENOSYS

        case SYS_wait4:
        case SYS_waitid:
            return -10; // ECHILD

        case SYS_kill:
            return 0;


        // ----------------------------------------------------------------
        // Exit
        // ----------------------------------------------------------------
        case 167: // SYS_prctl
            return 0;

        case SYS_exit: {
            long code = a1;
            char dbg[64];
            // Poor man's sprintf
            int i = 0;
            dbg[i++] = '['; dbg[i++] = 's'; dbg[i++] = 'y'; dbg[i++] = 'n'; dbg[i++] = 'r'; dbg[i++] = 'c'; dbg[i++] = ']'; dbg[i++] = ' ';
            dbg[i++] = 'B'; dbg[i++] = 'E'; dbg[i++] = 'A'; dbg[i++] = 'M'; dbg[i++] = ' '; dbg[i++] = 'e'; dbg[i++] = 'x'; dbg[i++] = 'i'; dbg[i++] = 't'; dbg[i++] = 'e'; dbg[i++] = 'd'; dbg[i++] = ' '; dbg[i++] = 'w'; dbg[i++] = 'i'; dbg[i++] = 't'; dbg[i++] = 'h'; dbg[i++] = ' '; dbg[i++] = 'c'; dbg[i++] = 'o'; dbg[i++] = 'd'; dbg[i++] = 'e'; dbg[i++] = ':'; dbg[i++] = ' ';
            if (code == 0) dbg[i++] = '0';
            else {
                long temp = code < 0 ? -code : code;
                if (code < 0) dbg[i++] = '-';
                char buf[16];
                int j = 0;
                while (temp > 0) { buf[j++] = (temp % 10) + '0'; temp /= 10; }
                while (j > 0) dbg[i++] = buf[--j];
            }
            dbg[i++] = '\n';
            dbg[i++] = '\0';
            microkit_dbg_puts(dbg);
            for (;;) { sel4_yield(); }
        }

        case SYS_exit_group: {
            long code = a1;
            char dbg[64];
            int i = 0;
            dbg[i++] = '['; dbg[i++] = 's'; dbg[i++] = 'y'; dbg[i++] = 'n'; dbg[i++] = 'r'; dbg[i++] = 'c'; dbg[i++] = ']'; dbg[i++] = ' ';
            dbg[i++] = 'B'; dbg[i++] = 'E'; dbg[i++] = 'A'; dbg[i++] = 'M'; dbg[i++] = ' '; dbg[i++] = 'g'; dbg[i++] = 'r'; dbg[i++] = 'o'; dbg[i++] = 'u'; dbg[i++] = 'p'; dbg[i++] = ' '; dbg[i++] = 'e'; dbg[i++] = 'x'; dbg[i++] = 'i'; dbg[i++] = 't'; dbg[i++] = 'e'; dbg[i++] = 'd'; dbg[i++] = ':'; dbg[i++] = ' ';
            if (code == 0) dbg[i++] = '0';
            else {
                long temp = code < 0 ? -code : code;
                if (code < 0) dbg[i++] = '-';
                char buf[16];
                int j = 0;
                while (temp > 0) { buf[j++] = (temp % 10) + '0'; temp /= 10; }
                while (j > 0) dbg[i++] = buf[--j];
            }
            dbg[i++] = '\n';
            dbg[i++] = '\0';
            microkit_dbg_puts(dbg);
            for (;;) { sel4_yield(); }
        }

        default: {
            char dbg[32] = "[synrc] UNHANDLED SYSCALL: ";
            long n = sysno;
            int i = 0;
            char buf[16];
            if (n == 0) buf[i++] = '0';
            else {
                long temp = n;
                while (temp > 0) { buf[i++] = (temp % 10) + '0'; temp /= 10; }
            }
            microkit_dbg_puts(dbg);
            while (i > 0) {
                char c_str[2] = {buf[--i], 0};
                microkit_dbg_puts(c_str);
            }
            microkit_dbg_puts("\n");
            return -38; // ENOSYS
        }
    }
}

long tyn_syscall_dispatch(long sysno, long a1, long a2, long a3, long a4, long a5, long a6) {
    if (sysno == SYS_sched_yield || sysno == SYS_futex || sysno == SYS_nanosleep || sysno == SYS_exit || sysno == SYS_exit_group) {
        // Handle without lock to avoid deadlock
        return do_syscall(sysno, a1, a2, a3, a4, a5, a6);
    }
    sys_lock();
    long ret = do_syscall(sysno, a1, a2, a3, a4, a5, a6);
    sys_unlock();
    return ret;
}
