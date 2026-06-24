/*
 * usb_cdc.c — STM32F042 USB FS device CDC-ACM driver (P5b). See usb_cdc.h.
 *
 * Direct-register, no HAL/middleware, in the board_f042.c style. Every register,
 * bit, offset and sequence below is confirmed against the in-repo RM0091 Rev 10
 * (USB §30, CRS §7) and the STM32F042 datasheet DocID025832 Rev 5 — page-cited
 * inline. ES0243 Rev 4 (silicon rev A) USB errata are addressed where noted.
 *
 * NOT YET hardware-validated (no bench): a mock-CMSIS host compile-smoke catches
 * typos, and the descriptors are unit-tested (tests/test_usb_desc.c), but the
 * enumeration has not been run on silicon. See HARDWARE.md.
 *
 * PMA access scheme: the F042 USB IP uses the "2 x 16 bits/word" packed layout
 * (RM0091 Table 121, p868): a PMA-local byte offset maps 1:1 to a CPU half-word
 * address at USB_PMAADDR + offset (NOT the x2 stride of the older F103 IP).
 * PMA must be accessed by 8/16-bit only — never 32-bit words (RM0091 p897).
 */
#include "usb_cdc.h"
#include "usb_desc.h"
#include "bytefifo.h"

#if defined(__has_include)
#  if __has_include("stm32f042x6.h")
#    include "stm32f042x6.h"
#    define HAVE_CMSIS 1
#  endif
#endif

#ifdef HAVE_CMSIS

/* ---- Register bit definitions (RM0091 §30.6, values stated explicitly so the
 *      code does not depend on which field macros a CMSIS version ships) ------ */
/* USB_CNTR (offset 0x40), RM0091 p884-886 */
#define CNTR_CTRM    (1u << 15)
#define CNTR_RESETM  (1u << 10)
#define CNTR_FRES    (1u << 0)
#define CNTR_PDWN    (1u << 1)
/* USB_ISTR (offset 0x44), RM0091 p886-889. rc_w0 bits: clear by writing 0 to the
 * serviced bit and 1 to all others (load, never read-modify-write). */
#define ISTR_CTR     (1u << 15)
#define ISTR_RESET   (1u << 10)
#define ISTR_DIR     (1u << 4)
#define ISTR_EPID    (0xFu)
/* USB_DADDR (offset 0x4C), RM0091 p890 */
#define DADDR_EF     (1u << 7)
/* USB_BCDR (offset 0x58), RM0091 p891 */
#define BCDR_DPPU    (1u << 15)
/* USB_EPnR bits (RM0091 p892-896) */
#define EP_CTR_RX    (1u << 15)   /* rc_w0 */
#define EP_DTOG_RX   (1u << 14)   /* toggle-on-write-1 */
#define EP_STAT_RX   (3u << 12)   /* toggle-on-write-1 */
#define EP_SETUP     (1u << 11)   /* read-only */
#define EP_TYPE      (3u << 9)    /* rw */
#define EP_KIND      (1u << 8)    /* rw */
#define EP_CTR_TX    (1u << 7)    /* rc_w0 */
#define EP_DTOG_TX   (1u << 6)    /* toggle-on-write-1 */
#define EP_STAT_TX   (3u << 4)    /* toggle-on-write-1 */
#define EP_EA        (0xFu)       /* rw */
#define EP_TYPE_BULK      (0u << 9)
#define EP_TYPE_CONTROL   (1u << 9)
#define EP_TYPE_INTERRUPT (3u << 9)
#define STAT_DISABLED 0u
#define STAT_STALL    1u
#define STAT_NAK      2u
#define STAT_VALID    3u
/* Invariant (rw + rc_w0) bits to preserve on any EPnR write (RM0091 p892):
 * write the toggle bits as 0 (no change) and force CTR bits to 1 (do not clear). */
#define EPREG_MASK   (EP_CTR_RX | EP_TYPE | EP_KIND | EP_CTR_TX | EP_EA)  /* 0x878F */

/* RCC/CRS bits used here (values per RM0091 §6/§7; named with a _BIT suffix so
 * they never collide with the CMSIS header's own EN-suffixed macros). */
