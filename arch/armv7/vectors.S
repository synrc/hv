.syntax unified
.arch armv7-a
.arch_extension virt

.section .text, "ax"
.align 5
.global hyp_vectors
hyp_vectors:
    /* Hyp mode exception vectors (ARMv7 VE) */
    b       hyp_reset           /* 0x00 Reset */
    b       hyp_undef           /* 0x04 Undefined */
    b       hyp_svc             /* 0x08 HVC / SVC */
    b       hyp_pabort          /* 0x0C Prefetch Abort */
    b       hyp_dabort          /* 0x10 Data Abort */
    b       hyp_trap            /* 0x14 Hyp Trap */
    b       hyp_irq             /* 0x18 IRQ */
    b       hyp_fiq             /* 0x1C FIQ */

/* Minimal stubs – just spin for now */
.macro  vector_stub name
.global \name
\name:
    b       .
.endm

vector_stub hyp_reset
vector_stub hyp_undef
vector_stub hyp_svc
vector_stub hyp_pabort
vector_stub hyp_dabort
vector_stub hyp_trap
vector_stub hyp_irq
vector_stub hyp_fiq
