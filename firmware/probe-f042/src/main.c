/*
 * main.c — CC5X debug probe firmware, P5a bench bring-up.
 *
 * Data path (all pins verified — see probe_pins.h):
 *   CC5X target --UART--> USART1 (PA9/PA10) --RX ISR--> relay core (timestamped)
 *     --main loop--> USART2 (PA2/PA15) --> ST-LINK VCP --USB--> PC terminal
 *
 * P5b will swap board_pc_write() from USART2/VCP to native USB-CDC (PA11/PA12)
 * with no change to the relay core or this file.
 */
#include "board.h"
#include "relay.h"
#include "probe_pins.h"

int main(void)
{
    relay_init(board_pc_write);
    board_init();

    /* Heartbeat: TIM2 is free-running us; toggle the LED ~2 Hz so a live board
     * is obvious even before any target traffic arrives. */
    uint32_t last_blink = board_timestamp_us();
    int led = 0;

    for (;;) {
        relay_poll();       /* target ring -> encoded frame -> PC TX FIFO */
        board_pc_poll();    /* PC TX FIFO -> USART2 as TXE allows */

        uint32_t now = board_timestamp_us();
        if ((uint32_t)(now - last_blink) >= 250000u) {  /* 250 ms */
            last_blink = now;
            led = !led;
            board_led(led);
        }
    }
}
