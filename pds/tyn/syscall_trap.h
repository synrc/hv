#pragma once

#include <stdint.h>
#include <stddef.h>

// AArch64 Linux syscall numbers (UAPI, same ABI as musl aarch64)
#define SYS_read              63
#define SYS_write             64
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

void tyn_syscall_init(void);
long tyn_syscall_dispatch(long sysno, long a1, long a2, long a3, long a4, long a5, long a6);
