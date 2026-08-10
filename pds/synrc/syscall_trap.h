#pragma once

#include <stdint.h>
#include <stddef.h>

// AArch64 Linux syscall numbers (UAPI, same ABI as musl aarch64)
#define SYS_read              63
#define SYS_write             64
#define SYS_writev            66
#define SYS_close             57
#define SYS_openat            56
#define SYS_fstat             80
#define SYS_lstat             1039 // unique aarch64 stub number
#define SYS_lseek             62
#define SYS_faccessat         48
#define SYS_pipe2             59
#define SYS_fcntl             25
#define SYS_tkill             130
#define SYS_getdents64        61
#define SYS_readlink          78
#define SYS_fstatat           79
#define SYS_brk               214
#define SYS_mmap              222
#define SYS_mprotect          226
#define SYS_munmap            215
#define SYS_clone             220
#define SYS_futex             98
#define SYS_clock_gettime     113
#define SYS_gettimeofday      169
#define SYS_nanosleep         101
#define SYS_getpid            172
#define SYS_getppid           173
#define SYS_gettid            178
#define SYS_set_tid_address   96
#define SYS_prlimit64         261
#define SYS_getrlimit         163
#define SYS_rt_sigprocmask    135
#define SYS_rt_sigaction      134
#define SYS_tgkill            131
#define SYS_ioctl             29
#define SYS_sched_getaffinity 123
#define SYS_uname             160
#define SYS_exit              93
#define SYS_exit_group        94
#define SYS_getrandom         278
// Filesystem extras
#define SYS_getcwd            17
#define SYS_chdir             49
#define SYS_fchdir            50
#define SYS_statfs            43
#define SYS_fstatfs           44
// Socket family
#define SYS_socket            198
#define SYS_socketpair        199
#define SYS_bind              200
#define SYS_listen            201
#define SYS_accept            202
#define SYS_connect           203
#define SYS_getsockname       204
#define SYS_sendto            206
#define SYS_recvfrom          207
#define SYS_setsockopt        208
#define SYS_getsockopt        209
#define SYS_sendmsg           211
#define SYS_recvmsg           212
#define SYS_accept4           242
// Process/signal extras
#define SYS_wait4             260
#define SYS_waitid            95
#define SYS_kill              129
#define SYS_signalfd4         74
#define SYS_epoll_create1     20
#define SYS_epoll_ctl         21
#define SYS_epoll_wait        22
#define SYS_dup               23
#define SYS_dup3              24
#define SYS_epoll_pwait       22  // alias
#define SYS_eventfd2          19
#define SYS_poll              73
#define SYS_ppoll             73  // alias
#define SYS_select            1038
#define SYS_pselect6          72

void tyn_syscall_init(void);
long tyn_syscall_dispatch(long sysno, long a1, long a2, long a3, long a4, long a5, long a6);

void tyn_syscall_init(void);
long tyn_syscall_dispatch(long sysno, long a1, long a2, long a3, long a4, long a5, long a6);
