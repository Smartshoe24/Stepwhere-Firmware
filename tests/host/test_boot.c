/*
 * test_boot.c - Host tests for the bootloader state machine (boot_core.c),
 * image checks, metadata and SHA-256.
 *
 * The MCU application flash and the W25Q128 are simulated. Power cuts are
 * injected at every single erase/program operation of an update and of a
 * rollback; after each cut the device must still end up with a complete,
 * bootable image.
 *
 * Optional argument: path to a .ota file made by tools/ota_tool.py, checked
 * with the C image code (format contract between Python and C).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_core.h"
#include "fake_w25q.h"
#include "ota_crc32.h"
#include "ota_image.h"
#include "ota_layout.h"
#include "ota_meta.h"
#include "../../tools/dev_keys/ota_public_key.h"   /* dev key: test images are dev-signed */
#include "ota_sha256.h"
#include "ota_uecc.h"
#include "w25q.h"

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ------------------------- simulated MCU flash -------------------------- */

static uint8_t g_mcu[APP_MAX_SIZE];
static int g_mcu_fail_program;   /* force program errors */
static int g_verbose;

static int sim_erase_page(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr < APP_ADDR || addr >= APP_ADDR + APP_MAX_SIZE || addr % MCU_FLASH_PAGE_SIZE) return -1;
    uint8_t *pg = g_mcu + (addr - APP_ADDR);
    if (cut_now()) {
        memset(pg, 0x00, MCU_FLASH_PAGE_SIZE / 3);   /* half-erased page */
        cut_fire();
    }
    memset(pg, 0xFF, MCU_FLASH_PAGE_SIZE);
    return 0;
}

static int sim_program(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    if (g_mcu_fail_program) return -1;
    if (addr < APP_ADDR || (addr & 7u) || (len & 7u) || addr + len > APP_ADDR + APP_MAX_SIZE) return -1;
    uint8_t *dst = g_mcu + (addr - APP_ADDR);
    for (uint32_t i = 0; i < len; i++) {
        if (dst[i] != 0xFF) return -1;               /* like PROGERR on real flash */
    }
    if (cut_now()) {
        memcpy(dst, data, (len / 2) & ~7u);
        cut_fire();
    }
    memcpy(dst, data, len);
    return 0;
}

static void sim_log(void *ctx, const char *line)
{
    (void)ctx;
    if (g_verbose) printf("    %s\n", line);
}

static w25q_t g_dev;

/* Signing keys used by the tests (generated at start). */
static uint8_t g_priv[32], g_pub[64];        /* the device's trusted key */
static uint8_t g_other_priv[32], g_other_pub[64];
static const uint8_t *g_pubkey = g_pub;      /* key given to the bootloader */

static int g_wdg_starts, g_resets;
static void sim_wdg_start(void *ctx) { (void)ctx; g_wdg_starts++; }
static void sim_wdg_kick(void *ctx) { (void)ctx; }

static int rng(uint8_t *dest, unsigned size)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t n = f ? fread(dest, 1, size, f) : 0;
    if (f) fclose(f);
    return n == size;
}

/* One power-on of the device, following BOOT_RESET restarts (max 3). */
static boot_result_t do_boot(void)
{
    boot_result_t r = BOOT_RESET;
    for (int i = 0; i < 4 && r == BOOT_RESET; i++) {
        fake_power_cycle();
        boot_port_t port = {
            .app_mem = g_mcu, .erase_page = sim_erase_page, .program = sim_program,
            .log = sim_log, .pubkey = g_pubkey,
            .wdg_start = sim_wdg_start, .wdg_kick = sim_wdg_kick,
        };
        if (w25q_init(&g_dev, &FAKE_BUS) == W25Q_OK) port.ext = &g_dev;
        r = boot_run(&port);
        if (r == BOOT_RESET) g_resets++;
    }
    return r;
}

/* A single boot_run() without following restarts. */
static boot_result_t boot_once(void)
{
    fake_power_cycle();
    boot_port_t port = {
        .app_mem = g_mcu, .erase_page = sim_erase_page, .program = sim_program,
        .log = sim_log, .pubkey = g_pubkey,
        .wdg_start = sim_wdg_start, .wdg_kick = sim_wdg_kick,
    };
    if (w25q_init(&g_dev, &FAKE_BUS) == W25Q_OK) port.ext = &g_dev;
    return boot_run(&port);
}

