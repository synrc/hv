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
static uintptr_t heap_curr  = 0x30000000;
static uintptr_t mmap_curr  = 0x80000000;

static uint64_t fake_timer_nsec = 1000000000ULL; // start at 1 second

static void get_fake_time(int64_t *sec, long *nsec) {
    fake_timer_nsec += 100000; // increment 100us per call
    *sec = (int64_t)(fake_timer_nsec / 1000000000ULL);
    *nsec = (long)(fake_timer_nsec % 1000000000ULL);
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
#define FD_MAX 64

typedef struct {
    const vfs_file_t *file;
    size_t offset;
    int    used;
    char   dir_path[128];
} fd_entry_t;

static fd_entry_t fd_table[FD_MAX];

static const vfs_file_t dev_null_file = {
    .path = "/dev/null",
    .data = 0,
    .size = 0
};

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

static int fd_alloc(const vfs_file_t *f, const char *dir_path) {
    for (int i = 3; i < FD_MAX; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = 1;
            fd_table[i].file = f;
            fd_table[i].offset = 0;
            if (dir_path) {
                tyn_strncpy(fd_table[i].dir_path, dir_path, 128);
            } else {
                fd_table[i].dir_path[0] = '\0';
            }
            return i;
        }
    }
    return -1;
}

void tyn_syscall_init(void) {
    microkit_dbg_puts("[synrc] Synrc host trap dispatcher online (~50 musl syscall handlers ready)\n");
    for (int i = 0; i < FD_MAX; i++) {
        fd_table[i].file   = 0;
        fd_table[i].offset = 0;
        fd_table[i].used   = 0;
    }
}