#define RCC_CR2_HSI48ON_BIT     (1u << 16)   /* RM0091 p134 (also set in board P5a) */
#define RCC_CR2_HSI48RDY_BIT    (1u << 17)
#define RCC_APB1ENR_USBEN_BIT   (1u << 23)   /* RM0091 p125 */
#define RCC_APB1ENR_CRSEN_BIT   (1u << 27)
/* RCC_CFGR3 USBSW = 0 selects HSI48 as USB clock (reset default; RM0091 p119). */
#define CRS_CR_CEN_BIT          (1u << 5)    /* RM0091 p143 */
#define CRS_CR_AUTOTRIMEN_BIT   (1u << 6)

/* ---- USB standard / CDC request constants (USB 2.0 §9.4; CDC PSTN §6.3) ----- */
#define REQ_GET_STATUS         0x00u
#define REQ_CLEAR_FEATURE      0x01u
#define REQ_SET_FEATURE        0x03u
#define REQ_SET_ADDRESS        0x05u
#define REQ_GET_DESCRIPTOR     0x06u
#define REQ_GET_CONFIGURATION  0x08u
#define REQ_SET_CONFIGURATION  0x09u
#define CDC_SET_LINE_CODING        0x20u
#define CDC_GET_LINE_CODING        0x21u
#define CDC_SET_CONTROL_LINE_STATE 0x22u
#define RT_TYPE_MASK   0x60u            /* bmRequestType type field */
#define RT_TYPE_STD    0x00u
#define RT_TYPE_CLASS  0x20u

/* ---- Endpoint numbers & PMA layout (PMA-local byte offsets) ---------------- */
#define EP_CTRL   0u
#define EP_DATA   1u
#define EP_NOTIF  2u

#define BTABLE_OFFSET  0x000u           /* 3 EPs * 8 B = 24 B; round to 64 B    */
#define EP0_TX_OFF     0x040u
#define EP0_RX_OFF     0x080u
#define EP1_TX_OFF     0x0C0u           /* bulk IN  buffer */
#define EP1_RX_OFF     0x100u           /* bulk OUT buffer */
#define EP2_TX_OFF     0x140u           /* notification buffer */
/* All buffers <= 64 B; highest used PMA byte = 0x140 + 8 = 0x148 of 0x400. */

/* ---- Register/PMA accessors ----------------------------------------------- */
/* EPnR registers sit at USB_BASE + n*4 (CMSIS struct has a reserved u16 gap). */
#define EPR(n)  (*(&USB->EP0R + 2u * (n)))

/* BTABLE entry half-words for endpoint n (RM0091 §30.6.2). */
static volatile uint16_t *pma16(uint32_t off)
{
    return (volatile uint16_t *)(USB_PMAADDR + off);
}
#define BT_TX_ADDR(n)   pma16(BTABLE_OFFSET + (n) * 8u + 0u)
#define BT_TX_COUNT(n)  pma16(BTABLE_OFFSET + (n) * 8u + 2u)
#define BT_RX_ADDR(n)   pma16(BTABLE_OFFSET + (n) * 8u + 4u)
#define BT_RX_COUNT(n)  pma16(BTABLE_OFFSET + (n) * 8u + 6u)

/* COUNTn_RX block encoding for a 64-byte buffer: BL_SIZE=1 (32-B blocks),
 * NUM_BLOCK=1 → 64 B allocated (RM0091 Table 130, p899). */
#define RXCOUNT_64   ((1u << 15) | (1u << 10))

static void pma_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    volatile uint16_t *d = pma16(off);
    uint32_t i = 0;
    for (; i + 1u < n; i += 2u)
        *d++ = (uint16_t)(src[i] | ((uint16_t)src[i + 1u] << 8));
    if (i < n)
        *d = src[i];                    /* trailing odd byte (high byte unused) */
}

static void pma_read(uint32_t off, uint8_t *dst, uint32_t n)
{
    const volatile uint16_t *s = pma16(off);
    uint32_t i = 0;
    for (; i + 1u < n; i += 2u) {
        uint16_t w = *s++;
        dst[i] = (uint8_t)w;
        dst[i + 1u] = (uint8_t)(w >> 8);
    }
    if (i < n)
        dst[i] = (uint8_t)(*s);
}

/* ---- EPnR read-modify-write helpers (RM0091 p892 invariant-bit rules) ------ */
static void ep_set_stat_tx(uint8_t n, uint32_t stat)
{
    uint16_t r = EPR(n);
    uint16_t toggle = (uint16_t)((r ^ (uint16_t)(stat << 4)) & EP_STAT_TX);
    EPR(n) = (uint16_t)((r & EPREG_MASK) | EP_CTR_RX | EP_CTR_TX | toggle);
}

