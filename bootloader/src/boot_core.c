/*
 * boot_core.c - Bootloader state machine. See docs/OTA_DESIGN.md.
 *
 * Every state is saved to the W25Q128 BEFORE its step starts. If power is
 * lost, the next boot sees the same state and repeats the step. Steps are
 * written so that repeating them is always safe:
 *   BACKING_UP   internal flash is still untouched; backup is redone
 *   INSTALLING   download slot is still intact; erase + program is redone
 *   ROLLING_BACK backup slot is still intact; restore is redone
 */
#include "boot_core.h"

#include <stdarg.h>
#include <string.h>

#include "ota_image.h"
#include "ota_layout.h"
#include "ota_meta.h"
#include "ota_sha256.h"

#define MAX_STEPS 10   /* guard against state loops */

static uint8_t s_buf[MCU_FLASH_PAGE_SIZE];
static ota_boot_meta_t s_meta;

/* ------------------------------------------------------------------------ */
/* Tiny logger: %s string, %u decimal, %x 0x%08X, %v version a.b.c          */

static char *put_str(char *o, char *end, const char *s)
{
    while (*s && o < end) *o++ = *s++;
    return o;
}

static char *put_dec(char *o, char *end, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n && o < end) *o++ = tmp[--n];
    return o;
}

static char *put_hex(char *o, char *end, uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    o = put_str(o, end, "0x");
    for (int i = 28; i >= 0 && o < end; i -= 4) *o++ = hx[(v >> i) & 15u];
    return o;
}

static void blog(const boot_port_t *p, const char *fmt, ...)
{
    char line[100];
    char *o = line, *end = line + sizeof line - 1;
    va_list ap;

    if (!p->log) return;
    o = put_str(o, end, "[BOOT] ");
    va_start(ap, fmt);
    for (; *fmt && o < end; fmt++) {
        if (*fmt != '%') { *o++ = *fmt; continue; }
        switch (*++fmt) {
        case 's': o = put_str(o, end, va_arg(ap, const char *)); break;
        case 'u': o = put_dec(o, end, va_arg(ap, uint32_t)); break;
        case 'x': o = put_hex(o, end, va_arg(ap, uint32_t)); break;
        case 'v': {
            uint32_t v = va_arg(ap, uint32_t);
            o = put_dec(o, end, (v >> 16) & 0xFFu);
            o = put_str(o, end, ".");
            o = put_dec(o, end, (v >> 8) & 0xFFu);
            o = put_str(o, end, ".");
            o = put_dec(o, end, v & 0xFFu);
            break;
        }
        case '\0': fmt--; break;
        default: *o++ = *fmt; break;
        }
    }
    va_end(ap);
    *o = '\0';
    p->log(p->ctx, line);
}

/* ------------------------------------------------------------------------ */

static uint32_t app_word(const boot_port_t *p, uint32_t off)
{
    uint32_t w;
    memcpy(&w, p->app_mem + off, sizeof w);
    return w;
}

bool boot_app_valid(const boot_port_t *p)
{
    return ota_app_vectors_ok(app_word(p, 0), app_word(p, 4));
}

static bool save_meta(const boot_port_t *p, uint32_t state)
{
    w25q_status_t st;
    s_meta.state = state;
    st = ota_meta_save(p->ext, &s_meta);
    if (st != W25Q_OK) {
        blog(p, "metadata write failed: %s", w25q_status_str(st));
        return false;
    }
    return true;
}

/* Size of the app currently in internal flash. Uses the metadata if it is
 * known, otherwise the end of the programmed area. */
static uint32_t app_used_size(const boot_port_t *p)
{
    uint32_t n = s_meta.installed_size;
    if (n >= 8u && n <= APP_MAX_SIZE) {
        return n;
    }
    n = APP_MAX_SIZE;
    while (n >= 8u) {
        uint32_t a = app_word(p, n - 8u), b = app_word(p, n - 4u);
        if (a != 0xFFFFFFFFUL || b != 0xFFFFFFFFUL) break;
        n -= 8u;
    }
    return n;
}

/* Copy the current internal app into the backup slot as a normal image.
 * The header is written last, so a half-written backup is never valid. */
