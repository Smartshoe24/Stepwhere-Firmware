#include "ota_meta.h"

#include <string.h>

#include "ota_crc32.h"

#define META_CRC_LEN (sizeof(ota_boot_meta_t) - sizeof(uint32_t))

static bool read_record(w25q_t *dev, uint32_t addr, ota_boot_meta_t *m)
{
    if (w25q_read(dev, addr, m, sizeof *m) != W25Q_OK) {
        return false;
    }
    return m->magic == OTA_META_MAGIC && m->meta_version == OTA_META_VERSION &&
           m->record_size == sizeof *m && ota_crc32(m, META_CRC_LEN) == m->crc32;
}

/* Returns the address of the newest valid record, or 0xFFFFFFFF if none. */
static uint32_t find_newest(w25q_t *dev, ota_boot_meta_t *out)
{
    ota_boot_meta_t a, b;
    bool va = read_record(dev, EXT_META_A_ADDR, &a);
    bool vb = read_record(dev, EXT_META_B_ADDR, &b);

    if (va && vb) {
        /* Signed difference handles sequence wrap-around. */
        vb = (int32_t)(b.seq - a.seq) > 0;
        va = !vb;
    }
    if (va) { *out = a; return EXT_META_A_ADDR; }
    if (vb) { *out = b; return EXT_META_B_ADDR; }
    return 0xFFFFFFFFUL;
}

bool ota_meta_load(w25q_t *dev, ota_boot_meta_t *m)
{
    if (find_newest(dev, m) != 0xFFFFFFFFUL) {
        return true;
    }
    memset(m, 0, sizeof *m);
    m->state = OTA_STATE_IDLE;
    return false;
}

w25q_status_t ota_meta_save(w25q_t *dev, ota_boot_meta_t *m)
{
    ota_boot_meta_t newest;
    uint32_t newest_addr = find_newest(dev, &newest);
    uint32_t target;
    w25q_status_t st;

    if (newest_addr == 0xFFFFFFFFUL) {
        target = EXT_META_A_ADDR;
        m->seq = 1;
    } else {
        target = newest_addr == EXT_META_A_ADDR ? EXT_META_B_ADDR : EXT_META_A_ADDR;
        m->seq = newest.seq + 1u;
    }
    m->magic = OTA_META_MAGIC;
    m->meta_version = OTA_META_VERSION;
    m->record_size = (uint16_t)sizeof *m;
    m->crc32 = ota_crc32(m, META_CRC_LEN);

    st = w25q_erase_sector(dev, target);
    if (st != W25Q_OK) {
        return st;
    }
    return w25q_write_verify(dev, target, m, sizeof *m);
}

w25q_status_t ota_meta_confirm(w25q_t *dev)
{
    ota_boot_meta_t m;
    if (!ota_meta_load(dev, &m) || m.state != OTA_STATE_TRIAL) {
        return W25Q_OK;
    }
    m.state = OTA_STATE_IDLE;
    m.trial_boots = 0;
    m.last_result = OTA_RES_CONFIRMED;
    return ota_meta_save(dev, &m);
}

w25q_status_t ota_meta_set_pending(w25q_t *dev, uint32_t version)
{
    ota_boot_meta_t m;
    ota_meta_load(dev, &m);
    m.state = OTA_STATE_PENDING;
    m.pending_version = version;
    return ota_meta_save(dev, &m);
}

const char *ota_state_str(uint32_t state)
{
    switch (state) {
    case OTA_STATE_IDLE:         return "IDLE";
    case OTA_STATE_PENDING:      return "PENDING";
    case OTA_STATE_BACKING_UP:   return "BACKING_UP";
    case OTA_STATE_INSTALLING:   return "INSTALLING";
    case OTA_STATE_TRIAL:        return "TRIAL";
    case OTA_STATE_ROLLING_BACK: return "ROLLING_BACK";
    default:                     return "UNKNOWN";
    }
}
