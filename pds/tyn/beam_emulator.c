#include "beam_emulator.h"
#include <microkit.h>
#include "syscall_trap.h"
#include "vfs.h"

// Forward declaration of real BEAM entrance point from static-musl beam.smp
extern int beam_smp_main(int argc, char **argv);

int beam_main(int argc, char **argv) {
    (void)argc; (void)argv;
    microkit_dbg_puts("[beam.smp] Initializing Erlang/OTP emulator fallback...\n");
#if defined(__aarch64__)
    return 0;
#else
    extern int beam_smp_main(int argc, char **argv);
    return beam_smp_main(argc, argv);
#endif
}

void beam_process_input(void) {
    // Process input streams directly through musl SYS_read / SYS_write syscall traps
    char stream_buf[128];
    long nread = tyn_syscall_dispatch(SYS_read, 0, (long)stream_buf, sizeof(stream_buf) - 1, 0, 0, 0);
    if (nread > 0) {
        stream_buf[nread] = '\0';
        // Pass stream to real BEAM process standard input
    }
}
