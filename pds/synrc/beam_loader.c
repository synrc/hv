#include "beam_loader.h"
#include "beam_emulator.h"
#include <microkit.h>
#include "vfs.h"

// Physical/virtual base for BEAM ELF image (mapped in tyn-beam.system)
#define BEAM_ELF_BASE  ((const uint8_t *)0x50000000UL)
#define BEAM_ELF_LIMIT (0x800000UL)  // 8 MB — OTP 29 text segment is ~6 MB

// Stack for BEAM process (above the cpio region — 0x65000000)
#define BEAM_STACK_BASE 0x65000000UL
#define BEAM_STACK_SIZE 0x80000UL    // 512 KB

// -------------------------------------------------------------------------
// ELF64 types
// -------------------------------------------------------------------------
#define ELF_MAGIC0 0x7f
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ET_EXEC    2
#define PT_LOAD    1
#define EM_AARCH64 0xB7
#define PF_X       1
#define PF_W       2
#define PF_R       4

extern void tyn_syscall_entry(void);

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_hdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

// -------------------------------------------------------------------------
// argv[] for the real BEAM process (OTP 20 non-SMP flags)
// -------------------------------------------------------------------------
static const char *beam_argv[] = {
    "beam",
    "-S1:1",
    "-SDcpu1:1",
    "-SDio1",
    "-A1",
    "-a8192",
    "-h", "10240",
    "--",
    "-root", "/otp",
    "-progname", "erl",
    "-home", "/",
    "-boot", "/otp/bin/start",
    "-init_debug",
    "-noshell",
    "-eval", "erlang:display(hello_world), init:stop()",
    0
};
static const int beam_argc = (sizeof(beam_argv) / sizeof(beam_argv[0])) - 1;

// -------------------------------------------------------------------------
// envp[] — BEAM's sys.c fatally exits without BINDIR/ROOTDIR/EMU
// -------------------------------------------------------------------------
static char *beam_envp[] = {
    "BINDIR=/otp/bin",
    "ROOTDIR=/otp",
    "PROGNAME=erl",
    "HOME=/",
    "EMU=beam",
    "ERL_CRASH_DUMP=/dev/null",
    "ERL_FLAGS=+MMscs 32 +Musac false",
    0
};

// -------------------------------------------------------------------------
// Freestanding memory helpers (no libc)
// -------------------------------------------------------------------------
static void tyn_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void tyn_memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

// -------------------------------------------------------------------------
// Jump to ELF entry — never returns
// Linux AArch64 ABI expects SP to point to:
//   argc
//   argv[0]...argv[argc-1]
//   NULL
//   envp[0]...envp[n]
//   NULL
//   auxv[0]...auxv[m]
//   AT_NULL
// -------------------------------------------------------------------------
__attribute__((noreturn))
static void elf_enter(uintptr_t entry, uintptr_t sp_base,
                      int argc, char **argv, char **envp) {
    // count envp entries
    int envc = 0;
    if (envp) { while (envp[envc]) envc++; }

    int total_words = 1 + (argc + 1) + (envc + 1) + 6; // argc, argv[], NULL, envp[], NULL, auxv

    // Put random_bytes at the very top
    uintptr_t random_ptr = (sp_base - 16) & ~(uintptr_t)15;
    uint8_t *random_bytes = (uint8_t *)random_ptr;
    for (int i = 0; i < 16; i++) random_bytes[i] = (uint8_t)(i ^ 0xA5);

    // Put stack frame below random_bytes
    uintptr_t aligned_sp = (random_ptr - ((size_t)total_words * 8)) & ~(uintptr_t)15;
    uintptr_t *p = (uintptr_t *)aligned_sp;

    // 1. argc
    *p++ = (uintptr_t)argc;
    // 2. argv[]
    microkit_dbg_puts("[synrc] BEAM ARGS:\n");
    for (int i = 0; i < argc; i++) {
        microkit_dbg_puts("  ");
        microkit_dbg_puts(argv[i]);
        microkit_dbg_puts("\n");
        *p++ = (uintptr_t)argv[i];
    }
    *p++ = 0; // NULL terminator for argv
    // 3. envp[]
    for (int i = 0; i < envc; i++) *p++ = (uintptr_t)envp[i];
    *p++ = 0; // NULL terminator for envp
    // 4. auxv
    *p++ = 25; // AT_RANDOM
    *p++ = random_ptr;
    *p++ = 6;  // AT_PAGESZ
    *p++ = 4096;
    *p++ = 0;  // AT_NULL
    *p++ = 0;

    register uintptr_t  r_entry __asm__("x9") = entry;
    __asm__ volatile(
        "mov sp, %[stack]\n"
        "mov x0, #0\n" // Linux entry expects registers x0-x2 to be 0 or cleanup func
        "br  %[entry]\n"
        :
        : [stack] "r"(aligned_sp), [entry] "r"(r_entry)
        : "memory"
    );
    __builtin_unreachable();
}