/* Boot with a power cut at the n-th mutating operation.
 * Returns 1 if the cut happened, 0 if the boot finished first. */
static int boot_with_cut(long n, boot_result_t *res)
{
    cut_countdown = n;
    if (setjmp(cut_env)) {
        cut_countdown = 0;
        return 1;
    }
    *res = do_boot();
    cut_countdown = 0;
    return 0;
}

static void save_with_cut(long n, ota_boot_meta_t *m)
{
    cut_countdown = n;
    if (setjmp(cut_env) == 0) {
        ota_meta_save(&g_dev, m);
    }
    cut_countdown = 0;
}

/* ------------------------------ helpers --------------------------------- */

typedef struct {
    uint8_t  bytes[APP_MAX_SIZE];
    uint32_t size;
    uint32_t version;
} app_t;

static app_t APP_V1, APP_V2;

static void make_app(app_t *a, uint32_t size, uint32_t version, uint32_t seed)
{
    uint32_t sp = 0x20009000u, reset = APP_ADDR + 0x101u;
    a->size = size;
    a->version = version;
    memcpy(a->bytes, &sp, 4);
    memcpy(a->bytes + 4, &reset, 4);
    for (uint32_t i = 8; i < size; i++) {
        seed = seed * 1103515245u + 12345u;
        a->bytes[i] = (uint8_t)(seed >> 16);
    }
}

static void sign_header(ota_image_header_t *h, const uint8_t *priv)
{
    uint8_t digest[32];
    h->flags |= OTA_FLAG_SIGNED;
    ota_sha256(h, OTA_HDR_SIGNED_LEN, digest);
    if (!uECC_sign(priv, digest, sizeof digest, h->signature, uECC_secp256r1())) {
        printf("uECC_sign failed\n");
        exit(2);
    }
    ota_image_header_seal(h);
}

static void build_header(ota_image_header_t *h, const app_t *a)
{
    memset(h, 0, sizeof *h);
    h->magic = OTA_HDR_MAGIC;
    h->hdr_version = OTA_HDR_VERSION;
    h->hdr_size = OTA_HDR_SIZE;
    h->fw_version = a->version;
    h->image_size = a->size;
    h->load_addr = APP_ADDR;
    h->hw_id = OTA_HW_ID_GSM_PCB_R1;
    ota_sha256(a->bytes, a->size, h->payload_sha256);
    sign_header(h, g_priv);
}

static void put_image(uint32_t slot, const app_t *a)
{
    ota_image_header_t h;
    build_header(&h, a);
    memset(F.mem + slot, 0xFF, EXT_SLOT_SIZE);
    memcpy(F.mem + slot, &h, sizeof h);
    memcpy(F.mem + slot + OTA_HDR_SIZE, a->bytes, a->size);
}

static void install_direct(const app_t *a)
{
    memset(g_mcu, 0xFF, sizeof g_mcu);
    memcpy(g_mcu, a->bytes, a->size);
}

static int mcu_holds(const app_t *a)
{
    if (memcmp(g_mcu, a->bytes, a->size) != 0) return 0;
    for (uint32_t i = a->size; i < APP_MAX_SIZE; i++) {
        if (g_mcu[i] != 0xFF) return 0;
    }
    return 1;
}

static ota_boot_meta_t meta(void)
{
    ota_boot_meta_t m;
    fake_power_cycle();
    w25q_init(&g_dev, &FAKE_BUS);
    ota_meta_load(&g_dev, &m);
    return m;
}

static void set_meta(uint32_t state, const app_t *installed, uint32_t trial)
{
    ota_boot_meta_t m;
    fake_power_cycle();
    w25q_init(&g_dev, &FAKE_BUS);
    ota_meta_load(&g_dev, &m);
    m.state = state;
    m.trial_boots = trial;
    if (installed) {
        m.installed_version = installed->version;
        m.installed_size = installed->size;
        ota_sha256(installed->bytes, installed->size, m.installed_sha256);
    }
    ota_meta_save(&g_dev, &m);
}

/* Device running v1 (confirmed), v2 downloaded and marked PENDING. */
static void scenario_update_ready(void)
{
    fake_new_chip();
    install_direct(&APP_V1);
    set_meta(OTA_STATE_IDLE, &APP_V1, 0);
    put_image(EXT_SLOT_DOWNLOAD, &APP_V2);
    fake_power_cycle();
    w25q_init(&g_dev, &FAKE_BUS);
    ota_meta_set_pending(&g_dev, APP_V2.version);
}