static bool backup_app(const boot_port_t *p)
{
    ota_image_header_t hdr;
    uint32_t size = app_used_size(p);
    const uint32_t slot = EXT_SLOT_BACKUP;

    memset(&hdr, 0, sizeof hdr);
    hdr.magic = OTA_HDR_MAGIC;
    hdr.hdr_version = OTA_HDR_VERSION;
    hdr.hdr_size = OTA_HDR_SIZE;
    hdr.fw_version = s_meta.installed_version;
    hdr.image_size = size;
    hdr.load_addr = APP_ADDR;
    hdr.hw_id = OTA_HW_ID_GSM_PCB_R1;
    ota_sha256(p->app_mem, size, hdr.payload_sha256);
    ota_image_header_seal(&hdr);

    blog(p, "backing up current app (%u bytes)", size);
    if (w25q_erase_range(p->ext, slot, EXT_SLOT_SIZE) != W25Q_OK ||
        w25q_write_verify(p->ext, slot + OTA_HDR_SIZE, p->app_mem, size) != W25Q_OK ||
        w25q_write_verify(p->ext, slot, &hdr, sizeof hdr) != W25Q_OK) {
        blog(p, "backup failed");
        return false;
    }
    return true;
}

/* Erase the app area and program it from an already verified slot image. */
static bool install_from(const boot_port_t *p, uint32_t slot, const ota_image_header_t *hdr)
{
    uint8_t digest[OTA_SHA256_LEN];
    uint32_t addr;

    for (addr = APP_ADDR; addr < APP_ADDR + APP_MAX_SIZE; addr += MCU_FLASH_PAGE_SIZE) {
        if (p->erase_page(p->ctx, addr) != 0) {
            blog(p, "erase failed at %x", addr);
            return false;
        }
    }
    for (uint32_t off = 0; off < hdr->image_size; off += MCU_FLASH_PAGE_SIZE) {
        uint32_t n = hdr->image_size - off;
        if (n > MCU_FLASH_PAGE_SIZE) n = MCU_FLASH_PAGE_SIZE;
        if (w25q_read(p->ext, slot + OTA_HDR_SIZE + off, s_buf, n) != W25Q_OK) {
            blog(p, "read failed at slot offset %u", off);
            return false;
        }
        uint32_t padded = (n + 7u) & ~7u;
        memset(s_buf + n, 0xFF, padded - n);
        if (p->program(p->ctx, APP_ADDR + off, s_buf, padded) != 0) {
            blog(p, "program failed at %x", APP_ADDR + off);
            return false;
        }
    }
    ota_sha256(p->app_mem, hdr->image_size, digest);
    if (memcmp(digest, hdr->payload_sha256, sizeof digest) != 0) {
        blog(p, "verify after programming failed");
        return false;
    }
    s_meta.installed_version = hdr->fw_version;
    s_meta.installed_size = hdr->image_size;
    memcpy(s_meta.installed_sha256, hdr->payload_sha256, OTA_SHA256_LEN);
    return true;
}

static ota_img_status_t check_slot(const boot_port_t *p, uint32_t slot, const char *name,
                                   ota_image_header_t *hdr)
{
    ota_img_status_t st = ota_image_check_slot(p->ext, slot, hdr, s_buf, sizeof s_buf);
    if (st == OTA_IMG_OK) {
        blog(p, "%s slot: v%v, %u bytes, ok", name, hdr->fw_version, hdr->image_size);
    } else {
        blog(p, "%s slot: %s", name, ota_img_status_str(st));
    }
    return st;
}

/* Restore from the backup slot, or the factory slot if the backup is bad. */
static boot_result_t roll_back(const boot_port_t *p)
{
    static const uint32_t slots[2] = { EXT_SLOT_BACKUP, EXT_SLOT_FACTORY };
    static const char *const names[2] = { "backup", "factory" };
    ota_image_header_t hdr;

    for (int i = 0; i < 2; i++) {
        if (check_slot(p, slots[i], names[i], &hdr) != OTA_IMG_OK) continue;
        if (!install_from(p, slots[i], &hdr)) continue;
        s_meta.trial_boots = 0;
        s_meta.pending_version = 0;
        s_meta.last_result = OTA_RES_ROLLED_BACK;
        save_meta(p, OTA_STATE_IDLE);
        blog(p, "restored v%v from %s slot", hdr.fw_version, names[i]);
        return BOOT_JUMP;
    }
    blog(p, "no image to restore");
    s_meta.last_result = OTA_RES_NO_BACKUP;
    save_meta(p, OTA_STATE_IDLE);
    return boot_app_valid(p) ? BOOT_JUMP : BOOT_NO_APP;
}

