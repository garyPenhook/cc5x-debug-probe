/*
 * board.h — hardware abstraction the portable app/relay relies on.
 * Implemented for the NUCLEO-F042K6 in board_f042.c (against RM0091 + datasheet).
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include <stddef.h>

/* Bring up clock (SYSCLK=HSI48), GPIO AFs, USART1 (target), USART2 (VCP),
 * TIM2 (1 us timestamp). Enables the USART1 RX interrupt. */
void board_init(void);

/* Current free-running timestamp in microseconds (TIM2 CNT). */
uint32_t board_timestamp_us(void);

/* Write an encoded frame to the PC link (USART2 -> ST-LINK VCP in P5a).
 * All-or-nothing and non-blocking: returns len if the whole frame was queued,
 * or 0 if it did not fit (the relay retries the same frame next poll). */
size_t board_pc_write(const uint8_t *buf, size_t len);

/* Drain the PC-link TX buffer toward the hardware. Call from the main loop. */
void board_pc_poll(void);

/* Status LED (PB3, active-high). */
void board_led(int on);

/* Diagnostics: cumulative target-USART error events (ORE/FE/NE/PE). */
uint32_t board_rx_errors(void);

#endif /* BOARD_H */