/* v2 installed but never confirmed: the next boot must roll back to v1. */
static void scenario_rollback_due(void)
{
    scenario_update_ready();
    do_boot();                              /* installs v2, trial 1 */
    do_boot();                              /* trial 2 */
    do_boot();                              /* trial 3 */
}

/* -------------------------------- tests --------------------------------- */

static void test_sha256_vectors(void)
{
    uint8_t d[32];
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    static const uint8_t empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
    static const uint8_t two_blocks[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1 };
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    ota_sha256("abc", 3, d);
    CHECK(memcmp(d, abc, 32) == 0);
    ota_sha256("", 0, d);
    CHECK(memcmp(d, empty, 32) == 0);
    ota_sha256(m448, strlen(m448), d);
    CHECK(memcmp(d, two_blocks, 32) == 0);

    /* Incremental updates in odd chunk sizes give the same result. */
    ota_sha256_t c;
    ota_sha256_init(&c);
    for (size_t i = 0; i < strlen(m448); i += 7) {
        size_t n = strlen(m448) - i < 7 ? strlen(m448) - i : 7;
        ota_sha256_update(&c, m448 + i, n);
    }
    ota_sha256_final(&c, d);
    CHECK(memcmp(d, two_blocks, 32) == 0);
}

static void test_crc32(void)
{
    CHECK(ota_crc32("123456789", 9) == 0xCBF43926UL);    /* standard check value */
    CHECK(ota_crc32_update(ota_crc32("1234", 4), "56789", 5) == 0xCBF43926UL);
}

static void test_meta_ab(void)
{
    ota_boot_meta_t m;
    fake_new_chip();
    w25q_init(&g_dev, &FAKE_BUS);
    CHECK(!ota_meta_load(&g_dev, &m) && m.state == OTA_STATE_IDLE);

    for (uint32_t i = 1; i <= 5; i++) {
        m.installed_version = i;
        CHECK(ota_meta_save(&g_dev, &m) == W25Q_OK);
    }
    CHECK(ota_meta_load(&g_dev, &m) && m.installed_version == 5 && m.seq == 5);

    /* Corrupt the newest copy: the previous one must be used. */
    uint32_t newest = (m.seq & 1u) ? EXT_META_A_ADDR : EXT_META_B_ADDR;
    F.mem[newest + 20] ^= 0xFF;
    CHECK(ota_meta_load(&g_dev, &m) && m.installed_version == 4);
    /* The next save must go to the damaged sector, keeping the good one. */
    m.installed_version = 6;
    CHECK(ota_meta_save(&g_dev, &m) == W25Q_OK);
    CHECK(ota_meta_load(&g_dev, &m) && m.installed_version == 6);
    CHECK(F.violations == 0);
}

static void test_meta_power_cut(void)
{
    /* A cut at any point of a metadata save keeps either old or new. */
    for (long n = 1; n <= 40; n++) {
        ota_boot_meta_t m;
        fake_new_chip();
        w25q_init(&g_dev, &FAKE_BUS);
        m.installed_version = 1;
        ota_meta_load(&g_dev, &m);
        m.installed_version = 1;
        ota_meta_save(&g_dev, &m);

        m.installed_version = 2;
        save_with_cut(n, &m);
        CHECK(meta().installed_version == 1 || meta().installed_version == 2);
        CHECK(meta().magic == OTA_META_MAGIC);
    }
}

static void test_first_boot_no_metadata(void)
{
    fake_new_chip();
    install_direct(&APP_V1);
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1));
    CHECK(F.n_sector_erase == 0 && F.n_program == 0);   /* normal boot writes nothing */
}

static void test_no_app_no_backup(void)
{
    fake_new_chip();
    memset(g_mcu, 0xFF, sizeof g_mcu);
    CHECK(do_boot() == BOOT_NO_APP);
    CHECK(meta().last_result == OTA_RES_NO_BACKUP);
}

static void test_ext_flash_missing(void)
{
    fake_new_chip();
    F.stuck_busy = 1;                      /* chip not answering */
    install_direct(&APP_V1);
    CHECK(do_boot() == BOOT_JUMP);
    memset(g_mcu, 0xFF, sizeof g_mcu);
    CHECK(do_boot() == BOOT_NO_APP);
}