static void ep_set_stat_rx(uint8_t n, uint32_t stat)
{
    uint16_t r = EPR(n);
    uint16_t toggle = (uint16_t)((r ^ (uint16_t)(stat << 12)) & EP_STAT_RX);
    EPR(n) = (uint16_t)((r & EPREG_MASK) | EP_CTR_RX | EP_CTR_TX | toggle);
}

/* Clear CTR_RX (write 0 to it) while keeping CTR_TX and not toggling stat/dtog. */
static void ep_clear_ctr_rx(uint8_t n)
{
    EPR(n) = (uint16_t)((EPR(n) & EPREG_MASK & ~EP_CTR_RX) | EP_CTR_TX);
}

static void ep_clear_ctr_tx(uint8_t n)
{
    EPR(n) = (uint16_t)((EPR(n) & EPREG_MASK & ~EP_CTR_TX) | EP_CTR_RX);
}

/* Configure an endpoint's TYPE/EA from the reset (0) state; toggles untouched. */
static void ep_config(uint8_t n, uint32_t type, uint8_t addr)
{
    EPR(n) = (uint16_t)(type | addr);
}

/* ---- TX path (bulk IN, EP1) ----------------------------------------------- */
static uint8_t    s_tx_storage[256];
static bytefifo_t s_tx;
static volatile uint8_t s_tx_busy;      /* a packet is in flight on EP1 IN */

/* ---- EP0 control state ---------------------------------------------------- */
static const uint8_t *s_ep0_in_ptr;     /* remaining IN data to send */
static uint16_t       s_ep0_in_rem;
static uint8_t        s_ep0_in_zlp;      /* send a terminating ZLP after data */
static uint8_t        s_ep0_out_data;    /* an OUT data stage (control-write) is expected */
static uint8_t        s_addr_pending;    /* one-shot: apply s_pending_addr after status */
static uint8_t        s_pending_addr;    /* SET_ADDRESS value, applied after status */
/* Written in ISR (SET_CONFIGURATION / USB reset), read in the main loop
 * (usb_cdc_poll / usb_cdc_configured) → volatile. The other EP0 state above is
 * touched only in ISR context (handle_setup/ep0_handler) and needs no qualifier. */
static volatile uint8_t s_configured;
static uint8_t        s_strbuf[64];      /* scratch for string descriptors */
static uint8_t        s_setup[8];        /* last SETUP packet */

/* Send the next packet of the pending control-IN transfer (s_ep0_in_ptr/rem). */
static void ep0_in_continue(void)
{
    uint16_t chunk = (s_ep0_in_rem > USB_CDC_EP0_MAXPACKET)
                         ? USB_CDC_EP0_MAXPACKET : s_ep0_in_rem;
    pma_write(EP0_TX_OFF, s_ep0_in_ptr, chunk);
    *BT_TX_COUNT(EP_CTRL) = chunk;
    s_ep0_in_ptr += chunk;
    s_ep0_in_rem = (uint16_t)(s_ep0_in_rem - chunk);
    ep_set_stat_tx(EP_CTRL, STAT_VALID);
}

/* Queue an IN data response, capped to the host's wLength (USB 2.0 §9.3.5), and
 * arm RX for the OUT status stage that ends this control-read. A terminating
 * ZLP is required ONLY when we return fewer bytes than the host asked for AND
 * that count is an exact multiple of the max packet size (otherwise the final
 * short/full packet already ends the data stage — a ZLP when len==wLength would
 * be a spurious extra IN packet the host is not reading). */
static void ep0_reply(const uint8_t *data, uint16_t len)
{
    uint16_t wlen = (uint16_t)(s_setup[6] | ((uint16_t)s_setup[7] << 8));
    if (len > wlen)
        len = wlen;
    s_ep0_in_ptr = data;
    s_ep0_in_rem = len;
    s_ep0_in_zlp = (len < wlen && (len % USB_CDC_EP0_MAXPACKET) == 0u) ? 1u : 0u;
    ep0_in_continue();
    ep_set_stat_rx(EP_CTRL, STAT_VALID);
}

static void ep0_send_status(void)       /* zero-length IN status stage */
{
    *BT_TX_COUNT(EP_CTRL) = 0;
    s_ep0_in_ptr = 0;
    s_ep0_in_rem = 0;
    s_ep0_in_zlp = 0;
    ep_set_stat_tx(EP_CTRL, STAT_VALID);
}

static void ep0_stall(void)
{
    ep_set_stat_tx(EP_CTRL, STAT_STALL);
    ep_set_stat_rx(EP_CTRL, STAT_STALL);
}

