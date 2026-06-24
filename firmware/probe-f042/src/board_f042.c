/*
 * board_f042.c — NUCLEO-F042K6 hardware layer for the probe (P5a).
 *
 * STATUS: implemented. The peripheral init sequences below use register/bit
 * facts confirmed against RM0091 Rev 10 (in repo) — page citations are inline.
 * NOT YET hardware-validated (no bench run); a mock-CMSIS compile-smoke catches
 * typos but not on-silicon behavior. Review ES0243 errata before first flash.
 *
 * Requires vendored CMSIS device support (see README): stm32f042x6.h,
 * system_stm32f0xx.c, startup_stm32f042x6.s, and a 32K-flash/6K-RAM linker
 * script.
 */
#include "board.h"
#include "relay.h"
#include "bytefifo.h"
#include "probe_pins.h"
#include "usb_cdc.h"

#define PC_OVER_USB (PROBE_PC_TRANSPORT == PROBE_PC_TRANSPORT_USB_CDC)

#if defined(__has_include)
#  if __has_include("stm32f042x6.h")
#    include "stm32f042x6.h"
#    define HAVE_CMSIS 1
#  endif
#endif

#ifdef HAVE_CMSIS

/* Register bits used below — all verified against RM0091 Rev 10.
 * (Bit positions stated explicitly so the code does not depend on which
 * optional field-value macros a given CMSIS header version defines.) */
#define FLASH_ACR_PRFTBE_BIT   (1u << 4)    /* prefetch enable        RM0091 p69  */
#define FLASH_ACR_LATENCY_1WS  (1u << 0)    /* 1 wait state (24<f<=48) RM0091 p69 */
#define RCC_CR2_HSI48ON_BIT    (1u << 16)   /* RM0091 p134 */
#define RCC_CR2_HSI48RDY_BIT   (1u << 17)   /* RM0091 p134 */
/* RCC_CFGR_SW_HSI48 (0x3) and RCC_CFGR_SWS_HSI48 (0xC) come from CMSIS and
 * match RM0091 p114 exactly (SW=11 selects HSI48, SWS=11 confirms). */
#define RCC_AHBENR_IOPAEN_BIT  (1u << 17)   /* RM0091 p121 */
#define RCC_AHBENR_IOPBEN_BIT  (1u << 18)   /* RM0091 p121 */
#define RCC_APB2ENR_USART1_BIT (1u << 14)   /* RM0091 p123 */
#define RCC_APB1ENR_TIM2_BIT   (1u << 0)    /* RM0091 p125 */
#define RCC_APB1ENR_USART2_BIT (1u << 17)   /* RM0091 p125 */
#define TIM_CR1_CEN_BIT        (1u << 0)    /* RM0091 p446 */
#define TIM_EGR_UG_BIT         (1u << 0)    /* load PSC/ARR    RM0091 §18.4.6 */
#define USART_CR1_UE_BIT       (1u << 0)    /* RM0091 p744 */
#define USART_CR1_RE_BIT       (1u << 2)    /* RM0091 p744 */
#define USART_CR1_TE_BIT       (1u << 3)    /* RM0091 p744 */
#define USART_CR1_RXNEIE_BIT   (1u << 5)    /* RM0091 p744 */
#define USART_ISR_RXNE_BIT     (1u << 5)    /* RM0091 p758 */
#define USART_ISR_ERR_MASK     (0xFu)       /* ORE=3,NF=2,FE=1,PE=0  RM0091 p758 */
#define USART_ICR_ERR_CLR      (0xFu)       /* ORECF/NCF/FECF/PECF    RM0091 p763 */
#define GPIO_PUPDR_PULLUP      (1u)         /* PUPDR 01 = pull-up     RM0091 p159 */

/* BRR = round(fCK / baud), OVER8=0, fCK = PCLK = SYSCLK (RM0091 §27.5.4). */
#define USART_BRR_VALUE(baud)  ((SYSCLK_HZ + (baud)/2u) / (baud))

#if !PC_OVER_USB
/* PC (VCP) transmit FIFO — backs the all-or-nothing board_pc_write(). The
 * USB-CDC transport keeps its own FIFO inside usb_cdc.c. */
static uint8_t    s_tx_storage[256];
static bytefifo_t s_tx;
#endif

