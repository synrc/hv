The remaining obstacles preventing the interactive Erlang prompt (1> ) from rendering and accepting input fall into three main areas:

1. Interactive STDIN Character Delivery (SYS_read on fd 0)
Current State: SYS_read on fd 0 checks console_ring_read_rx(console_ring, buf, count). When no character is present in the ring buffer, it returns -11 (-EAGAIN).
Obstacle:
In non-interactive QEMU execution (-nographic), serial input is not piped interactively to the console Protection Domain.
ERTS's user_drv reader thread issues a blocking read(0, buf, 1). When SYS_read(0) returns -EAGAIN, user_drv treats STDIN as non-blocking or EOF, preventing the shell process from prompting.
Fix Needed: Update SYS_read(fd=0) to yield/wait (seL4_Yield() or seL4 notification wait on console RX) when the ring buffer is empty, returning data only when a keypress arrives.
2. Terminal Emulation & ioctl Handling (isatty / TCGETS)
Current State: SYS_ioctl handles TCGETS (0x5401) and TIOCGWINSZ (0x5413).
Obstacle: Erlang's user_drv relies on isatty(0) and termios flags (c_lflag, c_iflag, ECHO, ICANON, RAW) to decide whether to launch group_history and shell:start(). If termios flags do not match an interactive terminal, ERTS falls back to batch mode (noshell mode).
Fix Needed: Ensure SYS_ioctl returns full termios flags (c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE) for fd 0.
3. OTP Core BEAM Module Availability in CPIO VFS
Current State: beam_loader.c passes -root /otp -boot /otp/bin/start.
Obstacle: For the REPL shell to launch, ERTS requires the following compiled BEAM modules inside the CPIO VFS image:
/otp/bin/start.boot
/otp/lib/kernel-x.x/ebin/user_drv.beam
/otp/lib/stdlib-x.x/ebin/shell.beam
/otp/lib/stdlib-x.x/ebin/erl_eval.beam
Fix Needed: Confirm these BEAM modules are bundled in cpio so user_drv can spawn shell:start().
Recommended Next Step

Maybe we shoule update SYS_read on fd 0 to block/yield waiting for UART input, and verify isatty termios flags so user_drv launches shell:start()?

Every boot log we've looked at this entire session — the original crash, every hang, every fault — used:

-noshell -eval erlang:display(hello_world), init:stop()

-noshell and -eval ... init:stop() don't just make the shell look non-interactive — they cause ERTS to take a structurally different code path that never touches any of the three things you've listed:

user_drv never starts. -noshell skips spawning the user_drv/user process pair entirely — there's no reader loop blocking on read(0, ...) in the first place, so SYS_read returning -EAGAIN was never a problem; nothing was ever waiting on stdin to begin with.
isatty/termios checks never happen, because they only exist to decide how user_drv should behave (raw line-editing shell vs. dumb fallback) — with user_drv never invoked, that decision logic is dead code for this boot path.
shell.beam/erl_eval (interactively) and group/edlin don't need to be loaded, because -eval runs its one expression through init's own minimal evaluation path and calls init:stop() immediately — no REPL loop is ever entered, no shell module gets started.

So it's not that OTP 20.0 (erts-9.3, matching the readlink stub we saw) or a single-core configuration had some capability the current build lost — it's that the working boot path was carefully chosen specifically because it avoids every piece of interactive-terminal machinery. These three "obstacles" aren't newly discovered latent bugs; they're the complete, expected, and entirely predictable cost of the one change from several messages back — removing -noshell -eval from beam_loader.c. Naming them now is good, accurate scoping work, not new bad news.