static void handle_get_descriptor(void)
{
    uint8_t  type  = s_setup[3];        /* wValue high = descriptor type */
    uint8_t  index = s_setup[2];        /* wValue low  = descriptor index */
    switch (type) {
    case USB_DT_DEVICE:
        ep0_reply(usb_device_desc, USB_CDC_DEVICE_LEN);
        break;
    case USB_DT_CONFIG:
        ep0_reply(usb_config_desc, USB_CDC_CONFIG_TOTAL_LEN);
        break;
    case USB_DT_STRING: {
        size_t n = usb_desc_string(index, s_strbuf, sizeof s_strbuf);
        if (n == 0)
            ep0_stall();
        else
            ep0_reply(s_strbuf, (uint16_t)n);
        break;
    }
    default:
        ep0_stall();                    /* e.g. DEVICE_QUALIFIER: FS-only → STALL */
        break;
    }
}

static void handle_standard(void)
{
    switch (s_setup[1]) {               /* bRequest */
    case REQ_GET_DESCRIPTOR:
        handle_get_descriptor();
        break;
    case REQ_SET_ADDRESS:
        s_pending_addr = (uint8_t)(s_setup[2] & 0x7Fu);
        s_addr_pending = 1u;            /* one-shot: applied after status ships */
        ep0_send_status();              /* address applied after status (RM §30.5.2) */
        break;
    case REQ_SET_CONFIGURATION:
        /* Activate the CDC data endpoints. EP1 bulk: RX VALID (ready to receive
         * host OUT), TX NAK (nothing to send yet). EP2 notify: TX NAK. */
        ep_config(EP_DATA, EP_TYPE_BULK, EP_DATA);
        *BT_TX_ADDR(EP_DATA)  = EP1_TX_OFF;
        *BT_TX_COUNT(EP_DATA) = 0;
        *BT_RX_ADDR(EP_DATA)  = EP1_RX_OFF;
        *BT_RX_COUNT(EP_DATA) = RXCOUNT_64;
        ep_set_stat_rx(EP_DATA, STAT_VALID);
        ep_set_stat_tx(EP_DATA, STAT_NAK);

        ep_config(EP_NOTIF, EP_TYPE_INTERRUPT, EP_NOTIF);
        *BT_TX_ADDR(EP_NOTIF)  = EP2_TX_OFF;
        *BT_TX_COUNT(EP_NOTIF) = 0;
        ep_set_stat_tx(EP_NOTIF, STAT_NAK);

        s_tx_busy = 0;
        s_configured = (s_setup[2] != 0u);
        ep0_send_status();
        break;
    case REQ_GET_CONFIGURATION: {
        uint8_t cfg = s_configured ? 1u : 0u;
        ep0_reply(&cfg, 1u);
        break;
    }
    case REQ_GET_STATUS: {
        static const uint8_t st[2] = { 0u, 0u };  /* not self-powered/ remote-wake here */
        ep0_reply(st, 2u);
        break;
    }
    case REQ_CLEAR_FEATURE:
    case REQ_SET_FEATURE:
        ep0_send_status();
        break;
    default:
        ep0_stall();
        break;
    }
}

static void handle_class(void)
{
    switch (s_setup[1]) {               /* bRequest */
    case CDC_GET_LINE_CODING: {
        /* 115200 8N1 placeholder: dwDTERate=115200, bCharFormat=0, bParity=0,
         * bDataBits=8 (CDC PSTN §6.3.11). The probe does not gate on it. */
        static const uint8_t lc[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };
        ep0_reply(lc, 7u);
        break;
    }
    case CDC_SET_LINE_CODING:
        /* 7-byte OUT data stage follows; accept and discard it. Mark that an OUT
         * data stage is expected, then arm RX; the IN status ZLP is sent when
         * that OUT completes (ep0_handler), not now. */
        s_ep0_out_data = 1u;
        ep_set_stat_rx(EP_CTRL, STAT_VALID);
        break;
    case CDC_SET_CONTROL_LINE_STATE:
        ep0_send_status();              /* DTR/RTS — no data stage */
        break;
    default:
        ep0_stall();
        break;
    }
}

