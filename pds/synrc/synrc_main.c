#include <microkit.h>
#include "syscall_trap.h"
#include "vfs.h"
#include "beam_loader.h"

extern void tyn_syscall_entry(void);

void init(void) {
    *(uint64_t *)0x08000000 = (uint64_t)&tyn_syscall_entry;
    microkit_dbg_puts("[synrc] OS.1 Synrc Protection Domain active.\n");
    tyn_syscall_init();
    vfs_init();
    beam_loader_start();
}

void notified(microkit_channel ch) {
    (void)ch;
}