static void test_update_and_confirm(void)
{
    ota_image_header_t h;
    scenario_update_ready();
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V2));
    ota_boot_meta_t m = meta();
    CHECK(m.state == OTA_STATE_TRIAL && m.trial_boots == 1);
    CHECK(m.installed_version == APP_V2.version && m.installed_size == APP_V2.size);
    CHECK(m.last_result == OTA_RES_INSTALLED);

    /* Backup slot now holds v1 as a valid image. */
    uint8_t buf[512];
    CHECK(ota_image_check_slot(&g_dev, EXT_SLOT_BACKUP, &h, buf, sizeof buf, NULL) == OTA_IMG_OK);
    CHECK(h.fw_version == APP_V1.version && h.image_size == APP_V1.size);

    /* App confirms; later boots stay on v2 and write nothing. */
    CHECK(ota_meta_confirm(&g_dev) == W25Q_OK);
    CHECK(meta().state == OTA_STATE_IDLE && meta().last_result == OTA_RES_CONFIRMED);
    int writes = F.n_program;
    for (int i = 0; i < 5; i++) CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V2) && F.n_program == writes);
    CHECK(F.violations == 0);
}

static void test_bad_download_kept_old(void)
{
    scenario_update_ready();
    F.mem[EXT_SLOT_DOWNLOAD + OTA_HDR_SIZE + 5000] ^= 0x01;   /* corrupt payload */
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1));
    CHECK(meta().state == OTA_STATE_IDLE && meta().last_result == OTA_RES_BAD_IMAGE);
}

static void test_wrong_board_rejected(void)
{
    ota_image_header_t h;
    scenario_update_ready();
    memcpy(&h, F.mem + EXT_SLOT_DOWNLOAD, sizeof h);
    h.hw_id = 0x00020001u;
    sign_header(&h, g_priv);           /* validly signed, but for another board */
    memcpy(F.mem + EXT_SLOT_DOWNLOAD, &h, sizeof h);
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1) && meta().last_result == OTA_RES_BAD_IMAGE);
}

static void test_wrong_link_address_rejected(void)
{
    app_t *bad = malloc(sizeof *bad);
    *bad = APP_V2;
    uint32_t reset = 0x080001C1u;          /* linked at 0x08000000 */
    memcpy(bad->bytes + 4, &reset, 4);
    scenario_update_ready();
    put_image(EXT_SLOT_DOWNLOAD, bad);
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1) && meta().last_result == OTA_RES_BAD_IMAGE);
    free(bad);
}

static void test_rollback_after_unconfirmed_trials(void)
{
    scenario_update_ready();
    CHECK(do_boot() == BOOT_JUMP && meta().trial_boots == 1);
    CHECK(do_boot() == BOOT_JUMP && meta().trial_boots == 2);
    CHECK(do_boot() == BOOT_JUMP && meta().trial_boots == 3);
    CHECK(mcu_holds(&APP_V2));
    CHECK(do_boot() == BOOT_JUMP);          /* 4th boot: give up on v2 */
    CHECK(mcu_holds(&APP_V1));
    ota_boot_meta_t m = meta();
    CHECK(m.state == OTA_STATE_IDLE && m.last_result == OTA_RES_ROLLED_BACK);
    CHECK(m.installed_version == APP_V1.version);
    CHECK(F.violations == 0);
}

static void test_install_failure_rolls_back(void)
{
    scenario_update_ready();
    g_mcu_fail_program = 1;                 /* internal flash refuses to program */
    CHECK(do_boot() == BOOT_NO_APP);        /* install and rollback both fail */
    g_mcu_fail_program = 0;
    CHECK(meta().last_result == OTA_RES_NO_BACKUP);
    CHECK(do_boot() == BOOT_JUMP);          /* next boot: flash works again, restore */
    CHECK(mcu_holds(&APP_V1) && meta().state == OTA_STATE_IDLE);
}

static void test_damaged_app_recovered_from_backup(void)
{
    scenario_update_ready();
    do_boot();                              /* v2 installed, v1 in backup */
    ota_meta_confirm(&g_dev);
    memset(g_mcu, 0xFF, 4096);              /* app vector table destroyed */
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1));              /* backup is v1 */
}

static void test_factory_slot_used_when_backup_bad(void)
{
    scenario_rollback_due();
    put_image(EXT_SLOT_FACTORY, &APP_V1);
    memset(F.mem + EXT_SLOT_BACKUP, 0xFF, 512);   /* backup header gone */
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1) && meta().last_result == OTA_RES_ROLLED_BACK);
}