static void handle_setup(void)
{
    pma_read(EP0_RX_OFF, s_setup, 8u);
    /* Reset per-transfer state; each handler arms its own STAT_RX/STAT_TX. */
    s_ep0_in_ptr  = 0;
    s_ep0_in_rem  = 0;
    s_ep0_in_zlp  = 0;
    s_ep0_out_data = 0;
    switch (s_setup[0] & RT_TYPE_MASK) {
    case RT_TYPE_STD:   handle_standard(); break;
    case RT_TYPE_CLASS: handle_class();    break;
    default:            ep0_stall();       break;
    }
}

/* EP0 correct-transfer servicing. Runs only in ISR context (no main-loop access
 * to EP0), so the EPnR read-modify-writes here are not raced. */
static void ep0_handler(void)
{
    uint16_t epr = EPR(EP_CTRL);
    if (epr & EP_CTR_RX) {
        uint8_t is_setup = (epr & EP_SETUP) ? 1u : 0u;
        ep_clear_ctr_rx(EP_CTRL);
        if (is_setup) {
            /* handle_setup() sets the right STAT_RX/STAT_TX per request — do NOT
             * blanket re-arm RX afterward (that would defeat an ep0_stall()). */
            handle_setup();
        } else if (s_ep0_out_data) {
            /* OUT data stage of a control-write (e.g. SET_LINE_CODING) completed:
             * accept the payload (discarded) and ship the IN status ZLP. */
            s_ep0_out_data = 0;
            ep0_send_status();
            ep_set_stat_rx(EP_CTRL, STAT_VALID);   /* ready for the next SETUP */
        } else {
            /* OUT status stage of a control-read (host ZLP): nothing to send,
             * just re-arm RX. (A blanket ep0_send_status() here would queue a
             * spurious IN packet on an already-complete transfer.) */
            ep_set_stat_rx(EP_CTRL, STAT_VALID);
        }
    }
    if (EPR(EP_CTRL) & EP_CTR_TX) {
        ep_clear_ctr_tx(EP_CTRL);
        if (s_ep0_in_rem > 0u) {        /* continue a multi-packet IN transfer */
            ep0_in_continue();
        } else if (s_ep0_in_zlp) {      /* terminating zero-length packet */
            s_ep0_in_zlp = 0;
            *BT_TX_COUNT(EP_CTRL) = 0;
            ep_set_stat_tx(EP_CTRL, STAT_VALID);
        } else if (s_addr_pending) {
            /* Apply the device address now that its status stage has shipped. */
            USB->DADDR = (uint16_t)(DADDR_EF | s_pending_addr);
            s_addr_pending = 0;
        }
    }
}

/* EP1 bulk data servicing. */
static void ep1_handler(void)
{
    uint16_t epr = EPR(EP_DATA);
    if (epr & EP_CTR_RX) {              /* host → device bulk OUT: drain & discard */
        ep_clear_ctr_rx(EP_DATA);
        ep_set_stat_rx(EP_DATA, STAT_VALID);   /* re-arm for the next OUT packet */
    }
    if (EPR(EP_DATA) & EP_CTR_TX) {    /* previous IN packet was ACKed */
        ep_clear_ctr_tx(EP_DATA);
        s_tx_busy = 0;
    }
}

/* ---- USB reset: (re)initialize to the default control endpoint state ------- */
static void usb_reset(void)
{
    USB->BTABLE = BTABLE_OFFSET;

    /* EP0 control buffers. */
    *BT_TX_ADDR(EP_CTRL)  = EP0_TX_OFF;
    *BT_TX_COUNT(EP_CTRL) = 0;
    *BT_RX_ADDR(EP_CTRL)  = EP0_RX_OFF;
    *BT_RX_COUNT(EP_CTRL) = RXCOUNT_64;
    ep_config(EP_CTRL, EP_TYPE_CONTROL, EP_CTRL);
    ep_set_stat_rx(EP_CTRL, STAT_VALID);
    ep_set_stat_tx(EP_CTRL, STAT_NAK);

    s_ep0_in_ptr = 0;
    s_ep0_in_rem = 0;
    s_ep0_in_zlp = 0;
    s_ep0_out_data = 0;
    s_addr_pending = 0;
    s_pending_addr = 0;
    s_configured = 0;
    s_tx_busy = 0;

    /* Address 0, function enabled (RM0091 §30.5.2). */
    USB->DADDR = DADDR_EF;
}

