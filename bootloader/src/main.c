/*
 * main.c - Stepwhere bootloader, STM32G070CB hardware layer.
 *
 * Register-level code (no HAL) to stay well inside the 16 KB budget.
 * Runs on the reset-default 16 MHz HSI clock; the app sets its own clocks.
 */
#include <string.h>

#include "stm32g0xx.h"

#include "board.h"
#include "boot_core.h"
#include "ota_public_key.h"
#include "ota_layout.h"
#include "w25q.h"

/* ------------------------------------------------------------------------ */
/* Time                                                                     */

static void log_line(void *ctx, const char *line);

static volatile uint32_t s_ms;

void SysTick_Handler(void)
{
    s_ms++;
}

static uint32_t ms_now(void) { return s_ms; }

static void delay_ms(uint32_t ms)
{
    uint32_t t0 = s_ms;
    while ((uint32_t)(s_ms - t0) < ms) { }
}

static void delay_us(uint32_t us)
{
    /* ~4 cycles per iteration at 16 MHz; only needs to be "at least". */
    volatile uint32_t n = us * (SYSCLK_HZ / 4000000u) + 1u;
    while (n--) { }
}

/* ------------------------------------------------------------------------ */
/* Independent watchdog: LSI ~32 kHz / 256 = 125 Hz, reload 2500 -> ~20 s.  */

#define IWDG_RELOAD 2500u

static void wdg_kick(void) { IWDG->KR = 0xAAAAu; }

static void port_wdg_start(void *ctx)
{
    (void)ctx;
    IWDG->KR = 0xCCCCu;            /* start (also enables the LSI) */
    IWDG->KR = 0x5555u;            /* unlock PR/RLR */
    IWDG->PR = 6u;                 /* /256 */
    IWDG->RLR = IWDG_RELOAD;
    while (IWDG->SR != 0u) { }
    wdg_kick();
    log_line(NULL, "[BOOT] watchdog on (~20 s)");
}

static void port_wdg_kick(void *ctx) { (void)ctx; wdg_kick(); }

/* ------------------------------------------------------------------------ */
/* GPIO                                                                     */

enum { MODE_IN = 0, MODE_OUT = 1, MODE_AF = 2 };

static void gpio_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af)
{
    port->OSPEEDR |= 3u << (pin * 2u);                                   /* very high */
    port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
    if (mode == MODE_AF) {
        volatile uint32_t *afr = &port->AFR[pin >> 3];
        uint32_t sh = (pin & 7u) * 4u;
        *afr = (*afr & ~(0xFu << sh)) | (af << sh);
    }
}

static void gpio_write(GPIO_TypeDef *port, uint32_t pin, int high)
{
    port->BSRR = high ? (1u << pin) : (1u << (pin + 16u));
}

/* ------------------------------------------------------------------------ */
/* Debug UART (USART1 TX on PB6)                                            */

static void uart_init(void)
{
    gpio_mode(DBG_TX_PORT, DBG_TX_PIN, MODE_AF, 0);
    USART1->BRR = (SYSCLK_HZ + DBG_BAUD / 2u) / DBG_BAUD;
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void uart_puts(const char *s)
{
    while (*s) {
        while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) { }
        USART1->TDR = (uint8_t)*s++;
    }
}

static void uart_flush(void)
{
    while (!(USART1->ISR & USART_ISR_TC)) { }
}

static void log_line(void *ctx, const char *line)
{
    (void)ctx;
    uart_puts(line);
    uart_puts("\r\n");
}

/* ------------------------------------------------------------------------ */
/* SPI1 + W25Q128                                                           */

static void spi_init(void)
{
    gpio_mode(FLASH_CS_PORT, FLASH_CS_PIN, MODE_OUT, 0);
    gpio_write(FLASH_CS_PORT, FLASH_CS_PIN, 1);
    gpio_mode(FLASH_WP_PORT, FLASH_WP_PIN, MODE_OUT, 0);
    gpio_write(FLASH_WP_PORT, FLASH_WP_PIN, 1);
    gpio_mode(FLASH_HOLD_PORT, FLASH_HOLD_PIN, MODE_OUT, 0);
    gpio_write(FLASH_HOLD_PORT, FLASH_HOLD_PIN, 1);

    gpio_mode(FLASH_SCK_PORT, FLASH_SCK_PIN, MODE_AF, 0);
    gpio_mode(FLASH_MISO_PORT, FLASH_MISO_PIN, MODE_AF, 0);
    gpio_mode(FLASH_MOSI_PORT, FLASH_MOSI_PIN, MODE_AF, 0);
    /* Pull-up on MISO: a missing chip then reads 0xFF instead of noise. */
    FLASH_MISO_PORT->PUPDR = (FLASH_MISO_PORT->PUPDR & ~(3u << (FLASH_MISO_PIN * 2u))) |
                             (1u << (FLASH_MISO_PIN * 2u));

    /* Master, mode 0, software NSS, fPCLK/2 = 8 MHz, 8-bit frames. */
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;
    SPI1->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;
    SPI1->CR1 |= SPI_CR1_SPE;
}

