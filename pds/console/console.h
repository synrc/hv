#pragma once

#include <stdint.h>
#include <stddef.h>

#define CONSOLE_BUFFER_SIZE 4096

typedef struct {
    // Output Ring Buffer (Tyn -> Console -> UART TX)
    volatile uint32_t tx_head;
    volatile uint32_t tx_tail;
    volatile char tx_buffer[CONSOLE_BUFFER_SIZE];

    // Input Ring Buffer (UART RX -> Console -> Tyn)
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    volatile char rx_buffer[CONSOLE_BUFFER_SIZE];
} console_ring_t;

static inline void console_ring_write_tx(console_ring_t *ring, const char *data, size_t len) {
    if (!ring) return;
    for (size_t i = 0; i < len; i++) {
        uint32_t next = (ring->tx_head + 1) % CONSOLE_BUFFER_SIZE;
        if (next != ring->tx_tail) {
            ring->tx_buffer[ring->tx_head] = data[i];
            ring->tx_head = next;
        }
    }
}

static inline size_t console_ring_read_rx(console_ring_t *ring, char *out, size_t max_len) {
    if (!ring) return 0;
    size_t count = 0;
    while (ring->rx_tail != ring->rx_head && count < max_len) {
        out[count++] = ring->rx_buffer[ring->rx_tail];
        ring->rx_tail = (ring->rx_tail + 1) % CONSOLE_BUFFER_SIZE;
    }
    return count;
}
