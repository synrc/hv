#include <microkit.h>
#include <stdint.h>
#include "worker_syscall.h"
#include "worker_run.h"

#define SLOT_ID 14


void init(void) {
    *(uint64_t *)0x08000000 = (uint64_t)&tyn_syscall_entry;
    microkit_dbg_puts("[beam_worker14] Initialized and waiting for work on core 1...\n");
}

void notified(microkit_channel ch) {
    if (ch == 1) { 
        struct mailbox_slot *slot = &mailbox[SLOT_ID];
        if (slot->active && slot->start_routine) {
            microkit_dbg_puts("[beam_worker14] Waking up and starting thread routine...\n");
            
            slot->retval = run_on_stack(slot->child_stack, slot->tls, slot->arg, slot->start_routine);
            slot->active = 0; 
            microkit_dbg_puts("[beam_worker14] Thread routine finished.\n");
        }
    }
}
