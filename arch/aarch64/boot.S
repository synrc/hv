.section .text.boot
.global _start

_start:
    /* --------------------------------------------------------
     * We expect to be started in EL2 by the Raspberry Pi firmware.
     * -------------------------------------------------------- */

    /* Get current EL */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    1f

    /* If not in EL2 → just hang (should not happen on correctly configured Pi 500) */
    b       .

1:  /* Disable all interrupts */
    msr     daifset, #0xf

    /* Set up EL2 stack */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Set Vector Base Address Register for EL2 */
    ldr     x0, =el2_vectors
    msr     vbar_el2, x0
    isb

    /* Enable EL1 access to physical timer (useful later) */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3          /* EL1PCEN | EL1PCTEN */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    /* Disable co-processor traps to EL2 */
    mov     x0, #0x33ff
    msr     cptr_el2, x0
    msr     hstr_el2, xzr

    /* Clear HCR_EL2 (we will configure it properly later when creating VMs) */
    msr     hcr_el2, xzr

    /* Clear BSS */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    mov     x2, #0
2:  cmp     x0, x1
    b.ge    3f
    str     x2, [x0], #8
    b       2b

3:  /* Jump to C */
    bl      init

    /* Should never return */
4:  wfe
    b       4b
