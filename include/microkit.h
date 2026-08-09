#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint32_t microkit_channel;
typedef uint32_t microkit_msginfo;

void init(void);
void notified(microkit_channel ch);

static inline void microkit_notify(microkit_channel ch) {
    (void)ch;
}

// Semihosting output utility for QEMU bare-metal / microkit console
static inline void sys_puts(const char *s) {
    register unsigned int w0 __asm__("w0") = 0x04; // SYS_WRITE0
    register const void *x1 __asm__("x1") = s;
    __asm__ volatile ("hlt #0xf000" : "+r" (w0) : "r" (x1) : "memory");
}
