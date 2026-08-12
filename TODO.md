Detailed plan: Porting the OTP 29 prim_tty mechanism to the seL4 proxy
1. Architecture of the modern terminal stack (OTP 26+)
textuser_drv (gen_statem)
    │
    ├── prim_tty:init/1          ← creates TTY state
    │       │
    │       ├── NIF: isatty/1
    │       ├── NIF: tty_create/1
    │       ├── NIF: tty_init/2
    │       ├── NIF: tty_select/2, read_nif/3, write_nif/2
    │       ├── NIF: tty_window_size/1, tty_encoding/1
    │       └── terminfo NIFs (tigetstr, tigetnum, …)
    │
    ├── reader process (prim_tty:reader/1)
    └── writer process (prim_tty:writer/1)
The critical path that currently fails on seL4 is:
erlangIsTTY = prim_tty:isatty(stdin) =:= true andalso prim_tty:isatty(stdout) =:= true
If this is false, user_drv raises enotsup and the shell never appears.
2. High-level strategy for seL4
Because the seL4 environment has no real kernel TTY, we have two realistic options:

OptionDescriptionProsConsA. Full NIF emulationImplement a complete (or sufficiently complete) prim_tty NIF that talks to the existing console ringClosest to upstream behaviourHigh effort, need to re-implement termios + select + encodingB. Hybrid / “dumb terminal” pathMake isatty return true + provide a minimal working TTY that satisfies prim_tty:init and then fall back to a simplified reader/writer that uses the existing SYS_read/SYS_write + epollMuch faster, re-uses most of the current proxySome advanced features (raw mode, full terminfo, SIGWINCH) will be limited
Recommended starting point: Option B, then gradually move toward A as needed.
3. Detailed attack plan (phased)
Phase 0 – Diagnosis & observability (1–2 days)

Instrument the proxy so every ioctl, read, write, fcntl on fd 0/1/2 is logged with arguments and return value.
Force TERM=dumb (or xterm) in the environment the BEAM sees.
Capture the exact sequence of NIFs that prim_tty:init calls when it fails.
Confirm whether the failure is:
isatty returning false, or
later inside tty_init / terminfo setup, or
the reader/writer processes crashing on registration (known race that was fixed upstream).


Phase 1 – Make isatty succeed (highest priority)
prim_tty:isatty(Fd) ultimately does something equivalent to:
Cioctl(fd, TCGETS, &termios) == 0   // or platform equivalent
Actions in the seL4 syscall trap:

Guarantee that TCGETS (0x5401) always succeeds for fd 0, 1 and 2 and returns a plausible struct termios.
Also answer:
TIOCGWINSZ (0x5413) – already present
TCSETS / TCSETSW / TCSETSF (accept and ignore, or store a soft copy)
FIONREAD / TIOCINQ
any other ioctls that appear in the log from Phase 0

Optionally implement a tiny in-memory termios state per stdio fd so successive TCGETS/TCSETS are consistent.

Once isatty(0) and isatty(1) return true, user_drv will proceed past the first guard.
Phase 2 – Satisfy prim_tty:init / tty_create + tty_init
The NIF creates an opaque TTY resource and puts the terminal into the requested mode (raw or cooked).
Minimal viable implementation:

In the NIF layer (or by intercepting the calls if you recompile OTP with a custom NIF):
tty_create → return a dummy resource handle that the rest of the code can use.
tty_init(TTY, Options) → succeed and remember the mode (raw/cooked).

On the proxy side keep the existing console-ring + epoll machinery; the NIF will later call read_nif / write_nif which can be mapped onto the same ring.

If you cannot easily replace the NIF binary, the alternative is to patch prim_tty.erl (or supply a replacement module) that short-circuits the NIF calls for the seL4 environment and uses a pure-Erlang “fake TTY” that still speaks the same message protocol that user_drv expects.
Phase 3 – Reader & Writer processes
prim_tty spawns two linked processes:

Reader – does tty_select + read_nif in a loop and sends data to the parent.
Writer – receives write requests and calls write_nif.

Required work:

Make sure these processes can register themselves (the recent race around process_info(..., registered_name) must not crash).
Map read_nif → your existing console_ring_read_rx (or a blocking wait that yields via seL4_Yield).
Map write_nif → microkit_dbg_putc / console ring TX + notify.
Support both blocking and the “select” style that tty_select uses (the current epoll infrastructure is already a good match).

Phase 4 – terminfo / geometry / encoding

tty_window_size → return the hard-coded 24×80 you already have (or make it configurable).
tigetstr / tigetnum / tigetflag – for TERM=dumb most of these can return empty/0. For richer terminals you will eventually need a tiny terminfo database or a hard-coded table for the capabilities prim_tty actually uses (cursor movement, clear, etc.).
Unicode / encoding path – start with latin1 or UTF-8 passthrough; the existing SYS_write already just dumps bytes.

Phase 5 – Signals & advanced features (optional)

SIGWINCH / SIGCONT are currently handled via erl_signal_server + prim_tty_sighandler. On seL4 these can be stubbed or turned into artificial messages if you ever need resize support.
Raw mode (needed for shell:start_interactive({noshell, raw}) and modern line editing) requires the reader to deliver keystrokes as soon as they arrive, not only on newline. Your current non-blocking console ring already supports this.

Phase 6 – Integration & fallback

Keep the old OTP-20 style path as a compile-time or runtime fallback (-oldshell or a custom user module).
Add a seL4-specific boot flag or environment variable that selects the “minimal prim_tty” implementation.
Once the shell appears, verify:
basic input/output
Ctrl-G job control
io:get_line, io:put_chars
Unicode (if required)
that the global spinlock / futex path still works under concurrent I/O from multiple schedulers.


4. Concrete implementation sketch (proxy side)
C// In SYS_ioctl for fd 0/1/2
case 0x5401: // TCGETS
    // fill a real-looking termios
    // important flags: ICANON, ECHO, ISIG, etc. depending on mode
    return 0;

case 0x5402: case 0x5403: case 0x5404: // TCSETS*
    // accept and optionally store
    return 0;

case 0x5413: // TIOCGWINSZ
    // already present
    return 0;
Plus a small table that remembers whether the terminal is currently in raw or cooked mode so that subsequent reads behave correctly.
5. Risk & effort estimate

PhaseEffortRiskImpact0 – DiagnosisLowLowHigh clarity1 – isattyLow–MediumLowUnblocks shell start2 – tty_create/initMediumMediumRequired for init3 – Reader/WriterMedium–HighMediumActual I/O4 – terminfo/geometryLow–MediumLowPolish5 – Signals/rawMediumLowNice-to-have
6. Recommended immediate next actions

Add exhaustive logging of every stdio-related syscall and the exact return values.
Force TERM=dumb and confirm whether prim_tty:isatty is the first failure.
Harden the existing TCGETS / TIOCGWINSZ handlers so they are 100 % consistent.
Decide whether you will:
replace/patch the prim_tty NIF, or
supply a pure-Erlang shim that implements the same API that user_drv expects.

Once Phase 1 succeeds you will see user_drv and the reader/writer processes appear; the rest of the work is then incremental.
This plan keeps the existing console-ring + epoll infrastructure you already have while systematically satisfying every requirement the modern prim_tty path imposes.