/* Power cut at every mutating operation of a full update. */
static void test_power_cut_sweep_update(void)
{
    long n, cuts = 0;
    for (n = 1;; n++) {
        boot_result_t r = BOOT_NO_APP;
        scenario_update_ready();
        if (!boot_with_cut(n, &r)) break;   /* update finished before cut n */
        cuts++;
        r = do_boot();                      /* next power-on, no cut */
        int ok = r == BOOT_JUMP && mcu_holds(&APP_V2) && meta().state == OTA_STATE_TRIAL;
        if (!ok) {
            printf("  update cut at op %ld: result %d, state %s, holds v2 %d v1 %d\n", n, r,
                   ota_state_str(meta().state), mcu_holds(&APP_V2), mcu_holds(&APP_V1));
        }
        CHECK(ok);
    }
    printf("  update: %ld power-cut points tested\n", cuts);
    CHECK(cuts > 300);
}

/* Power cut at every mutating operation of a rollback. */
static void test_power_cut_sweep_rollback(void)
{
    long n, cuts = 0;
    for (n = 1;; n++) {
        boot_result_t r = BOOT_NO_APP;
        scenario_rollback_due();
        if (!boot_with_cut(n, &r)) break;
        cuts++;
        r = do_boot();
        int ok = r == BOOT_JUMP && mcu_holds(&APP_V1) && meta().state == OTA_STATE_IDLE;
        if (!ok) {
            printf("  rollback cut at op %ld: result %d, state %s\n", n, r, ota_state_str(meta().state));
        }
        CHECK(ok);
    }
    printf("  rollback: %ld power-cut points tested\n", cuts);
    CHECK(cuts > 50);
}

/* Two cuts in a row: cut during the first boot and again during the retry. */
static void test_double_power_cut(void)
{
    long tested = 0;
    for (long n = 1; n < 520; n += 13) {
        for (long k = 1; k < 520; k += 29) {
            boot_result_t r = BOOT_NO_APP;
            scenario_update_ready();
            if (!boot_with_cut(n, &r)) continue;
            boot_with_cut(k, &r);
            r = do_boot();
            CHECK(r == BOOT_JUMP && mcu_holds(&APP_V2));
            tested++;
        }
    }
    printf("  double cuts: %ld combinations tested\n", tested);
}


static void test_unsigned_rejected(void)
{
    ota_image_header_t h;
    scenario_update_ready();
    memcpy(&h, F.mem + EXT_SLOT_DOWNLOAD, sizeof h);
    h.flags = 0;
    memset(h.signature, 0, sizeof h.signature);
    ota_image_header_seal(&h);
    memcpy(F.mem + EXT_SLOT_DOWNLOAD, &h, sizeof h);
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1) && meta().last_result == OTA_RES_BAD_IMAGE);
}

static void test_wrong_key_rejected(void)
{
    ota_image_header_t h;
    scenario_update_ready();
    memcpy(&h, F.mem + EXT_SLOT_DOWNLOAD, sizeof h);
    sign_header(&h, g_other_priv);
    memcpy(F.mem + EXT_SLOT_DOWNLOAD, &h, sizeof h);
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1) && meta().last_result == OTA_RES_BAD_IMAGE);
}

static void test_tampered_header_rejected(void)
{
    /* Attacker changes the version and fixes the CRC, but cannot re-sign. */
    ota_image_header_t h;
    scenario_update_ready();
    memcpy(&h, F.mem + EXT_SLOT_DOWNLOAD, sizeof h);
    h.fw_version = OTA_VERSION(9, 9, 9);
    ota_image_header_seal(&h);
    memcpy(F.mem + EXT_SLOT_DOWNLOAD, &h, sizeof h);
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(mcu_holds(&APP_V1) && meta().last_result == OTA_RES_BAD_IMAGE);
}

static void test_signature_api(void)
{
    ota_image_header_t h;
    build_header(&h, &APP_V2);
    CHECK(ota_image_check_signature(&h, g_pub) == OTA_IMG_OK);
    CHECK(ota_image_check_signature(&h, g_other_pub) == OTA_IMG_ERR_SIGNATURE);
    h.signature[10] ^= 1;
    CHECK(ota_image_check_signature(&h, g_pub) == OTA_IMG_ERR_SIGNATURE);
    h.flags = 0;
    CHECK(ota_image_check_signature(&h, g_pub) == OTA_IMG_ERR_UNSIGNED);
}

