# QEMU Virt aarch64 Board Configuration

BOARD_NAME     = qemu_virt_aarch64
CPU            = cortex-a53
ARCH           = aarch64
RAM_BASE       = 0x40000000
RAM_SIZE       = 0x40000000 # 1 GB
UART_BASE      = 0x09000000 # PL011 UART
GIC_DIST_BASE  = 0x08000000 # GICv2 Distributor
GIC_CPU_BASE   = 0x08010000 # GICv2 CPU interface

CFLAGS_BOARD   = -DQEMU_VIRT_AARCH64 -mcpu=$(CPU)
