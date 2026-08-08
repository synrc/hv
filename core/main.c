#include <stdint.h>

#define SYS_WRITEC   0x03   // Write single character
#define SYS_WRITE0   0x04   // Write null-terminated string
#define SYS_EXIT     0x18   // Exit emulator

static inline void semi_call(unsigned int op, const void *param)
{
    register unsigned int w0 __asm__("w0") = op;
    register const void *x1 __asm__("x1") = param;

    __asm__ volatile (
        "hlt #0xf000"
        : "+r" (w0)
        : "r" (x1)
        : "memory"
    );
}

static void semi_puts(const char *s)
{
    semi_call(SYS_WRITE0, s);
}

void hv_main(void)
{
    semi_puts("synrc/hv booted successfully at EL2\n");

    for (;;) {
        __asm__ volatile("wfe");
    }
}
