#include <microkit.h>
#include "syscall_trap.h"
#include "vfs.h"
#include "beam_loader.h"

void init(void) {
    microkit_dbg_puts("[tyn] OS.1 Tyn Protection Domain active\n");
    tyn_syscall_init();
    vfs_init();
    
    beam_loader_start();
}

void notified(microkit_channel ch) {
    (void)ch;
}
