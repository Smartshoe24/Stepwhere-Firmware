/*
 * boot_core.h - Hardware-independent bootloader logic (state machine).
 *
 * main.c supplies the hardware through boot_port_t. The host tests supply a
 * simulated MCU flash and W25Q128 instead, including power cuts.
 */
#ifndef STEPWHERE_BOOT_CORE_H
#define STEPWHERE_BOOT_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "w25q.h"

#define BOOT_VERSION_STR "0.1.0"

typedef struct {
    void *ctx;
    /* External flash, already initialised. NULL if it did not respond: the
     * bootloader then just starts the app if it looks valid. */
    w25q_t *ext;
    /* Readable view of the application area (APP_ADDR .. +APP_MAX_SIZE).
     * On the MCU this is simply (const uint8_t *)APP_ADDR. */
    const uint8_t *app_mem;
    /* Erase one 2 KB internal flash page. addr is absolute (0x0800xxxx).
     * Returns 0 on success. */
    int (*erase_page)(void *ctx, uint32_t addr);
    /* Program len bytes (multiple of 8) at addr (8-byte aligned, erased).
     * Returns 0 on success. */
    int (*program)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len);
    /* Write one log line (no newline included). May be NULL. */
    void (*log)(void *ctx, const char *line);
} boot_port_t;

typedef enum {
    BOOT_JUMP = 0,   /* start the application at APP_ADDR      */
    BOOT_NO_APP      /* nothing bootable, stay in the bootloader */
} boot_result_t;

boot_result_t boot_run(const boot_port_t *p);

/* True if the vector table at APP_ADDR looks like a valid application. */
bool boot_app_valid(const boot_port_t *p);

#endif /* STEPWHERE_BOOT_CORE_H */
