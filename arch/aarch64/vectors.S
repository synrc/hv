.section .text
.align 11                       /* 2KB aligned for VBAR_EL2 */
.global el2_vectors

el2_vectors:
    /* Current EL with SP_EL0 */
    .align 7
    b       el2_sync_invalid        /* Synchronous */
    .align 7
    b       el2_irq_invalid         /* IRQ */
    .align 7
    b       el2_fiq_invalid         /* FIQ */
    .align 7
    b       el2_error_invalid       /* SError */

    /* Current EL with SP_ELx */
    .align 7
    b       el2_sync_invalid
    .align 7
    b       el2_irq_invalid
    .align 7
    b       el2_fiq_invalid
    .align 7
    b       el2_error_invalid

    /* Lower EL using AArch64 */
    .align 7
    b       el2_sync_invalid
    .align 7
    b       el2_irq_invalid
    .align 7
    b       el2_fiq_invalid
    .align 7
    b       el2_error_invalid

    /* Lower EL using AArch32 */
    .align 7
    b       el2_sync_invalid
    .align 7
    b       el2_irq_invalid
    .align 7
    b       el2_fiq_invalid
    .align 7
    b       el2_error_invalid

/* Temporary invalid handlers – just hang */
.macro  inv_handler name
.global \name
\name:
    b       .
.endm

inv_handler el2_sync_invalid
inv_handler el2_irq_invalid
inv_handler el2_fiq_invalid
inv_handler el2_error_invalid