static void bus_select(void *ctx, bool active)
{
    (void)ctx;
    gpio_write(FLASH_CS_PORT, FLASH_CS_PIN, active ? 0 : 1);
}

static void bus_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    volatile uint8_t *dr = (volatile uint8_t *)&SPI1->DR;
    for (size_t i = 0; i < len; i++) {
        while (!(SPI1->SR & SPI_SR_TXE)) { }
        *dr = tx ? tx[i] : 0xFFu;
        while (!(SPI1->SR & SPI_SR_RXNE)) { }
        uint8_t b = *dr;
        if (rx) rx[i] = b;
    }
}

/* Called in every W25Q busy-wait loop, so long erases also feed the watchdog. */
static uint32_t bus_millis(void *ctx) { (void)ctx; wdg_kick(); return ms_now(); }
static void bus_delay_us(void *ctx, uint32_t us) { (void)ctx; delay_us(us); }

static const w25q_bus_t s_bus = { NULL, bus_select, bus_transfer, bus_millis, bus_delay_us };

/* Fast check for a Winbond answer, without the long BUSY wait of init. */
static bool flash_probe(w25q_t *dev)
{
    uint8_t manuf = 0;
    memset(dev, 0, sizeof *dev);
    dev->bus = s_bus;
    dev->powered_down = true;   /* sends Release Power-down first */
    w25q_read_jedec_id(dev, &manuf, NULL);
    return manuf == W25Q_MANUF_WINBOND;
}

static w25q_t s_flash;
static bool s_ldo_used;

static w25q_t *flash_start(void)
{
    if (flash_probe(&s_flash) && w25q_init(&s_flash, &s_bus) == W25Q_OK) {
        log_line(NULL, "[BOOT] external flash found (always-on rail)");
        return &s_flash;
    }
    /* Not answering: maybe it is powered from the GSM/GPS LDO. */
    gpio_write(LDO_EN_PORT, LDO_EN_PIN, 1);
    delay_ms(100);
    if (flash_probe(&s_flash) && w25q_init(&s_flash, &s_bus) == W25Q_OK) {
        log_line(NULL, "[BOOT] external flash found (LDO_EN rail)");
        s_ldo_used = true;
        return &s_flash;
    }
    gpio_write(LDO_EN_PORT, LDO_EN_PIN, 0);
    /* Last try with the full init: covers a chip still busy finishing an
     * erase that was running when the MCU was reset (up to ~3 s). */
    if (w25q_init(&s_flash, &s_bus) == W25Q_OK) {
        log_line(NULL, "[BOOT] external flash found after BUSY wait");
        return &s_flash;
    }
    log_line(NULL, "[BOOT] external flash not responding");
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Internal flash                                                           */

#define FLASH_SR_ERR_MASK (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                           FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | \
                           FLASH_SR_OPTVERR)

static void mcu_flash_wait(void)
{
    while (FLASH->SR & (FLASH_SR_BSY1 | FLASH_SR_CFGBSY)) { }
}

static void mcu_flash_unlock(void)
{
    mcu_flash_wait();
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

static int mcu_flash_result(void)
{
    uint32_t sr = FLASH->SR;
    FLASH->SR = sr & (FLASH_SR_ERR_MASK | FLASH_SR_EOP);   /* write 1 to clear */
    return (sr & FLASH_SR_ERR_MASK) ? -1 : 0;
}

static int port_erase_page(void *ctx, uint32_t addr)
{
    (void)ctx;
    /* Never touch the bootloader itself. */
    if (addr < APP_ADDR || addr >= MCU_FLASH_BASE + MCU_FLASH_SIZE || (addr % MCU_FLASH_PAGE_SIZE)) {
        return -1;
    }
    uint32_t page = (addr - MCU_FLASH_BASE) / MCU_FLASH_PAGE_SIZE;
    wdg_kick();
    mcu_flash_unlock();
    FLASH->SR = FLASH_SR_ERR_MASK | FLASH_SR_EOP;
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_PNB | FLASH_CR_PG)) | (page << FLASH_CR_PNB_Pos) | FLASH_CR_PER;
    FLASH->CR |= FLASH_CR_STRT;
    mcu_flash_wait();
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
    return mcu_flash_result();
}