static void test_watchdog_usage(void)
{
    /* Normal boot: no watchdog, no restart. */
    fake_new_chip();
    install_direct(&APP_V1);
    set_meta(OTA_STATE_IDLE, &APP_V1, 0);
    g_wdg_starts = 0;
    CHECK(boot_once() == BOOT_JUMP && g_wdg_starts == 0);

    /* Install: watchdog on, jump into TRIAL with it running (no restart). */
    scenario_update_ready();
    g_wdg_starts = 0;
    CHECK(boot_once() == BOOT_JUMP && g_wdg_starts == 1);
    CHECK(meta().state == OTA_STATE_TRIAL);

    /* After confirm: clean boot without watchdog. */
    ota_meta_confirm(&g_dev);
    g_wdg_starts = 0;
    CHECK(boot_once() == BOOT_JUMP && g_wdg_starts == 0);

    /* Rejected download: watchdog was needed, so restart once. */
    scenario_update_ready();
    F.mem[EXT_SLOT_DOWNLOAD + OTA_HDR_SIZE + 100] ^= 1;
    g_wdg_starts = 0;
    CHECK(boot_once() == BOOT_RESET && g_wdg_starts == 1);
    CHECK(boot_once() == BOOT_JUMP && g_wdg_starts == 1);      /* no new start */

    /* Rollback: restart once, then clean boot of the old app. */
    scenario_rollback_due();
    CHECK(boot_once() == BOOT_RESET && mcu_holds(&APP_V1));
    g_wdg_starts = 0;
    CHECK(boot_once() == BOOT_JUMP && g_wdg_starts == 0);
}

static void test_python_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t buf[256];
    ota_image_header_t h;

    CHECK(f != NULL);
    if (!f) return;
    fake_new_chip();
    len = (long)fread(F.mem + EXT_SLOT_DOWNLOAD, 1, EXT_SLOT_SIZE, f);
    fclose(f);
    CHECK(len > OTA_HDR_SIZE);
    w25q_init(&g_dev, &FAKE_BUS);
    CHECK(ota_image_check_slot(&g_dev, EXT_SLOT_DOWNLOAD, &h, buf, sizeof buf, OTA_PUBLIC_KEY) == OTA_IMG_OK);
    CHECK(h.fw_version == OTA_VERSION(1, 3, 0));
    CHECK((long)(h.image_size + OTA_HDR_SIZE) == len);

    /* And the bootloader (built with the dev key) installs it. */
    g_pubkey = OTA_PUBLIC_KEY;
    install_direct(&APP_V1);
    ota_meta_set_pending(&g_dev, h.fw_version);
    CHECK(do_boot() == BOOT_JUMP);
    CHECK(memcmp(g_mcu, F.mem + EXT_SLOT_DOWNLOAD + OTA_HDR_SIZE, h.image_size) == 0);
    g_pubkey = g_pub;
    printf("  python-made, dev-key-signed image accepted and installed (%u bytes)\n", (unsigned)h.image_size);
}

int main(int argc, char **argv)
{
    g_verbose = getenv("VERBOSE") != NULL;
    uECC_set_rng(rng);
    if (!uECC_make_key(g_pub, g_priv, uECC_secp256r1()) ||
        !uECC_make_key(g_other_pub, g_other_priv, uECC_secp256r1())) {
        printf("uECC_make_key failed\n");
        return 2;
    }
    fake_new_chip();
    make_app(&APP_V1, 73 * 1024 + 4, OTA_VERSION(1, 2, 0), 1);
    make_app(&APP_V2, 76 * 1024 + 100, OTA_VERSION(1, 3, 0), 2);

    test_sha256_vectors();
    test_crc32();
    test_meta_ab();
    test_meta_power_cut();
    test_first_boot_no_metadata();
    test_no_app_no_backup();
    test_ext_flash_missing();
    test_update_and_confirm();
    test_bad_download_kept_old();
    test_wrong_board_rejected();
    test_wrong_link_address_rejected();
    test_rollback_after_unconfirmed_trials();
    test_install_failure_rolls_back();
    test_damaged_app_recovered_from_backup();
    test_factory_slot_used_when_backup_bad();
    test_unsigned_rejected();
    test_wrong_key_rejected();
    test_tampered_header_rejected();
    test_signature_api();
    test_watchdog_usage();
    test_power_cut_sweep_update();
    test_power_cut_sweep_rollback();
    test_double_power_cut();
    if (argc > 1) test_python_image(argv[1]);

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
