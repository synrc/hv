#include <microkit.h>
#include "console.h"

#define UART_DR    ((volatile uint32_t *)0x09000000)
#define UART_FR    ((volatile uint32_t *)0x09000018)
#define UART_IMSC  ((volatile uint32_t *)0x09000038)
#define UART_ICR   ((volatile uint32_t *)0x09000044)

#define UART_RXFE  0x10

static volatile console_ring_t *console_ring = (volatile console_ring_t *)0x10000000;

void init(void) {
    microkit_dbg_puts("[console] OS.1 Console Protection Domain active (exclusive UART owner)\n");
    // Enable UART RX and RX timeout interrupts in PL011
    *UART_IMSC = 0x50;
}

void notified(microkit_channel ch) {
    if (ch == 1 && console_ring) {
        // Channel 1: Flush TX ring buffer (Tyn -> UART)
        while (console_ring->tx_tail != console_ring->tx_head) {
            char c = console_ring->tx_buffer[console_ring->tx_tail];
            console_ring->tx_tail = (console_ring->tx_tail + 1) % CONSOLE_BUFFER_SIZE;
            *UART_DR = (uint32_t)c;
        }
    } else if (ch == 2 && console_ring) {
        // Channel 2: Hardware UART RX IRQ (IRQ 33)
        *UART_ICR = 0x7FF; // Clear PL011 UART interrupt flags
        while (!(*UART_FR & UART_RXFE)) {
            char c = (char)(*UART_DR & 0xFF);
            uint32_t next = (console_ring->rx_head + 1) % CONSOLE_BUFFER_SIZE;
            if (next != console_ring->rx_tail) {
                console_ring->rx_buffer[console_ring->rx_head] = c;
                console_ring->rx_head = next;
            }
        }
        microkit_irq_ack(ch);
        microkit_notify(1); // Signal Tyn PD that stdin character is ready
    }
}
