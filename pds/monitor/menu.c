#include <unistd.h>
#include <termios.h>

// Розміри меню
#define MAX_ITEMS 17
#define CMDLINE_MAX 256

// ANSI Ескейп-послідовності для TERMIOS
#define ANSI_CLEAR "\033[2J\033[H"
#define ANSI_CURSOR_HIDE "\033[?25l"
#define ANSI_CURSOR_SHOW "\033[?25h"
#define ANSI_RESET "\033[0m"
#define ANSI_CYAN "\033[36m"
#define ANSI_SELECT "\033[1;37;46m" // Білий на ціановому тлі
#define ANSI_YELLOW "\033[1;33m"

// Стани кінцевого автомату для парсингу стрілочок
#define KEY_STATE_NORMAL 0
#define KEY_STATE_ESC 1
#define KEY_STATE_BRACKET 2

typedef struct {
    const char *name;
    const char *cart_type;
    const char *country;          // ← нова колонка
    char cmdline[CMDLINE_MAX];
    int cpu_count;
} boot_entry_t;

/*
   Стандарт VirtIO (що виріс з QEMU) ліг в основу багатьох систем віртуалізації:
   KVM/QEMU, seL4, CrosVM, Firecracker, Cloud Hypervisor, ACRN, Xvisor та ін.
   ...
*/

static boot_entry_t menu_items[MAX_ITEMS] = {
    {"NuttX 12.0",                      "[RTOS Card]",         "USA",          "console=hvc0 root=/dev/vda1 virtio_mmio.device=0x0a000000 smp=1 mem=32M nsh", 1},
    {"FreeRTOS",                        "[RTOS Card]",         "UK",           "console=hvc0 root=viocon0 virtio_mmio.device=0x0a000000 smp=false mem=64M platform=virt", 1},
    {"Erlang/OTP 20.0 (seL4 Microkit)", "[BEAM Card]",         "Sweden",       "net_backend=sddf smp=false node=hv@localhost cookie=monitor sched=priority+edf console=hvc0", 1},
    {"NetBSD 11.0 (smolBSD MICROVM)",   "[UNIX Card]",         "USA",          "com0=0x09000000 root=viocon0 crypto=opencrypto consdev=hvc0 virtio_mmio.device=0x0a000000 smp=1 mem=128M", 2},
    {"Alpine Linux",                    "[Linux Card]",        "Switzerland",  "quiet root=/dev/vda1 console=hvc0 virtio_mmio.device=0x0a000000 earlyprintk=hvc0 smp=2 mem=256M init=/sbin/init", 2},
    {"POSIX Binary Editor",             "[POSIX Card]",        "Ukraine",      "bitness=64 arch=riscv64 rootfs=ramfs console=hvc0 editor=bed mode=vi", 1},
    {"POSIX Text Editor",               "[POSIX Card]",        "Ukraine",      "mode=CUA enc=UTF-8 console=hvc0 term=vt220 color=true mouse=virtio-input", 1},
    {"POSIX Sokhatsky Commander",       "[POSIX Card]",        "Ukraine",      "shell=vsh console=hvc0 root=/ fs=sddf-block mouse=virtio-input", 1},
    {"POSIX Verified Shell",            "[POSIX Card]",        "Ukraine",      "shell=vsh verified=true console=hvc0 capdl=static no_dyn_alloc", 1},
    {"Litte Kernel (LK)",               "[Embedded Card]",     "USA",          "console=hvc0 root=viocon0 virtio_mmio.device=0x0a000000 smp=false mem=64M platform=virt", 1},
    {"NewOS",                           "[Embedded Card]",     "USA",          "console=hvc0 root=viocon0 virtio_mmio.device=0x0a000000 smp=false mem=64M platform=virt", 1},
//    {"Project Oberon",                  "[Retro Card]",        "Switzerland",  "console=hvc0 display=virtio-gpu mem=32M", 1},
    {"MINIX 3",                         "[Retro Card]",        "Netherlands",  "console=hvc0 root=viocon0 mem=64M", 1},
    {"Smalltalk-80",                    "[Retro Card]",        "USA",          "image=SM-80.image console=hvc0 display=virtio-gpu input=virtio-input mem=64M gc=generational", 1},
    {"TIC-80",                          "[Fantasy Cartridge]", "Poland",       "cart=auto console=hvc0 display=virtio-gpu audio=virtio-snd mem=16M", 1},
    {"TRON T-Kernel",                   "[Japan Card]",        "Japan",        "arch=arm32 console=hvc0 root=ramfs tick=1ms max_tasks=64", 1},
    {"NEC PC-98 Linux 7.1 i386",        "[Japan Card]",        "Japan",        "arch=i386 machine=pc98 console=ttyS0 root=/dev/hda1 mem=16M noapic nolapic", 1},
    {"NEC V30 os8088",                  "[Japan Card]",        "Japan",        "arch=i8186 machine=nec-v30 console=ttyS0 mem=640K bios=rom", 1}
};

