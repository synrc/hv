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
    seL4_Yield();
}

static inline void sys_lock_dbg(long sysno) {
    uint32_t spins = 0;
    while (__atomic_test_and_set(&sys_state->lock, __ATOMIC_ACQUIRE)) {
        spins++;
        if (spins == 100000) {
            microkit_dbg_puts("[synrc] SPINLOCK HELD BY sysno=");
            puthex64((uint64_t)sys_state->lock_owner_sysno);
            microkit_dbg_puts(" WAITER=");
            microkit_dbg_puts(microkit_name);
            microkit_dbg_puts(" sysno=");
            puthex64((uint64_t)sysno);
            microkit_dbg_puts("\n");
        }
        sel4_yield();
    }
    sys_state->lock_owner_sysno = (uint32_t)sysno;
}

static inline void sys_lock(void) {
    sys_lock_dbg(0);
}

static inline void sys_unlock(void) {
    sys_state->lock_owner_sysno = 0;
    __atomic_clear(&sys_state->lock, __ATOMIC_RELEASE);
}

static inline void __attribute__((unused)) synrc_syscall_lock(void) {
    sys_lock();
}
static void get_fake_time(int64_t *sec, long *nsec) {
    sys_state->fake_timer_nsec += 100000; // increment 100us per call
    *sec = (int64_t)(sys_state->fake_timer_nsec / 1000000000ULL);
    *nsec = (long)(sys_state->fake_timer_nsec % 1000000000ULL);
}