/* GPIO helpers (RM0091 §8.4: MODER 2 bits/pin, AFR 4 bits/pin). PA13/PA14 are
 * left untouched so SWD keeps working (MODER reset already puts them in AF). */
static void gpio_set_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (2u << (pin * 2u)); /* AF=10 */
    uint32_t reg = pin >> 3;            /* AFR[0]=pins0-7, AFR[1]=pins8-15 */
    uint32_t sh  = (pin & 7u) * 4u;
    port->AFR[reg] = (port->AFR[reg] & ~(0xFu << sh)) | (af << sh);
}

static void gpio_set_output(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (1u << (pin * 2u)); /* out=01 */
}

/* Pull-up so an unconnected/idle UART RX line reads high (UART idle = high),
 * preventing a floating pin from generating spurious start bits / noise. */
static void gpio_set_pullup(GPIO_TypeDef *port, uint32_t pin)
{
    port->PUPDR = (port->PUPDR & ~(3u << (pin * 2u))) | (GPIO_PUPDR_PULLUP << (pin * 2u));
}

static volatile uint32_t s_rx_errors;     /* USART1 ORE/FE/NE/PE events */

/* --- Clock: SYSCLK = HSI48 (48 MHz), no crystal (UM §7.8) -------------------- */
static void clock_init(void)
{
    /* Flash latency before raising the clock (RM0091 p69). */
    FLASH->ACR = FLASH_ACR_PRFTBE_BIT | FLASH_ACR_LATENCY_1WS;
    /* Enable HSI48 and wait until stable (RM0091 p134). */
    RCC->CR2 |= RCC_CR2_HSI48ON_BIT;
    while ((RCC->CR2 & RCC_CR2_HSI48RDY_BIT) == 0u) { }
    /* Select HSI48 as SYSCLK; HPRE/PPRE stay at reset (=not divided) so
     * HCLK = PCLK = 48 MHz (RM0091 p112-114). */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_HSI48) | RCC_CFGR_SW_HSI48;
    while ((RCC->CFGR & RCC_CFGR_SWS_HSI48) != RCC_CFGR_SWS_HSI48) { }
}

/* --- GPIO alternate functions (all pins verified in probe_pins.h) ----------- */
static void gpio_init(void)
{
    RCC->AHBENR |= RCC_AHBENR_IOPAEN_BIT | RCC_AHBENR_IOPBEN_BIT;
    gpio_set_af(TARGET_GPIO_PORT, TARGET_TX_PIN, TARGET_USART_AF); /* PA9  USART1_TX */
    gpio_set_af(TARGET_GPIO_PORT, TARGET_RX_PIN, TARGET_USART_AF); /* PA10 USART1_RX */
    gpio_set_pullup(TARGET_GPIO_PORT, TARGET_RX_PIN);             /* PA10 idle-high */
#if !PC_OVER_USB
    /* P5a VCP path needs USART2 GPIO. The USB-CDC path leaves PA11/PA12 at reset
     * (USB additional-function pins, driven by the transceiver — DS Table 13). */
    gpio_set_af(PC_TX_PORT, PC_TX_PIN, PC_USART_AF);               /* PA2  USART2_TX */
    gpio_set_af(PC_RX_PORT, PC_RX_PIN, PC_USART_AF);               /* PA15 USART2_RX */
    gpio_set_pullup(PC_RX_PORT, PC_RX_PIN);                       /* PA15 idle-high */
#endif
    gpio_set_output(LED_PORT, LED_PIN);                           /* PB3 LED */
}

/* --- TIM2: free-running 1 us timestamp (DS Table 7: TIM2 is 32-bit) ---------- */
static void tim2_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2_BIT;
    TIMESTAMP_TIM->PSC = (SYSCLK_HZ / 1000000u) - 1u;   /* 48 MHz/(47+1) = 1 MHz */
    TIMESTAMP_TIM->ARR = 0xFFFFFFFFu;                   /* full 32-bit wrap */
    TIMESTAMP_TIM->EGR = TIM_EGR_UG_BIT;                /* latch PSC/ARR now */
    TIMESTAMP_TIM->CR1 |= TIM_CR1_CEN_BIT;              /* start counting */
}