static int current_selection = 0;
static int edit_mode = 0;
static int input_state = KEY_STATE_NORMAL;

// Структури для збереження стану термінала macOS
static struct termios orig_termios;

// Чиста реалізація strlen
static int local_strlen(const char *str) {
    int len = 0;
    while (str[len] != '\0') len++;
    return len;
}

// Пряме виведення рядка на екран через POSIX write
static void print_str(const char *str) {
    write(STDOUT_FILENO, str, local_strlen(str));
}

// Виведення одного символу чи числа
static void print_int(int num) {
    char c = '0' + num;
    write(STDOUT_FILENO, &c, 1);
}

// Малювання всього інтерфейсу системного монітора
static void draw_interface(void) {
    print_str(ANSI_CLEAR);
    print_str(ANSI_CURSOR_HIDE);
    print_str("\n");

    // 1. ASCII Логотип
    print_str(ANSI_CYAN);
    print_str("    OS.1 [ARM64][cpu:8]        ___           \n");
    print_str("   ___ _   _ _ __  _ __ ___   / / |____   __ \n");
    print_str("  / __| | | | '_ \\| '__/ __| / /| '_ \\ \\ / / \n");
    print_str("  \\__ \\ |_| | | | | | | (__ / / | | | \\ V /  \n");
    print_str("  |___/\\__, |_| |_|_|  \\___/_/  |_| |_|\\_/   \n");
    print_str("       |___/       (synrc/hv monitor)           \n\n");
    print_str(ANSI_RESET);

    // 2. Рамка вибору систем (з колонками Type + Country)
    print_str(ANSI_CYAN);
    print_str(" ┌────────────────────────────────────────────────────────┬──────────────────────┬─────────────────┐\n");
    for (int i = 0; i < MAX_ITEMS; i++) {
        print_str(" │ ");
        if (i == current_selection) {
            print_str(ANSI_SELECT);
            print_str("--> ");
            print_str(menu_items[i].name);
            int spaces = 50 - local_strlen(menu_items[i].name);
            for (int j = 0; j < spaces; j++) print_str(" ");
            print_str(" │ ");
            print_str(menu_items[i].cart_type);
            spaces = 20 - local_strlen(menu_items[i].cart_type);
            for (int j = 0; j < spaces; j++) print_str(" ");
            print_str(" │ ");
            print_str(menu_items[i].country);
            spaces = 15 - local_strlen(menu_items[i].country);
            for (int j = 0; j < spaces; j++) print_str(" ");
            print_str(ANSI_RESET);
        } else {
            print_str("    ");
            print_str(menu_items[i].name);
            int spaces = 50 - local_strlen(menu_items[i].name);
            for (int j = 0; j < spaces; j++) print_str(" ");
            print_str(" │ ");
            print_str(menu_items[i].cart_type);
            spaces = 20 - local_strlen(menu_items[i].cart_type);
            for (int j = 0; j < spaces; j++) print_str(" ");
            print_str(" │ ");
            print_str(menu_items[i].country);
            spaces = 15 - local_strlen(menu_items[i].country);
            for (int j = 0; j < spaces; j++) print_str(" ");
        }
        print_str(ANSI_CYAN);
        print_str(" │\n");
    }
    print_str(" └────────────────────────────────────────────────────────┴──────────────────────┴─────────────────┘\n");
    print_str(ANSI_RESET);

    // 3. Секція Boot Arguments
    print_str("\n Boot Arguments (Press [E] to edit, [Enter] to Select/Boot, [Q] to Exit):\n");
    print_str(" > ");
    if (edit_mode) {
        print_str(ANSI_YELLOW);
    }
    print_str(menu_items[current_selection].cmdline);
    print_str(ANSI_RESET);
    if (edit_mode) {
        print_str("█");
    }
    print_str("\n");

    // 4. Налаштування ядер
    print_str(" Allocated Cores: ");
    print_int(menu_items[current_selection].cpu_count);
    print_str(" (Press [+] / [-] to change affinity)\n");
    print_str("\n Use [↑/↓] arrows or [W/S] keys to navigate. [TERMIOS][CUA].\n");
}