static void advance_fake_time(uint64_t nsec) {
    sys_state->fake_timer_nsec += nsec;
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
            sys_state->fd_table[i].pipe_head = 0;
            sys_state->fd_table[i].pipe_tail = 0;
            sys_state->fd_table[i].epoll_registered = 0;
            sys_state->fd_table[i].epoll_epfd = -1;
            sys_state->fd_table[i].pipe_read_fd = -1;
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


static struct mailbox_slot *const mailbox = (struct mailbox_slot *)0x21000000;

void puthex64(uint64_t val) {
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

    if (sys_state->pending_notify != 0 && microkit_name[0] == 's' && microkit_name[1] == 'y' && microkit_name[2] == 'n') {
        uint32_t target_ch = sys_state->pending_notify;
        sys_state->pending_notify = 0;
        microkit_dbg_puts("[synrc] Proxying pending notification to channel ");
        puthex64(target_ch);
        microkit_dbg_puts("\n");
        microkit_notify(target_ch);
    }

    if (sysno != 63 && sysno != 113 && sysno != 124 && sysno != 72 && sysno != 73 && sysno != 40 && sysno != 66) {
        microkit_dbg_puts("[");
        microkit_dbg_puts(microkit_name);
        microkit_dbg_puts("] sysno=");
        puthex64(sysno);
        microkit_dbg_puts(" a1=");
        puthex64(a1);
        microkit_dbg_puts("\n");
    }

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
            int *fds = (int *)a1;
            int flags = (int)a2;
            int r = fd_alloc((const vfs_file_t *)&sys_state->dev_pipe_file, 0);
            int w = fd_alloc((const vfs_file_t *)&sys_state->dev_pipe_file, 0);
            if (r < 0 || w < 0) return -24;
            sys_state->fd_table[w].file = &sys_state->dev_pipe_file;
            sys_state->fd_table[w].pipe_read_fd = r;
            if (flags & 04000) {
                sys_state->fd_table[r].nonblock = 1;
                sys_state->fd_table[w].nonblock = 1;
            } else {
                sys_state->fd_table[r].nonblock = 0;
                sys_state->fd_table[w].nonblock = 0;
            }
            fds[0] = r;
            fds[1] = w;
            microkit_dbg_puts("[synrc] SYS_pipe2 created read_fd=");
            puthex64(r);
            microkit_dbg_puts(" write_fd=");
            puthex64(w);
            microkit_dbg_puts(" flags=");
            puthex64((uint64_t)flags);
            microkit_dbg_puts("\n");
            return 0;
        }

        case SYS_fcntl: {
            int fd = (int)a1;
            int cmd = (int)a2;
            long arg = a3;
            
            microkit_dbg_puts("[synrc] SYS_fcntl fd=");
            puthex64(fd);
            microkit_dbg_puts(" cmd=");
            puthex64(cmd);
            microkit_dbg_puts(" arg=");
            puthex64(arg);
            microkit_dbg_puts("\n");
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
                if (fd == 100) { // timerfd
                    // ...
                }
                return 0; // Fake success for other commands
            }
            return -9; // EBADF
        }

        case SYS_ioctl: {
            int fd = (int)a1;
            long req = a2;
            if (fd == 0 || fd == 1 || fd == 2) {
                struct termios_ptr {
                    uint32_t c_iflag;
                    uint32_t c_oflag;
                    uint32_t c_cflag;
                    uint32_t c_lflag;
                    uint8_t c_line;
                    uint8_t c_cc[32];
                    uint32_t c_ispeed;
                    uint32_t c_ospeed;
                };

                if (req == 0x5401) { // TCGETS
                    struct termios_ptr *t = (struct termios_ptr *)a3;
                    if (t) {
                        for (size_t i = 0; i < sizeof(*t); i++) ((char *)t)[i] = 0;
                        t->c_iflag = 0x500;  // ICRNL | IXON
                        t->c_oflag = 0x5;    // OPOST | ONLCR
                        t->c_cflag = 0xbf;   // B38400 | CS8 | CREAD
                        t->c_lflag = 0x8a3b; // ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN
                    }
                    return 0;
                }
                if (req == 0x5402 || req == 0x5403 || req == 0x5404) { // TCSETS, TCSETSW, TCSETSF
                    // Accept termios mode changes (raw/cooked mode switches from Erlang user_drv)
                    return 0;
                }
                if (req == 0x5413) { // TIOCGWINSZ
                    uint16_t *ws = (uint16_t *)a3;
                    if (ws) {
                        ws[0] = 24; // rows
                        ws[1] = 80; // cols
                        ws[2] = 0;  // xpixel
                        ws[3] = 0;  // ypixel
                    }
                    return 0;
                }
                if (req == 0x5414) { // TIOCSWINSZ
                    return 0;
                }
                if (req == 0x541B) { // FIONREAD
                    int *cnt = (int *)a3;
                    if (cnt) {
                        *cnt = (console_ring && console_ring->rx_head != console_ring->rx_tail) ? 1 : 0;
                    }
                    return 0;
                }
                if (req == 0x540F || req == 0x5410) { // TIOCGPGRP, TIOCSPGRP
                    int *pgrp = (int *)a3;
                    if (pgrp) *pgrp = 1000;
                    return 0;
                }
                return 0; // fake success for other terminal ioctls
            }

            return -25; // ENOTTY
        }
        case SYS_set_tid_address: {
             int *ctid = (int *)a1;
             uint64_t tls;
             __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
             long tid = 999;
             for (int i = 0; i < 16; i++) {
                 if (mailbox[i].tls == tls && tls != 0) {
                     tid = 1000 + i;
                     break;
                 }
             }
             if (ctid) *ctid = (int)tid;
             return tid;
        }
        case SYS_futex: {
            int *uaddr = (int *)a1;
            int futex_op = (int)a2;
            int val = (int)a3;
            int cmd = futex_op & 127;

            if (cmd == 0) { // FUTEX_WAIT
                if (!uaddr) return -14; // EFAULT

                sys_lock();
                microkit_dbg_puts("[");
                microkit_dbg_puts(microkit_name);
                microkit_dbg_puts("] FUTEX_WAIT uaddr=");
                puthex64((uint64_t)uaddr);
                microkit_dbg_puts(" val=");
                puthex64((uint64_t)val);
                microkit_dbg_puts(" *uaddr=");
                if (uaddr) puthex64((uint64_t)*uaddr);
                microkit_dbg_puts("\n");

                if (*uaddr != val) {
                    sys_unlock();
                    return -11; // -EAGAIN
                }

                int slot = -1;
                for (int i = 0; i < 16; i++) {
                    if (sys_state->futex_waiters[i].uaddr == 0) {
                        slot = i;
                        sys_state->futex_waiters[i].uaddr = uaddr;
                        sys_state->futex_waiters[i].val = val;
                        sys_state->futex_waiters[i].woken = 0;
                        break;
                    }
                }
                sys_unlock();

                if (slot < 0) {
                    while (*uaddr == val) {
                        if (sys_state->pending_notify != 0 && microkit_name[0] == 's' && microkit_name[1] == 'y' && microkit_name[2] == 'n') {
                            uint32_t target_ch = sys_state->pending_notify;
                            sys_state->pending_notify = 0;
                            microkit_dbg_puts("[synrc] Proxying pending notification during futex wait to channel ");
                            puthex64(target_ch);
                            microkit_dbg_puts("\n");
                            microkit_notify(target_ch);
                        }
                        seL4_Yield();
                    }
                    return 0;
                }

                int yield_count = 0;
                while (*uaddr == val && !__atomic_load_n(&sys_state->futex_waiters[slot].woken, __ATOMIC_ACQUIRE)) {
                    if (sys_state->pending_notify != 0 && microkit_name[0] == 's' && microkit_name[1] == 'y' && microkit_name[2] == 'n') {
                        uint32_t target_ch = sys_state->pending_notify;
                        sys_state->pending_notify = 0;
                        microkit_dbg_puts("[synrc] Proxying pending notification during futex wait to channel ");
                        puthex64(target_ch);
                        microkit_dbg_puts("\n");
                        microkit_notify(target_ch);
                    }
                    if (++yield_count == 100) {
                        yield_count = 0;
                        if ((uint64_t)uaddr == 0x0089d300ULL && *uaddr == val) {
                            // Release Musl's __thread_list_lock if thread creation finished
                            *uaddr = 0;
                            break;
                        }
                    }
                    seL4_Yield();
                }

                sys_lock();
                sys_state->futex_waiters[slot].uaddr = 0;
                sys_state->futex_waiters[slot].woken = 0;
                sys_unlock();
                return 0;
            } else if (cmd == 1) { // FUTEX_WAKE
                if (!uaddr) return 0;

                microkit_dbg_puts("[");
                microkit_dbg_puts(microkit_name);
                microkit_dbg_puts("] FUTEX_WAKE uaddr=");
                puthex64((uint64_t)uaddr);
                microkit_dbg_puts(" val=");
                puthex64((uint64_t)val);
                microkit_dbg_puts("\n");

                sys_lock();
                int woken_count = 0;
                int max_wake = val;
                for (int i = 0; i < 16; i++) {
                    int *w_uaddr = sys_state->futex_waiters[i].uaddr;
                    if (w_uaddr && (w_uaddr == uaddr || *w_uaddr != sys_state->futex_waiters[i].val)) {
                        sys_state->futex_waiters[i].woken = 1;
                        woken_count++;
                        if (woken_count >= max_wake) break;
                    }
                }
                sys_unlock();
                return woken_count;
            }
            return 0;
        }

        case SYS_read: {
            int fd = (int)a1;
            char *buf = (char *)a2;
            size_t count = (size_t)a3;
            
            if (fd == 100) { // timerfd
                if (count >= 8) {
                    uint64_t expirations = 1;
                    char *src = (char *)&expirations;
                    for (int i = 0; i < 8; i++) buf[i] = src[i];
                    sys_state->timerfd_active = 0; // one-shot for simplicity
                    return 8;
                }
                return -22; // EINVAL
            }
            
            if (fd == 0) {
                if (count == 0) return 0;
                size_t c = console_ring_read_rx(console_ring, buf, count);
                if (c > 0) return (long)c;
                if (sys_state->fd_table[0].nonblock) return -11; // EAGAIN
                while ((c = console_ring_read_rx(console_ring, buf, count)) == 0) {
                    sys_unlock();
                    seL4_Yield();
                    sys_lock();
                }
                return (long)c;
            }
            if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                fd_entry_t *e = (fd_entry_t *)&sys_state->fd_table[fd];
                if (e->file == &sys_state->dev_pipe_file) {
                    if (e->pipe_head != e->pipe_tail) {
                        size_t copied = 0;
                        while (e->pipe_head != e->pipe_tail && copied < count) {
                            buf[copied++] = e->pipe_buf[e->pipe_tail];
                            e->pipe_tail = (e->pipe_tail + 1) % 32;
                        }
                        return copied;
                    }
                    if (fd == 25 || (e->nonblock == 0 && fd < 27)) {
                        // Signal pipe: block waiting for signal notifications
                        while (e->pipe_head == e->pipe_tail) {
                            sys_unlock();
                            seL4_Yield();
                            sys_lock();
                        }
                        size_t copied = 0;
                        while (e->pipe_head != e->pipe_tail && copied < count) {
                            buf[copied++] = e->pipe_buf[e->pipe_tail];
                            e->pipe_tail = (e->pipe_tail + 1) % 32;
                        }
                        return copied;
                    }
                    sys_unlock();
                    seL4_Yield();
                    sys_lock();
                    return -11; // EAGAIN for sys_msg and other non-blocking driver pipes
                }
                if (e->file == &sys_state->dev_null_file) {
                    sys_unlock();
                    seL4_Yield();
                    sys_lock();
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
                fd_entry_t *e = (fd_entry_t *)&sys_state->fd_table[fd];
                if (e->file == &sys_state->dev_pipe_file) {
                    if (e->pipe_read_fd >= 0) {
                        e = (fd_entry_t *)&sys_state->fd_table[e->pipe_read_fd];
                    }
                    const char *src = (const char *)a2;
                    for (size_t i = 0; i < a3; i++) {
                        e->pipe_buf[e->pipe_head] = src[i];
                        e->pipe_head = (e->pipe_head + 1) % 32;
                    }
                    asm volatile("sev");
                    return a3;
                }
                if (sys_state->fd_table[fd].file == &sys_state->dev_null_file) return a3;
                vfs_file_t *vf = (vfs_file_t *)e->file;
                if (vf && vf->data) {
                    const char *src = (const char *)a2;
                    size_t len = (size_t)a3;
                    uint8_t *dst = (uint8_t *)vf->data;
                    for (size_t i = 0; i < len; i++) {
                        dst[e->offset + i] = (uint8_t)src[i];
                    }
                    e->offset += len;
                    if (e->offset > vf->size) vf->size = e->offset;
                    return (long)len;
                }
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
                fd_entry_t *e = (fd_entry_t *)&sys_state->fd_table[fd];
                if (e->file == &sys_state->dev_pipe_file) {
                    if (e->pipe_read_fd >= 0) {
                        e = (fd_entry_t *)&sys_state->fd_table[e->pipe_read_fd];
                    }
                    struct iovec { void *iov_base; size_t iov_len; };
                    const struct iovec *iov = (const struct iovec *)a2;
                    long total = 0;
                    for (int i = 0; i < (int)a3; i++) {
                        const char *src = (const char *)iov[i].iov_base;
                        for (size_t j = 0; j < iov[i].iov_len; j++) {
                            e->pipe_buf[e->pipe_head] = src[j];
                            e->pipe_head = (e->pipe_head + 1) % 32;
                        }
                        total += iov[i].iov_len;
                    }
                    asm volatile("sev");
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
            int flags = (int)a3;
            const vfs_file_t *f = 0;
            if (streq(path, "/dev/null")) {
                f = (const vfs_file_t *)&sys_state->dev_null_file;
            } else {
                f = vfs_lookup(path);
                if (!f && (flags & 0100)) { // O_CREAT
                    f = vfs_create_file(path);
                }
            }
            if (!f) {
                microkit_dbg_puts("[synrc] SYS_openat ENOENT path: ");
                if (path) microkit_dbg_puts(path);
                microkit_dbg_puts("\n");
                return -2; // ENOENT
            }

            microkit_dbg_puts("[synrc] SYS_openat OK path: ");
            if (path) microkit_dbg_puts(path);
            microkit_dbg_puts("\n");

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

            microkit_dbg_puts("[synrc] fstatat: ");
            microkit_dbg_puts(path);
            microkit_dbg_puts("\n");

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

        case SYS_readlink: { // readlinkat (dirfd, pathname, buf, bufsiz)
            const char *pathname = (const char *)a2;
            char *buf = (char *)a3;
            size_t bufsiz = (size_t)a4;
            
            microkit_dbg_puts("[synrc] readlinkat: ");
            microkit_dbg_puts(pathname);
            microkit_dbg_puts("\n");
            
            const char *exe = "/otp/erts-9.3/bin/beam";
            if (pathname && pathname[0] == '/' && pathname[1] == 'p' && pathname[2] == 'r' && pathname[3] == 'o' && pathname[4] == 'c' && pathname[5] == '/' && pathname[6] == 's' && pathname[7] == 'e' && pathname[8] == 'l' && pathname[9] == 'f' && pathname[10] == '/' && pathname[11] == 'e' && pathname[12] == 'x' && pathname[13] == 'e') {
                size_t len = 0;
                while (exe[len]) len++;
                if (len >= bufsiz) len = bufsiz - 1;
                for (size_t i = 0; i < len; i++) buf[i] = exe[i];
                buf[len] = '\0';
                return (long)len;
            }
            return -2; // ENOENT
        }

        // ----------------------------------------------------------------
        // Memory
        // ----------------------------------------------------------------
        case SYS_brk:
            if (a1 == 0) return (long)sys_state->heap_curr;
            if ((uintptr_t)a1 > 0x4F000000) return -12; // ENOMEM
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
            return 1000;

        case SYS_getppid:
            return 0;

        case SYS_gettid: {
             uint64_t tls;
             __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
             for (int i = 0; i < 16; i++) {
                 if (mailbox[i].tls == tls && tls != 0) {
                     return 1000 + i;
                 }
             }
             return 999; // Main thread
        }
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
                int64_t sec;
                long nsec;
                get_fake_time(&sec, &nsec);
                ts->tv_sec = sec;
                ts->tv_nsec = nsec;
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

        case SYS_timerfd_create:
            return 100; // Fake FD for timerfd

        case SYS_timerfd_settime: {
            int fd = (int)a1;
            int flags = (int)a2; // TFD_TIMER_ABSTIME?
            
            struct itimerspec_abi {
                timespec_abi_t it_interval;
                timespec_abi_t it_value;
            };
            const struct itimerspec_abi *new_value = (const struct itimerspec_abi *)a3;
            
            if (fd == 100 && new_value) {
                if (new_value->it_value.tv_sec == 0 && new_value->it_value.tv_nsec == 0) {
                    sys_state->timerfd_active = 0;
                } else {
                    int64_t sec;
                    long nsec;
                    get_fake_time(&sec, &nsec);
                    uint64_t now_ns = (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
                    
                    uint64_t expire_ns = (uint64_t)new_value->it_value.tv_sec * 1000000000ULL + (uint64_t)new_value->it_value.tv_nsec;
                    
                    if ((flags & 1) == 0) { // TFD_TIMER_ABSTIME is 1
                        // Relative time
                        sys_state->timerfd_expire_nsec = now_ns + expire_ns;
                    } else {
                        // Absolute time
                        sys_state->timerfd_expire_nsec = expire_ns;
                    }
                    sys_state->timerfd_active = 1;
                }
                return 0;
            }
            return -9; // EBADF
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

        case SYS_epoll_ctl: {
            int epfd = (int)a1;
            int op = (int)a2;
            int target_fd = (int)a3;
            epoll_event_abi_t *ev = (epoll_event_abi_t *)a4;

            if (target_fd == 0 || target_fd == 1 || target_fd == 2) {
                if (op == 1 /* EPOLL_CTL_ADD */ || op == 3 /* EPOLL_CTL_MOD */) {
                    if (target_fd == 0) {
                        sys_state->epoll_stdin_registered = 1;
                        sys_state->epoll_stdin_epfd = epfd;
                        sys_state->epoll_stdin_events = ev ? ev->events : 0x01;
                        sys_state->epoll_stdin_data = ev ? ev->data : 0;
                    } else if (target_fd == 1) {
                        sys_state->epoll_stdout_registered = 1;
                        sys_state->epoll_stdout_epfd = epfd;
                        sys_state->epoll_stdout_events = ev ? ev->events : 0x04;
                        sys_state->epoll_stdout_data = ev ? ev->data : 0;
                    } else if (target_fd == 2) {
                        sys_state->epoll_stderr_registered = 1;
                        sys_state->epoll_stderr_epfd = epfd;
                        sys_state->epoll_stderr_events = ev ? ev->events : 0x04;
                        sys_state->epoll_stderr_data = ev ? ev->data : 0;
                    }
                } else if (op == 2 /* EPOLL_CTL_DEL */) {
                    if (target_fd == 0) sys_state->epoll_stdin_registered = 0;
                    if (target_fd == 1) sys_state->epoll_stdout_registered = 0;
                    if (target_fd == 2) sys_state->epoll_stderr_registered = 0;
                }
                return 0;
            }
            if (target_fd >= 3 && target_fd < FD_MAX && sys_state->fd_table[target_fd].used) {
                if (op == 1 /* EPOLL_CTL_ADD */ || op == 3 /* EPOLL_CTL_MOD */) {
                    sys_state->fd_table[target_fd].epoll_registered = 1;
                    sys_state->fd_table[target_fd].epoll_epfd = epfd;
                    sys_state->fd_table[target_fd].epoll_events = ev ? ev->events : 0x01;
                    sys_state->fd_table[target_fd].epoll_data = ev ? ev->data : 0;
                    microkit_dbg_puts("[synrc] SYS_epoll_ctl ADD/MOD fd=");
                    puthex64(target_fd);
                    microkit_dbg_puts(" to epfd=");
                    puthex64(epfd);
                    microkit_dbg_puts("\n");
                } else if (op == 2 /* EPOLL_CTL_DEL */) {
                    sys_state->fd_table[target_fd].epoll_registered = 0;
                    microkit_dbg_puts("[synrc] SYS_epoll_ctl DEL fd=");
                    puthex64(target_fd);
                    microkit_dbg_puts(" from epfd=");
                    puthex64(epfd);
                    microkit_dbg_puts("\n");
                }
            }
            return 0;
        }

        case SYS_epoll_wait: {
            int epfd = (int)a1;
            epoll_event_abi_t *events = (epoll_event_abi_t *)a2;
            int maxevents = (int)a3;
            int timeout = (int)a4;

            microkit_dbg_puts("[synrc] SYS_epoll_wait epfd=");
            puthex64(epfd);
            microkit_dbg_puts(" timeout=");
            puthex64((uint64_t)timeout);
            microkit_dbg_puts("\n");

            sys_unlock(); // Release global spinlock while waiting for I/O

            int yields = 0;
            for (;;) {
                int ready_count = 0;
                
                if (sys_state->epoll_stdin_registered && sys_state->epoll_stdin_epfd == epfd && events) {
                    uint32_t ready_events = 0;
                    if (console_ring && console_ring->rx_head != console_ring->rx_tail) ready_events |= 0x01; // EPOLLIN
                    if (ready_events & sys_state->epoll_stdin_events) {
                        events[ready_count].events = ready_events & sys_state->epoll_stdin_events;
                        events[ready_count].data = sys_state->epoll_stdin_data;
                        ready_count++;
                    }
                }
                if (sys_state->epoll_stdout_registered && sys_state->epoll_stdout_epfd == epfd && maxevents > ready_count && events) {
                    uint32_t ready_events = 0x04; // EPOLLOUT
                    if (ready_events & sys_state->epoll_stdout_events) {
                        events[ready_count].events = ready_events & sys_state->epoll_stdout_events;
                        events[ready_count].data = sys_state->epoll_stdout_data;
                        ready_count++;
                    }
                }
                if (sys_state->epoll_stderr_registered && sys_state->epoll_stderr_epfd == epfd && maxevents > ready_count && events) {
                    uint32_t ready_events = 0x04; // EPOLLOUT
                    if (ready_events & sys_state->epoll_stderr_events) {
                        events[ready_count].events = ready_events & sys_state->epoll_stderr_events;
                        events[ready_count].data = sys_state->epoll_stderr_data;
                        ready_count++;
                    }
                }

                for (int i = 3; i < FD_MAX; i++) {
                    if (sys_state->fd_table[i].used && sys_state->fd_table[i].epoll_registered && sys_state->fd_table[i].epoll_epfd == epfd) {
                        fd_entry_t *e = (fd_entry_t *)&sys_state->fd_table[i];
                        if (e->file == &sys_state->dev_pipe_file) {
                            if (e->pipe_head != e->pipe_tail && ready_count < maxevents) {
                                epoll_event_abi_t *ev = &events[ready_count];
                                ev->events = 1; // EPOLLIN
                                ev->data = e->epoll_data;
                                ready_count++;
                            }
                        }
                    }
                }

                // Check timerfd
                if (sys_state->timerfd_active && sys_state->timerfd_epoll_registered && ready_count < maxevents) {
                    int64_t sec;
                    long nsec;
                    // Bump fake time to simulate time passing while waiting!
                    sys_state->fake_timer_nsec += 1000000; // 1 ms
                    get_fake_time(&sec, &nsec);
                    uint64_t now_ns = (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
                    if (now_ns >= sys_state->timerfd_expire_nsec) {
                        epoll_event_abi_t *ev = &events[ready_count];
                        ev->events = 1; // EPOLLIN
                        ev->data = sys_state->timerfd_epoll_data;
                        ready_count++;
                    }
                }

                if (ready_count > 0) {
                    sys_lock();
                    return ready_count;
                }

                if (timeout == 0) {
                    sys_lock();
                    return 0; // Non-blocking poll returns 0 ready events
                }
                if (timeout > 0) {
                    if (yields++ > timeout * 10) {
                        sys_lock();
                        return 0;
                    }
                }
                seL4_Yield();
            }
        }

        case SYS_pselect6: {
            int nfds = (int)a1;
            uint8_t *readfds = (uint8_t *)a2;
            uint8_t *writefds = (uint8_t *)a3;
            const timespec_abi_t *timeout_ts = (const timespec_abi_t *)a4;

            sys_unlock(); // Release global spinlock while waiting for I/O

            int yields = 0;
            for (;;) {
                int ready = 0;
                int has_data = console_ring && (console_ring->rx_tail != console_ring->rx_head);
                if (nfds > 0) {
                    for (int i = 0; i < nfds; i++) {
                        int byte_idx = i / 8;
                        int bit_mask = 1 << (i % 8);
                        int r_ready = 0, w_ready = 0;

                        if (readfds && (readfds[byte_idx] & bit_mask)) {
                            if (i == 0 && has_data) r_ready = 1;
                            else readfds[byte_idx] &= ~bit_mask;
                        }
                        if (writefds && (writefds[byte_idx] & bit_mask)) {
                            if ((i == 1 || i == 2) && (has_data || yields > 0)) w_ready = 1;
                            else writefds[byte_idx] &= ~bit_mask;
                        }

                        if (r_ready || w_ready) ready++;
                    }
                }
                
                if (ready > 0) {
                    sys_lock();
                    return ready;
                }

                if (timeout_ts) {
                    if (timeout_ts->tv_sec == 0 && timeout_ts->tv_nsec == 0) {
                        sys_lock();
                        return 0;
                    }
                    if (yields++ > 100) {
                        sys_lock();
                        return 0;
                    }
                }

                sys_state->fake_timer_nsec += 1000000; // 1 ms
                seL4_Yield();
            }
        }

        case SYS_eventfd2: {
            int flags = (int)a2;
            int fd = fd_alloc((const vfs_file_t *)&sys_state->dev_pipe_file, 0);
            if (fd >= 0) sys_state->fd_table[fd].nonblock = (flags & 04000) ? 1 : 0;
            return fd;
        }

        case SYS_poll: { // 73: SYS_ppoll on AArch64
            struct pollfd_abi_t { int fd; short events; short revents; } *fds = (void *)a1;
            size_t nfds = (size_t)a2;
            const timespec_abi_t *tmo_p = (void *)a3;
            
            sys_unlock();
            
            int yields = 0;
            for (;;) {
                int ready = 0;
                if (fds && nfds > 0) {
                    int has_data = console_ring && (console_ring->rx_tail != console_ring->rx_head);
                    for (size_t i = 0; i < nfds; i++) {
                        fds[i].revents = 0;
                        int fd = fds[i].fd;
                        if (fd < 0) continue;
                        
                        if (fd == 0) {
                            if (has_data && (fds[i].events & 0x01)) fds[i].revents |= 0x01; // POLLIN
                        } else if (fd == 1 || fd == 2) {
                            if (fds[i].events & 0x04) fds[i].revents |= 0x04; // POLLOUT
                        } else if (fd >= 3 && fd < FD_MAX && sys_state->fd_table[fd].used) {
                            fd_entry_t *e = (fd_entry_t *)&sys_state->fd_table[fd];
                            if (e->file == &sys_state->dev_pipe_file) {
                                if (e->pipe_read_fd >= 0) {
                                    if (fds[i].events & 0x04) fds[i].revents |= 0x04;
                                } else {
                                    if (e->pipe_head != e->pipe_tail) {
                                        if (fds[i].events & 0x01) fds[i].revents |= 0x01;
                                    }
                                }
                            } else {
                                if (fds[i].events & 0x01) fds[i].revents |= 0x01;
                                if (fds[i].events & 0x04) fds[i].revents |= 0x04;
                            }
                        } else {
                            fds[i].revents |= 0x20; // POLLNVAL
                        }
                        
                        if (fds[i].revents) ready++;
                    }
                }
                
                if (ready > 0) {
                    sys_lock();
                    return ready;
                }
                
                if (tmo_p) {
                    if (tmo_p->tv_sec == 0 && tmo_p->tv_nsec == 0) {
                        sys_lock();
                        return 0;
                    }
                    if (yields++ > 100000) {
                        sys_lock();
                        return 0;
                    }
                }
                
                if (sys_state->pending_notify != 0 && microkit_name[0] == 's' && microkit_name[1] == 'y' && microkit_name[2] == 'n') {
                    uint32_t target_ch = sys_state->pending_notify;
                    sys_state->pending_notify = 0;
                    microkit_notify(target_ch);
                }
                
                sys_state->fake_timer_nsec += 1000000; // 1 ms
                seL4_Yield();
            }
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
            if ((flags & 0x11) == 0x11 || flags == 0x11 || flags == 0x1200011) { // fork() or vfork()
                static int fake_pid = 2000;
                return fake_pid++;
            }

            // In AArch64 Musl __clone, func and arg are saved on the child stack
            uint64_t func = child_stack[0];
            uint64_t arg = child_stack[1];

            int slot = sys_state->next_thread_slot;
            int mapped_slot = slot % 8;
            sys_state->next_thread_slot++;
            
            // Musl allocated the TCB, mapped the stack, and copied TLS! We just pass the pointers!
            mailbox[mapped_slot].tls = tls;
            mailbox[mapped_slot].child_stack = (void *)child_stack;
            mailbox[mapped_slot].start_routine = (void *(*)(void *))func;
            mailbox[mapped_slot].arg = (void *)arg;
            mailbox[mapped_slot].active = 1;
            
            long tid = slot + 1000;
            if (flags & 0x00080000) { // CLONE_CHILD_SETTID
                if (ctid) *(int *)ctid = tid;
            }
            if (flags & 0x00000100) { // CLONE_PARENT_SETTID
                if (ptid) *(int *)ptid = tid;
            }

            if (microkit_name[0] == 's' && microkit_name[1] == 'y' && microkit_name[2] == 'n') {
                microkit_notify(mapped_slot + 3); // Channels 3 to 10 map to slots 0 to 7 on synrc
            } else {
                sys_state->pending_notify = mapped_slot + 3;
                microkit_notify(1); // Worker Channel 1 notifies synrc
            }

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
        return do_syscall(sysno, a1, a2, a3, a4, a5, a6);
    }
    sys_lock_dbg(sysno);
    long ret = do_syscall(sysno, a1, a2, a3, a4, a5, a6);
    sys_unlock();
    return ret;
}
