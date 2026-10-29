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

#define BOOT_VERSION_STR "0.2.0"

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
    /* ECDSA P-256 public key (raw X||Y). Images in the download slot must be
     * signed with the matching private key. NULL disables the check (tests). */
    const uint8_t *pubkey;
    /* Independent watchdog. Started only while an update, trial or rollback
     * is in progress; it cannot be stopped again until the next reset.
     * May be NULL. */
    void (*wdg_start)(void *ctx);
    void (*wdg_kick)(void *ctx);
    /* Milliseconds, for timing logs. May be NULL. */
    uint32_t (*millis)(void *ctx);
} boot_port_t;

typedef enum {
    BOOT_JUMP = 0,   /* start the application at APP_ADDR                 */
    BOOT_NO_APP,     /* nothing bootable, stay in the bootloader          */
    BOOT_RESET       /* work done, watchdog running but not needed: reset
                        so the next boot starts the app without it         */
} boot_result_t;

boot_result_t boot_run(const boot_port_t *p);

/* True if the vector table at APP_ADDR looks like a valid application. */
bool boot_app_valid(const boot_port_t *p);

#endif /* STEPWHERE_BOOT_CORE_H */
