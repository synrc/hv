#include <microkit.h>
#include "syscall_trap.h"
#include "vfs.h"
#include "beam_loader.h"
#include "beam_emulator.h"

void init(void) {
    microkit_dbg_puts("[tyn] OS.1 Tyn Protection Domain active\n");
    tyn_syscall_init();
    vfs_init();
    
    beam_loader_start();
}

void notified(microkit_channel ch) {
    if (ch == 1) {
        // Notification from Console PD that stdin input data is available
        beam_process_input();
    }
}
