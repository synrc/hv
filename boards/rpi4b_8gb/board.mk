# Raspberry Pi 4 (8GB) Board Configuration

BOARD_NAME     = rpi4b_8gb
CPU            = cortex-a72
ARCH           = aarch64
RAM_BASE       = 0x00000000
RAM_SIZE       = 0x200000000 # 8 GB
PERIPHERAL_BASE= 0xfe000000
UART_BASE      = 0xfe215040 # Auxiliary Mini UART / PL011
GIC_DIST_BASE  = 0xff841000
GIC_CPU_BASE   = 0xff842000

CFLAGS_BOARD   = -DRPI4B_8GB -mcpu=$(CPU)