// -------------------------------------------------------------------------
// ELF loader + launch
// -------------------------------------------------------------------------
void beam_loader_start(void) {
    microkit_dbg_puts("[synrc] Transferring execution to real BEAM executable (beam.smp.elf)...\n");

    const uint8_t *elf = BEAM_ELF_BASE;

    // Check ELF magic — if absent QEMU -device loader was not used
    if (elf[0] != ELF_MAGIC0 || elf[1] != ELF_MAGIC1 ||
        elf[2] != ELF_MAGIC2 || elf[3] != ELF_MAGIC3) {
        microkit_dbg_puts("[synrc] ELF loader: no AArch64 ELF at 0x50000000\n");
        microkit_dbg_puts("[synrc] Run with: make run (uses -device loader for beam.aarch64.elf)\n");
        beam_main(beam_argc, beam_argv);
        return;
    }

    const elf64_hdr_t *hdr = (const elf64_hdr_t *)elf;

    if (hdr->e_type != ET_EXEC || hdr->e_machine != EM_AARCH64) {
        microkit_dbg_puts("[synrc] ELF loader: not an AArch64 ET_EXEC — wrong binary?\n");
        return;
    }

    microkit_dbg_puts("[synrc] ELF loader: mapping PT_LOAD segments...\n");

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)
            (elf + hdr->e_phoff + (uint64_t)i * hdr->e_phentsize);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        uint8_t *dst = (uint8_t *)(uintptr_t)ph->p_vaddr;
        tyn_memset(dst, 0, (size_t)ph->p_memsz);
        if (ph->p_filesz > 0) {
            tyn_memcpy(dst, elf + ph->p_offset, (size_t)ph->p_filesz);
        }

        // Check instruction at 0x59a120 specifically
        if (ph->p_vaddr == 0x400000) {
            uint32_t *probe = (uint32_t *)0x59a120;
            if (*probe == 0xd4000001) {
                microkit_dbg_puts("[synrc] Probe at 0x59a120 IS 0xd4000001!\n");
            } else {
                microkit_dbg_puts("[synrc] Probe at 0x59a120 IS NOT 0xd4000001!\n");
            }
        }

        // Patch `svc #0` (0xd4000001) with `bl tyn_syscall_entry` in executable segments
        if (ph->p_flags & PF_X) {
            uint32_t *trampoline_base = (uint32_t *)0x6F0000;
            int trampoline_index = 0;

            int patch_count = 0;
            
            // It's an executable segment. Scan for svc #0 (0xd4000001)
            for (uintptr_t addr = ph->p_vaddr; addr < ph->p_vaddr + ph->p_filesz - 3; addr += 4) {
                uint32_t *inst = (uint32_t *)addr;
                if (*inst == 0xd4000001) {
                    uint32_t *t = &trampoline_base[trampoline_index * 6];
                    
                    // 1. stp x30, x18, [sp, #-16]!
                    t[0] = 0xa9bf4bfe;
                    
                    // 2. mov x16, #0x08000000 (encoded as movz x16, #0x0800, lsl #16)
                    t[1] = 0xd2a10010;
                    
                    // 3. ldr x16, [x16]
                    t[2] = 0xf9400210;
                    
                    // 4. blr x16
                    t[3] = 0xd63f0200;
                    
                    // 5. ldp x30, x18, [sp], #16
                    t[4] = 0xa8c14bfe;
                    
                    // 6. b (addr + 4)
                    int64_t offset = (int64_t)(addr + 4) - (int64_t)&t[5];
                    uint32_t imm26 = (offset >> 2) & 0x03FFFFFF;
                    t[5] = 0x14000000 | imm26;
                    
                    // Replace svc #0 with b to trampoline
                    offset = (int64_t)t - (int64_t)addr;
                    imm26 = (offset >> 2) & 0x03FFFFFF;
                    *inst = 0x14000000 | imm26;

                    trampoline_index++;
                    patch_count++;
                }
            }
            if (patch_count > 0) {
                char msg[64];
                microkit_dbg_puts("[synrc] Patched ");
                // simple itoa
                int n = patch_count;
                int i = 0;
                char num[16];
                if (n == 0) num[i++] = '0';
                while (n > 0) {
                    num[i++] = '0' + (n % 10);
                    n /= 10;
                }
                while (i > 0) {
                    msg[0] = num[i-1];
                    msg[1] = '\0';
                    microkit_dbg_puts(msg);
                    i--;
                }
                microkit_dbg_puts(" svc instructions!\n");
            }
            // Scan for mov x16, x1 (0xaa0103f0) followed by br x16 (0xd61f0200)
            int indirect_patch_count = 0;
            for (uintptr_t addr = ph->p_vaddr; addr < ph->p_vaddr + ph->p_filesz - 7; addr += 4) {
                uint32_t *inst = (uint32_t *)addr;
                if (inst[0] == 0xaa0103f0 && inst[1] == 0xd61f0200) {
                    uint32_t *t = &trampoline_base[trampoline_index * 6];
                    
                    t[0] = 0xb9400030; // ldr w16, [x1]
                    t[1] = 0xb4000050; // cbz w16, #8 (jumps to t[3])
                    t[2] = 0xd61f0020; // br x1
                    t[3] = 0xd65f03c0; // ret
                    
                    int64_t offset = (int64_t)t - (int64_t)inst;
                    uint32_t imm26 = (offset >> 2) & 0x03FFFFFF;
                    inst[0] = 0x14000000 | imm26; // b trampoline
                    inst[1] = 0xd503201f;         // nop
                    
                    trampoline_index++;
                    indirect_patch_count++;
                }
            }
            if (indirect_patch_count > 0) {
                microkit_dbg_puts("[synrc] Patched ");
                int n = indirect_patch_count;
                int i = 0;
                char num[16];
                if (n == 0) num[i++] = '0';
                while (n > 0) {
                    num[i++] = '0' + (n % 10);
                    n /= 10;
                }
                char msg[2] = {0, 0};
                while (i > 0) {
                    msg[0] = num[i-1];
                    microkit_dbg_puts(msg);
                    i--;
                }
                microkit_dbg_puts(" indirect driver call sites!\n");
            }
        }
    }

    uintptr_t entry = (uintptr_t)hdr->e_entry;
    // Stack top aligned to 16 bytes per AArch64 ABI
    uintptr_t sp = (BEAM_STACK_BASE + BEAM_STACK_SIZE) & ~(uintptr_t)15;

    // Flush cache across executable segments
    for (uintptr_t p = 0x00400000; p < 0x00c00000; p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");

    microkit_dbg_puts("[synrc] PD loader: jumping to entry point...\n");
    elf_enter(entry, sp, beam_argc, beam_argv, beam_envp);
}