void usb_cdc_irq(void)
{
    uint16_t istr = (uint16_t)USB->ISTR;

    if (istr & ISTR_RESET) {
        USB->ISTR = (uint16_t)~ISTR_RESET;   /* clear (write 0 to bit, 1 elsewhere) */
        usb_reset();
        return;
    }

    /* Service all pending CTR events (CTR is cleared via the EPnR, not ISTR). */
    while (USB->ISTR & ISTR_CTR) {
        uint8_t ep = (uint8_t)(USB->ISTR & ISTR_EPID);
        if (ep == EP_CTRL)
            ep0_handler();
        else if (ep == EP_DATA)
            ep1_handler();
        else
            /* Unexpected endpoint: clear both CTR bits (write 0) so the loop
             * terminates — `& EPREG_MASK` alone would preserve them (rc_w0). */
            EPR(ep) = (uint16_t)(EPR(ep) & EPREG_MASK & ~(EP_CTR_RX | EP_CTR_TX));
    }
}

/* ---- Public transport API -------------------------------------------------- */
void usb_cdc_init(void)
{
    bytefifo_init(&s_tx, s_tx_storage, sizeof s_tx_storage);

    /* USB 48 MHz clock = HSI48 (RCC_CFGR3.USBSW=0, reset default). HSI48 is
     * already enabled by board clock_init() (P5a); ensure it regardless. */
    RCC->CR2 |= RCC_CR2_HSI48ON_BIT;
    while ((RCC->CR2 & RCC_CR2_HSI48RDY_BIT) == 0u) { }

    /* Crystal-less trim: CRS auto-trims HSI48 against USB SOF. CRS_CFGR reset
     * value (0x2022BB7F) already selects SYNCSRC=USB SOF, RELOAD=47999, so only
     * enable the clock + counter + auto-trim (RM0091 §7.7, p143-144). */
    RCC->APB1ENR |= RCC_APB1ENR_CRSEN_BIT;
    CRS->CR |= CRS_CR_AUTOTRIMEN_BIT | CRS_CR_CEN_BIT;

    /* USB peripheral clock. */
    RCC->APB1ENR |= RCC_APB1ENR_USBEN_BIT;

    /* Power up the analog transceiver, then release reset (RM0091 §30.5.2):
     * exit power-down (PDWN=0), wait t_STARTUP, then clear FRES. The startup
     * spin is generous (HSI48 ~48 cycles/us); transceiver t_STARTUP ~1 us. */
    USB->CNTR = CNTR_FRES;              /* PDWN=0, keep peripheral in reset */
    for (volatile uint32_t d = 0; d < 200u; d++) { }
    USB->CNTR = 0;                      /* clear FRES → leave reset */
    USB->ISTR = 0;                      /* clear any spurious pending interrupts */

    /* Enable reset + correct-transfer interrupts. */
    USB->CNTR = CNTR_CTRM | CNTR_RESETM;

    /* Assert the embedded DP pull-up → signal connect to the host (RM0091 p891).
     * PA11/PA12 are USB additional-function pins (DS Table 13) and need no GPIO
     * configuration — the transceiver drives them directly. */
    USB->BCDR |= BCDR_DPPU;

    NVIC_EnableIRQ(USB_IRQn);           /* vector 31 (CMSIS) */
}

size_t usb_cdc_write(const uint8_t *buf, size_t len)
{
    return bytefifo_push_all(&s_tx, buf, len) ? len : 0u;
}

void usb_cdc_poll(void)
{
    if (!s_configured)
        return;

    /* EP1's EPnR and s_tx_busy are also written by the USB ISR (ep1_handler,
     * SET_CONFIGURATION). Mask just the USB interrupt around the busy-check +
     * arm so the read-modify-write on the endpoint register can't be torn, and
     * so a re-configuration can't reset EP1 mid-arm. USART1 (target timestamp)
     * stays unmasked. */
    NVIC_DisableIRQ(USB_IRQn);
    if (!s_tx_busy && bytefifo_count(&s_tx) > 0u) {
        /* Pack up to one bulk max-packet (64 B) from the FIFO into the IN buffer. */
        uint8_t pkt[USB_CDC_BULK_MAXPACKET];
        uint32_t n = 0;
        uint8_t b;
        while (n < USB_CDC_BULK_MAXPACKET && bytefifo_pop(&s_tx, &b))
            pkt[n++] = b;

        pma_write(EP1_TX_OFF, pkt, n);
        *BT_TX_COUNT(EP_DATA) = (uint16_t)n;
        s_tx_busy = 1;
        ep_set_stat_tx(EP_DATA, STAT_VALID);
    }
    NVIC_EnableIRQ(USB_IRQn);
}

int usb_cdc_configured(void)
{
    return s_configured;
}

#endif /* HAVE_CMSIS */
