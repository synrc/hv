# synrc/hv

TARGET   = aarch64-none-elf
CC       = clang
AS       = clang
LD       = ld.lld
CFLAGS   = -target $(TARGET) -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -Wall -Wextra -O2
ASFLAGS  = -target $(TARGET)
SRCS_S   = arch/aarch64/boot.S arch/aarch64/vectors.S
SRCS_C   = core/main.c
OBJS     = $(SRCS_S:.S=.o) $(SRCS_C:.c=.o)

.PHONY: all clean
all: kernel8.img
%.o: %.S
	$(AS) $(ASFLAGS) -c $< -o $@
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
kernel8.img: $(OBJS)
	$(LD) -T arch/aarch64/linker.ld --oformat binary -o $@ $(OBJS)
clean:
	rm -f $(OBJS) kernel8.img
