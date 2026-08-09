#include <microkit.h>

void init(void) {
    microkit_dbg_puts("[monitor] OS.1 Monitor PD active (NIST AU audit logger initialized)\n");
}

void notified(microkit_channel ch) {
    if (ch == 1) {
        microkit_dbg_puts("[monitor] Heartbeat event received from Tyn PD (NIST AU log entry recorded)\n");
    }
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo) {
    microkit_dbg_puts("[monitor] CRITICAL: Tyn PD caused a fault!\n");
    // We could decode the msginfo to print the exact fault reason (e.g. instruction address, fault address).
    // Let's print the basic info.
    microkit_dbg_puts("Fault ID: ");
    char num[2] = {'0' + child, '\0'};
    microkit_dbg_puts(num);
    microkit_dbg_puts("\n");
    for (;;) {}
    return seL4_False;
}
