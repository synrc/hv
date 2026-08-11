static inline void worker_icache_flush(void) {
    uintptr_t start = 0x00400000;
    uintptr_t end   = 0x00c00000;
    for (uintptr_t p = start; p < end; p += 64) {
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy; isb" ::: "memory");
}

__asm__(
".global run_on_stack\n"
"run_on_stack:\n"
"    stp x29, x30, [sp, #-16]!\n"
"    mov x29, sp\n"
"    mov sp, x0\n"
"    msr tpidr_el0, x1\n"
"    dsb ish\n"
"    isb\n"
"    mov x0, x2\n"
"    blr x3\n"
"    mov sp, x29\n"
"    ldp x29, x30, [sp], #16\n"
"    ret\n"
);

extern void *run_on_stack(void *child_stack, uint64_t tls, void *arg, void *(*start_routine)(void *));