// ---------------------------------------------------------------------------
// Syscall dispatcher
// ---------------------------------------------------------------------------
long tyn_syscall_dispatch(long sysno, long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;

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
            int fd0 = fd_alloc(&dev_null_file, 0);
            int fd1 = fd_alloc(&dev_null_file, 0);
            if (fd0 < 0 || fd1 < 0) return -1;
            pipefd[0] = fd0;
            pipefd[1] = fd1;
            return 0;
        }

        case SYS_fcntl:
            return 0;

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

        case SYS_read: {
            int fd = (int)a1;
            char *buf = (char *)a2;
            size_t count = (size_t)a3;
            if (fd == 0) {
                size_t c = console_ring_read_rx(console_ring, buf, count);
                if (c == 0) return -11; // EAGAIN
                return (long)c;
            }
            if (fd >= 3 && fd < FD_MAX && fd_table[fd].used) {
                fd_entry_t *e = &fd_table[fd];
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
                console_ring_write_tx(console_ring, buf, len);
                microkit_notify(1);
                return a3;
            }
            if (fd >= 3 && fd < FD_MAX && fd_table[fd].used) {
                if (streq(fd_table[fd].file->path, "/dev/null")) return a3;
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
                    console_ring_write_tx(console_ring, buf, len);
                    total += len;
                }
                microkit_notify(1);
                return total;
            }
            if (fd >= 3 && fd < FD_MAX && fd_table[fd].used) {
                if (streq(fd_table[fd].file->path, "/dev/null")) {
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
                f = &dev_null_file;
            } else {
                f = vfs_lookup(path);
            }
            if (!f) return -2; // ENOENT
            const char *dp = streq(f->path, "DIR") ? path : 0;
            int fd = fd_alloc(f, dp);
            return fd < 0 ? -24 : (long)fd; // EMFILE
        }

        case SYS_close:
            if ((int)a1 >= 3 && (int)a1 < FD_MAX) {
                fd_table[(int)a1].used = 0;
            }
            return 0;

        case SYS_lseek: {
            int fd = (int)a1;
            long offset = a2;
            int whence = (int)a3;
            if (fd >= 3 && fd < FD_MAX && fd_table[fd].used) {
                fd_entry_t *e = &fd_table[fd];
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
            if (fd >= 3 && fd < FD_MAX && fd_table[fd].used) {
                uint8_t *st = (uint8_t *)a2;
                if (!st) return -14; // EFAULT
                for (int i = 0; i < 128; i++) st[i] = 0;

                if (streq(fd_table[fd].file->path, "DIR")) {
                    // st_mode at offset 16: set S_IFDIR | 0755 = 0x41ED
                    st[16] = 0xED; st[17] = 0x41;
                    st[20] = 2; // nlink
                } else {
                    // st_mode at offset 16: set S_IFREG | 0644 = 0x81A4
                    st[16] = 0xA4; st[17] = 0x81;
                    st[20] = 1;
                    // st_size at offset 48
                    size_t sz = fd_table[fd].file->size;
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
                if (streq(f->path, "DIR")) {
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

            if (fd >= 3 && fd < FD_MAX && fd_table[fd].used) {
                if (fd_table[fd].dir_path[0] != '\0') {
                    int res = vfs_getdents(fd_table[fd].dir_path, buf, count, &fd_table[fd].offset);
                    if (res >= 0) return res;
                }
                return 0; // Not a directory or empty
            }
            return -9; // EBADF
        }

        case SYS_readlink: {
            // /proc/self/exe -> /otp/erts-9.3/bin/beam
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
            if (a1 == 0) return (long)heap_curr;
            if ((uintptr_t)a1 > 0x31000000) return -12; // ENOMEM
            heap_curr = (uintptr_t)a1;
            return (long)heap_curr;

        case SYS_mmap: {
            size_t size    = (size_t)a2;
            int    flags   = (int)a4;
            int    MAP_FIXED_FLAG = 0x10;

            uintptr_t aligned_size = (size + 0xFFF) & ~(uintptr_t)0xFFF;

            // MAP_FIXED: ERTS commits pages within a previously reserved range.
            if (flags & MAP_FIXED_FLAG) {
                return (long)(uintptr_t)a1;
            }

            if (mmap_curr + aligned_size > 0xC8000000) {
                microkit_dbg_puts("[synrc] SYS_mmap ENOMEM!\n");
                return -12; // ENOMEM
            }
            uintptr_t addr = mmap_curr;
            mmap_curr += aligned_size;
            return (long)addr;
        }


        case SYS_mprotect:
        case SYS_munmap:
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

        case SYS_futex:
            return 0;

        // ----------------------------------------------------------------
        // Signals
        // ----------------------------------------------------------------
        case SYS_rt_sigprocmask:
        case SYS_rt_sigaction:
        case SYS_tkill:
        case SYS_tgkill:
            return 0;

        // ----------------------------------------------------------------
        // Time
        // ----------------------------------------------------------------
        case SYS_clock_gettime: {
            tyn_timespec_t *tp = (tyn_timespec_t *)a2;
            if (tp) {
                int64_t sec;
                long nsec;
                get_fake_time(&sec, &nsec);
                tp->tv_sec = sec;
                tp->tv_nsec = nsec;
            }
            return 0;
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
        // Socket family — stubs that return success/EAGAIN as appropriate
        // ----------------------------------------------------------------
        case SYS_socketpair: {
            // BEAM uses socketpair(AF_UNIX, SOCK_STREAM, 0, fds) in spawn_init
            // Give it two dev_null fds so spawn_init succeeds
            int *fds = (int *)a4;  // on AArch64: a1=domain a2=type a3=proto a4=sv
            if (!fds) return -22; // EINVAL
            int fd0 = fd_alloc(&dev_null_file, 0);
            int fd1 = fd_alloc(&dev_null_file, 0);
            if (fd0 < 0 || fd1 < 0) return -24; // EMFILE
            fds[0] = fd0;
            fds[1] = fd1;
            return 0;
        }

        case SYS_socket:
            // Return a dev_null fd so callers don't get -1 and crash
            return fd_alloc(&dev_null_file, 0);

        case SYS_bind:
        case SYS_listen:
        case SYS_connect:
        case SYS_setsockopt:
        case SYS_getsockopt:
            return 0;

        case SYS_accept:
        case SYS_accept4:
            return -11; // EAGAIN — no connections

        case SYS_sendto:
            return (long)a3; // pretend we sent all bytes

        case SYS_recvfrom:
            return -11; // EAGAIN — no data

        // ----------------------------------------------------------------
        // I/O multiplexing — return immediately (no fds ready)
        // ----------------------------------------------------------------
        case SYS_epoll_create1:
            return fd_alloc(&dev_null_file, 0);

        case SYS_epoll_ctl:
            return 0;

        case SYS_epoll_wait:
            return 0;

        case SYS_pselect6: {
            int nfds = (int)a1;
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

        case SYS_eventfd2:
            return fd_alloc(&dev_null_file, 0);

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
        case SYS_clone:
            return -11; // EAGAIN — cannot create child process

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

        case SYS_signalfd4:
            return fd_alloc(&dev_null_file, 0);

        // ----------------------------------------------------------------
        // Exit
        // ----------------------------------------------------------------
        case SYS_exit:
        case SYS_exit_group:
            microkit_dbg_puts("[synrc] BEAM runtime process exited.\n");
            for (;;) {};

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