/* --- USART config (target USART1 @PA9/10 + PC USART2 @PA2/15) ----------------
 * BRR/CR1 must be written with UE=0 (RM0091 p744/p755). 8N1 = M[1:0]=00 (reset).
 * ES0243 2.11.7 (noisy-RX data corruption, no silicon workaround) applies to the
 * target link; it is covered downstream by the CDL CRC8 (01 §3). Other USART
 * errata (2.11.1-2.11.6) don't apply to this 8N1 async bridge — see HARDWARE.md. */
static void usart_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1_BIT;

    TARGET_USART->CR1 = 0u;                              /* UE=0 to program BRR */
    TARGET_USART->BRR = USART_BRR_VALUE(PROBE_TARGET_BAUD);
    TARGET_USART->CR1 = USART_CR1_UE_BIT | USART_CR1_TE_BIT |
                        USART_CR1_RE_BIT | USART_CR1_RXNEIE_BIT;

#if !PC_OVER_USB
    /* P5a: USART2 → ST-LINK VCP. The USB-CDC path uses the USB peripheral instead. */
    RCC->APB1ENR |= RCC_APB1ENR_USART2_BIT;
    PC_USART->CR1 = 0u;
    PC_USART->BRR = USART_BRR_VALUE(PROBE_VCP_BAUD);
    PC_USART->CR1 = USART_CR1_UE_BIT | USART_CR1_TE_BIT | USART_CR1_RE_BIT;
#endif
}

void board_init(void)
{
#if !PC_OVER_USB
    bytefifo_init(&s_tx, s_tx_storage, sizeof s_tx_storage);
#endif
    clock_init();
    gpio_init();
    tim2_init();
    usart_init();
    NVIC_EnableIRQ(USART1_IRQn);          /* target-USART RX */
#if PC_OVER_USB
    usb_cdc_init();                       /* native USB-CDC PC link (P5b) */
#endif
}

uint32_t board_timestamp_us(void)
{
    return TIMESTAMP_TIM->CNT;            /* TIM2 32-bit CNT, 1 MHz (DS T7) */
}

/* All-or-nothing enqueue of an encoded frame: accept the whole buffer or none,
 * so the relay never emits a truncated frame on USART back-pressure. */
size_t board_pc_write(const uint8_t *buf, size_t len)
{
#if PC_OVER_USB
    return usb_cdc_write(buf, len);
#else
    return bytefifo_push_all(&s_tx, buf, len) ? len : 0u;
#endif
}

/* Drain the PC TX path toward the hardware. Call from the main loop. */
void board_pc_poll(void)
{
#if PC_OVER_USB
    usb_cdc_poll();
#else
    /* Drain the VCP TX FIFO to USART2 as TXE allows. */
    uint8_t b;
    while ((PC_USART->ISR & USART_ISR_TXE) && bytefifo_pop(&s_tx, &b))
        PC_USART->TDR = b;
#endif
}

uint32_t board_rx_errors(void) { return s_rx_errors; }

void board_led(int on)
{
#if LED_ACTIVE_HIGH
    LED_PORT->BSRR = on ? (1u << LED_PIN) : (1u << (LED_PIN + 16));
#else
    LED_PORT->BSRR = on ? (1u << (LED_PIN + 16)) : (1u << LED_PIN);
#endif
}

/* Target-USART RX ISR: clear error flags, stamp arrival time, hand the byte to
 * the relay. On the F0 USART, ORE/FE/NE/PE are cleared by writing USART_ICR (NOT
 * by reading RDR) — an uncleared ORE would otherwise stall RX (RM0091 §27.8.8/9).
 * Reading RDR clears RXNE. */
void USART1_IRQHandler(void)
{
    uint32_t isr = TARGET_USART->ISR;
    if (isr & USART_ISR_ERR_MASK) {
        TARGET_USART->ICR = USART_ICR_ERR_CLR;   /* clear ORE/FE/NE/PE */
        s_rx_errors++;
        relay_note_dropped(1);                   /* surface loss in STATUS frame */
    }
    if (isr & USART_ISR_RXNE_BIT) {
        uint32_t ts = board_timestamp_us();
        uint8_t  b  = (uint8_t)TARGET_USART->RDR;
        relay_target_rx(b, ts);
    }
}

#if PC_OVER_USB
/* USB global interrupt (NVIC vector 31, CMSIS USB_IRQn). Overrides the weak
 * USB_IRQHandler in startup_stm32f042x6.s. */
void USB_IRQHandler(void)
{
    usb_cdc_irq();
}
#endif

#endif /* HAVE_CMSIS */