static int port_program(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    if (addr < APP_ADDR || (addr & 7u) || (len & 7u) || addr + len > MCU_FLASH_BASE + MCU_FLASH_SIZE) {
        return -1;
    }
    mcu_flash_unlock();
    FLASH->SR = FLASH_SR_ERR_MASK | FLASH_SR_EOP;
    for (uint32_t i = 0; i < len; i += 8u) {
        uint32_t w0, w1;
        memcpy(&w0, data + i, 4);
        memcpy(&w1, data + i + 4, 4);
        FLASH->CR |= FLASH_CR_PG;
        *(volatile uint32_t *)(addr + i) = w0;
        __ISB();
        *(volatile uint32_t *)(addr + i + 4u) = w1;   /* second word starts programming */
        mcu_flash_wait();
        FLASH->CR &= ~FLASH_CR_PG;
        if (mcu_flash_result() != 0) {
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Jump to the application                                                  */

static void jump_to_app(void)
{
    const uint32_t *vt = (const uint32_t *)APP_ADDR;
    uint32_t sp = vt[0], reset = vt[1];

    uart_flush();
    if (s_flash.manufacturer == W25Q_MANUF_WINBOND) {
        w25q_power_down(&s_flash);   /* app wakes it when it needs it */
    }
    if (s_ldo_used) {
        gpio_write(LDO_EN_PORT, LDO_EN_PIN, 0);
    }
    FLASH->CR |= FLASH_CR_LOCK;

    /* Flush the instruction cache: the app area may just have been rewritten. */
    FLASH->ACR &= ~FLASH_ACR_ICEN;
    FLASH->ACR |= FLASH_ACR_ICRST;
    FLASH->ACR &= ~FLASH_ACR_ICRST;
    FLASH->ACR |= FLASH_ACR_ICEN;

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->VAL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    NVIC->ICER[0] = 0xFFFFFFFFu;
    NVIC->ICPR[0] = 0xFFFFFFFFu;

    /* Return SPI1 and USART1 to reset state. GPIOs are left as they are, so
     * the flash /CS, /WP and /HOLD stay high until the app sets them up. */
    RCC->APBRSTR2 |= RCC_APBRSTR2_SPI1RST | RCC_APBRSTR2_USART1RST;
    RCC->APBRSTR2 &= ~(RCC_APBRSTR2_SPI1RST | RCC_APBRSTR2_USART1RST);
    RCC->APBENR2 &= ~(RCC_APBENR2_SPI1EN | RCC_APBENR2_USART1EN);

    /* The STM32duino core does not set VTOR itself, so it must be set here. */
    SCB->VTOR = APP_ADDR;
    __DSB();
    __set_MSP(sp);
    __ISB();
    __enable_irq();   /* app expects the reset state: interrupts enabled */
    ((void (*)(void))reset)();
    for (;;) { }
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN | RCC_IOPENR_GPIOBEN | RCC_IOPENR_GPIOCEN;
    RCC->APBENR2 |= RCC_APBENR2_SPI1EN | RCC_APBENR2_USART1EN;
    (void)RCC->APBENR2;

    SysTick_Config(SYSCLK_HZ / 1000u);

    gpio_mode(LDO_EN_PORT, LDO_EN_PIN, MODE_OUT, 0);
    gpio_write(LDO_EN_PORT, LDO_EN_PIN, 0);
    uart_init();
    log_line(NULL, "");
    spi_init();
    delay_ms(5);   /* W25Q tPUW: 5 ms after power-up before writes */

    boot_port_t port = {
        .ctx = NULL,
        .ext = flash_start(),
        .app_mem = (const uint8_t *)APP_ADDR,
        .erase_page = port_erase_page,
        .program = port_program,
        .log = log_line,
        .pubkey = OTA_PUBLIC_KEY,
        .wdg_start = port_wdg_start,
        .wdg_kick = port_wdg_kick,
        .millis = bus_millis,
    };

#if OTA_PUBLIC_KEY_IS_DEV
    log_line(NULL, "[BOOT] WARNING: development signing key, not for products");
#endif
    {
        static const char hx[] = "0123456789ABCDEF";
        char line[] = "[BOOT] signing key 00000000";
        for (int i = 0; i < 4; i++) {
            line[19 + 2 * i] = hx[OTA_PUBLIC_KEY[i] >> 4];
            line[20 + 2 * i] = hx[OTA_PUBLIC_KEY[i] & 15];
        }
        log_line(NULL, line);
    }

    boot_result_t r = boot_run(&port);
    if (r == BOOT_JUMP) {
        log_line(NULL, "[BOOT] starting application at 0x08004000");
        jump_to_app();
    }
    if (r == BOOT_RESET) {
        uart_flush();
        NVIC_SystemReset();
    }

    log_line(NULL, "[BOOT] no valid application. Flash the firmware .hex with STM32CubeProgrammer.");
    for (;;) {
        __WFI();
    }
}