/* ------------------------------------------------------------------------ */

boot_result_t boot_run(const boot_port_t *p)
{
    ota_image_header_t hdr;

    blog(p, "Stepwhere bootloader v%s", BOOT_VERSION_STR);

    if (!p->ext) {
        blog(p, "external flash not available, OTA disabled");
        return boot_app_valid(p) ? BOOT_JUMP : BOOT_NO_APP;
    }

    if (ota_meta_load(p->ext, &s_meta)) {
        blog(p, "state %s, installed v%v, seq %u", ota_state_str(s_meta.state),
             s_meta.installed_version, s_meta.seq);
    } else {
        blog(p, "no metadata (first boot)");
    }

    for (int step = 0; step < MAX_STEPS; step++) {
        switch (s_meta.state) {
        case OTA_STATE_IDLE:
            if (boot_app_valid(p)) {
                return BOOT_JUMP;
            }
            blog(p, "application missing or damaged");
            if (!save_meta(p, OTA_STATE_ROLLING_BACK)) return BOOT_NO_APP;
            break;

        case OTA_STATE_PENDING:
            if (check_slot(p, EXT_SLOT_DOWNLOAD, "download", &hdr) != OTA_IMG_OK) {
                s_meta.last_result = OTA_RES_BAD_IMAGE;
                s_meta.pending_version = 0;
                if (!save_meta(p, OTA_STATE_IDLE)) return boot_app_valid(p) ? BOOT_JUMP : BOOT_NO_APP;
                break;
            }
            if (!save_meta(p, OTA_STATE_BACKING_UP)) return boot_app_valid(p) ? BOOT_JUMP : BOOT_NO_APP;
            break;

        case OTA_STATE_BACKING_UP:
            if (boot_app_valid(p) && !backup_app(p)) {
                s_meta.last_result = OTA_RES_INSTALL_FAILED;
                if (!save_meta(p, OTA_STATE_IDLE)) return BOOT_JUMP;
                break;   /* old app untouched, keep running it */
            }
            if (!save_meta(p, OTA_STATE_INSTALLING)) return boot_app_valid(p) ? BOOT_JUMP : BOOT_NO_APP;
            break;

        case OTA_STATE_INSTALLING:
            if (check_slot(p, EXT_SLOT_DOWNLOAD, "download", &hdr) != OTA_IMG_OK ||
                !install_from(p, EXT_SLOT_DOWNLOAD, &hdr)) {
                s_meta.last_result = OTA_RES_INSTALL_FAILED;
                if (!save_meta(p, OTA_STATE_ROLLING_BACK)) return BOOT_NO_APP;
                break;
            }
            s_meta.trial_boots = 1;
            s_meta.pending_version = 0;
            s_meta.last_result = OTA_RES_INSTALLED;
            if (!save_meta(p, OTA_STATE_TRIAL)) return BOOT_JUMP;
            blog(p, "installed v%v, trial boot 1 of %u", hdr.fw_version, (uint32_t)OTA_MAX_TRIAL_BOOTS);
            return BOOT_JUMP;

        case OTA_STATE_TRIAL:
            if (s_meta.trial_boots >= OTA_MAX_TRIAL_BOOTS || !boot_app_valid(p)) {
                blog(p, "new app not confirmed after %u boots, rolling back", s_meta.trial_boots);
                if (!save_meta(p, OTA_STATE_ROLLING_BACK)) return BOOT_NO_APP;
                break;
            }
            s_meta.trial_boots++;
            save_meta(p, OTA_STATE_TRIAL);
            blog(p, "trial boot %u of %u", s_meta.trial_boots, (uint32_t)OTA_MAX_TRIAL_BOOTS);
            return BOOT_JUMP;

        case OTA_STATE_ROLLING_BACK:
            return roll_back(p);

        default:
            blog(p, "unknown state %u, treating as IDLE", s_meta.state);
            s_meta.state = OTA_STATE_IDLE;
            break;
        }
    }
    return boot_app_valid(p) ? BOOT_JUMP : BOOT_NO_APP;
}
