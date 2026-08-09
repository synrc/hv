#include "beam_emulator.h"
#include <microkit.h>
#include "syscall_trap.h"
#include "vfs.h"

// Forward declaration of real BEAM entrance point from static-musl beam.smp
extern int beam_smp_main(int argc, char **argv);

int beam_main(int argc, char **argv) {
    (void)argc; (void)argv;
    microkit_dbg_puts("[synrc] Initializing Erlang/OTP emulator fallback...\n");
#if defined(__aarch64__)
    return 0;
#else
    return beam_smp_main(argc, argv);
#endif
}