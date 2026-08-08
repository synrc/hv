.syntax unified
.arch armv7-a
.arch_extension virt

.section .text.boot, "ax"
.global _start
_start:
    /* Disable interrupts */
    cpsid   if

    /* We assume we are already in Hyp mode (or a previous stage put us here).
       If not, you would switch from Secure SVC → Monitor → Hyp here. */

    /* Set Hyp vector table */
    ldr     r0, =hyp_vectors
    mcr     p15, 4, r0, c12, c0, 0      /* HVBAR */

    /* Set up Hyp stack */
    ldr     sp, =__stack_top

    /* Clear BSS */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlt   r2, [r0], #4
    blt     1b

    /* Jump to C */
    bl      hyp_main

    /* Should never return */
1:  wfi
    b       1b