// Перемикання термінала macOS в raw mode
void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Повернення термінала в початковий стан
void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    print_str(ANSI_CURSOR_SHOW);
}

int main(void) {
    enable_raw_mode();
    draw_interface();

    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        // Режим редагування рядка
        if (edit_mode) {
            int len = local_strlen(menu_items[current_selection].cmdline);
            if (c == 13 || c == 10) {
                edit_mode = 0;
                draw_interface();
            } else if (c == 127 || c == 8) {
                if (len > 0) {
                    menu_items[current_selection].cmdline[len - 1] = '\0';
                    draw_interface();
                }
            } else if (len < (CMDLINE_MAX - 1) && c >= 32 && c <= 126) {
                menu_items[current_selection].cmdline[len] = c;
                menu_items[current_selection].cmdline[len + 1] = '\0';
                draw_interface();
            }
            continue;
        }

        // Парсинг стрілочок
        if (input_state == KEY_STATE_ESC) {
            if (c == '[') input_state = KEY_STATE_BRACKET;
            else input_state = KEY_STATE_NORMAL;
            continue;
        }
        if (input_state == KEY_STATE_BRACKET) {
            input_state = KEY_STATE_NORMAL;
            if (c == 'A') {
                if (current_selection > 0) { current_selection--; draw_interface(); }
            } else if (c == 'B') {
                if (current_selection < MAX_ITEMS - 1) { current_selection++; draw_interface(); }
            }
            continue;
        }

        // Керування меню
        if (c == 27) {
            input_state = KEY_STATE_ESC;
            continue;
        }
        if (c == 'q' || c == 'Q') break;
        if (c == 'e' || c == 'E') {
            edit_mode = 1;
            draw_interface();
            continue;
        }
        if (c == 'w' || c == 'W') {
            if (current_selection > 0) { current_selection--; draw_interface(); }
        }
        if (c == 's' || c == 'S') {
            if (current_selection < MAX_ITEMS - 1) { current_selection++; draw_interface(); }
        }
        if (c == '+') {
            if (menu_items[current_selection].cpu_count < 8) {
                menu_items[current_selection].cpu_count++;
                draw_interface();
            }
        }
        if (c == '-') {
            if (menu_items[current_selection].cpu_count > 1) {
                menu_items[current_selection].cpu_count--;
                draw_interface();
            }
        }
        if (c == 13 || c == 10) {
            print_str(ANSI_CLEAR);
            disable_raw_mode();
            print_str("\n\033[1;32m[Handoff] Booting image...\033[0m\n");
            print_str("Kernel parameters transferred to seL4 shared memory block.\n");
            return 0;
        }
    }

    disable_raw_mode();
    print_str(ANSI_CLEAR);
    return 0;
}
