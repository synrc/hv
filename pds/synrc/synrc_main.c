#include <stdbool.h>
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
    uint32_t target_ch = sys_state->pending_notify;
    if (target_ch != 0) {
        sys_state->pending_notify = 0;
        microkit_notify(target_ch);
    }
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo) {
    (void)reply_msginfo;
    (void)msginfo;
    uint64_t ip = seL4_GetMR(0);
    uint64_t fault_addr = seL4_GetMR(1);
    uint64_t is_instruction = seL4_GetMR(2);
    uint64_t fsr = seL4_GetMR(3);

    microkit_dbg_puts("[synrc] FAULT INTERCEPTED: child=");
    puthex64(child);
    microkit_dbg_puts(" ip=");
    puthex64(ip);
    microkit_dbg_puts(" fault_addr=");
    puthex64(fault_addr);
    microkit_dbg_puts(" fsr=");
    puthex64(fsr);
    microkit_dbg_puts(" is_insn=");
    puthex64(is_instruction);
    microkit_dbg_puts("\n");

    return false; // Do not resume the faulting child thread
}

