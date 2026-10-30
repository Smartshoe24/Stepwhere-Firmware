/*
 * board.h - Stepwhere GSM PCB pin assignments used by the bootloader.
 * Must match tests/flash_selftest and the application.
 */
#ifndef STEPWHERE_BOARD_H
#define STEPWHERE_BOARD_H

/* W25Q128 (IC8) on SPI1, AF0 */
#define FLASH_SCK_PORT   GPIOA
#define FLASH_SCK_PIN    5u      /* MCU pin 16 */
#define FLASH_MISO_PORT  GPIOA
#define FLASH_MISO_PIN   6u      /* MCU pin 17 */
#define FLASH_MOSI_PORT  GPIOA
#define FLASH_MOSI_PIN   7u      /* MCU pin 18 */
#define FLASH_CS_PORT    GPIOC
#define FLASH_CS_PIN     6u      /* MCU pin 30, active low  */
#define FLASH_WP_PORT    GPIOC
#define FLASH_WP_PIN     7u      /* MCU pin 31, held high   */
#define FLASH_HOLD_PORT  GPIOB
#define FLASH_HOLD_PIN   12u     /* MCU pin 24, held high   */

/* GSM/GPS LDO enable. Used only if the flash turns out to be on that rail. */
#define LDO_EN_PORT      GPIOA
#define LDO_EN_PIN       10u

/* Debug console USART1, AF0, 115200 8N1 (TX only in the bootloader) */
#define DBG_TX_PORT      GPIOB
#define DBG_TX_PIN       6u
#define DBG_BAUD         115200u

/* Clock: reset default HSI16, SYSCLK = HCLK = PCLK = 16 MHz */
#define SYSCLK_HZ        16000000u

#endif /* STEPWHERE_BOARD_H */
