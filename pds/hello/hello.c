#include <microkit.h>

void init(void) {
    microkit_dbg_puts("Hello from seL4/Microkit on OS.1!\n");
}

void notified(microkit_channel ch) {
    (void)ch;
}
