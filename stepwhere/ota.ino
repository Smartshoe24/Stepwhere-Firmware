// =============================================================================
//  OTA CLIENT  (docs/OTA_DESIGN.md, docs/DECISIONS.md)
//
//  - At boot: open the W25Q128, read the boot metadata and the bootloader info.
//  - Trial boot (new firmware just installed): the first upload the server
//    accepts confirms the firmware (ota_meta_confirm). The watchdog the
//    bootloader started is fed from service_background(). Not confirmed after
//    30 min with a data connection: restart (uses up one of 3 trial boots).
//  - Once a day (and on 'o' from the debug UART): fetch
//      /fw/devices/<IMEI>.json, else /fw/manifest.json
//    from the ingest server. A newer, matching release is downloaded in 512 B
//    byte ranges as hex text (?enc=hex) between uploads, checked (SHA-256 +
//    signature), and, with
//    enough battery, marked PENDING; the board then resets into the bootloader.
//    A version whose image failed the check is not downloaded again until
//    the next restart.
//  - Every upload carries a "fw" status block.
//
//  Modem transport: AT+HTTPACTION=99 with our own GET request (Range header),
//  the response arrives behind +HTTPRECV: and ends with +HTTPCLOSED.
// =============================================================================

#define OTA_HOST "ingest.stepwhere.in"
#define OTA_PORT 80
#define OTA_BASE "/fw/"
#define OTA_FIRST_CHECK_MS (3UL * 60UL * 1000UL)       // after boot
#define OTA_CHECK_INTERVAL_MS (24UL * 60UL * 60UL * 1000UL)
#define OTA_RETRY_MS (30UL * 60UL * 1000UL)            // after a failed check
#define OTA_CHUNK 4096u                                // one W25Q sector
// Image bytes per HTTP request. The image is fetched with ?enc=hex: the N706B
// passes text replies but drops binary ones (live probe, 2026-11-09). 512
// bytes = 1024 hex characters, + ~300 B headers/framing, close to the 1 KB
// UART RX buffer.
#define OTA_PART 512u
#define OTA_PART_RETRY_MS 3000UL                       // pause after a failed part
#define OTA_MAX_CHUNK_FAILS 4
#define OTA_MIN_BATTERY_V 3.70f                        // to mark an update PENDING
#define OTA_HTTP_TIMEOUT_MS 40000UL
// A trial firmware that has had a data connection this long without one
// accepted upload restarts itself; each restart uses one of the bootloader's 3
// trial boots, so a firmware that cannot reach the server rolls back on its
// own. Time without a connection (no coverage, low battery) does not count.
#define OTA_TRIAL_ONLINE_LIMIT_MS (30UL * 60UL * 1000UL)

// Flash pins: SPI1 PA7/PA6/PA5, /CS PC6, /WP PC7, /HOLD PB12 (docs D-07)
static W25QArduino g_ota_flash(PA7, PA6, PA5, PC6, PC7, PB12);

enum OtaPhase { OTA_PH_IDLE, OTA_PH_DOWNLOAD };

struct OtaClient {
  bool flash_ok = false;
  bool trial = false;          // booted as a new, unconfirmed firmware
  bool confirmed = false;
  uint32_t app_version = 0;    // parsed from APP_VERSION
  uint32_t boot_version = 0;   // from the bootloader info block, 0 = unknown
  char key_fp[9] = "--------";
  uint32_t meta_state = OTA_STATE_IDLE;
  uint32_t last_result = OTA_RES_NONE;
  uint32_t bad_version = 0;    // rolled back from this one: never take it again
  uint32_t rejected_version = 0;  // failed the image check: skip until restart
  uint32_t trial_online_ms = 0;   // time with a data connection while unconfirmed
  uint32_t trial_last_ms = 0;
  uint32_t next_check_at = OTA_FIRST_CHECK_MS;
  bool force_check = false;
  OtaPhase phase = OTA_PH_IDLE;
  uint32_t avail_version = 0;  // newest version seen in a manifest
  uint32_t dl_version = 0;
  uint32_t dl_size = 0;        // whole .ota (header + payload)
  uint32_t dl_offset = 0;
  uint8_t dl_fails = 0;
  uint32_t dl_next_at = 0;     // millis() before which no part is requested
  char dl_path[128] = "";
} g_ota;

#include "src/ota_util.h"

// Raw modem bytes for one HTTP exchange: 1 KB body (one hex part, or the
// manifest) + headers + URC framing.
static uint8_t g_ota_rx[2 * OTA_PART + 1024];
static uint8_t g_ota_rx_copy[200];   // raw start of the last reply, for the log
static uint8_t g_ota_rx_tail[120];   // raw end of the last reply, for the log
// Download probe ('p' on the debug UART): a few test GETs, raw replies logged.
static bool g_ota_probe = false;
static bool g_ota_dump_all = false;  // log every raw reply, not only bad ones
static const char *g_ota_step = "";  // last AT step reached, for the probe log

// ----------------------------------------------------------------------------

static void ota_version_str(uint32_t v, char *out, size_t n) {
  snprintf(out, n, "%u.%u.%u", (unsigned)((v >> 16) & 0xFF), (unsigned)((v >> 8) & 0xFF), (unsigned)(v & 0xFF));
}

static const char *ota_result_name(uint32_t r) {
  switch (r) {
    case OTA_RES_INSTALLED: return "installed";
    case OTA_RES_CONFIRMED: return "confirmed";
    case OTA_RES_BAD_IMAGE: return "rejected";
    case OTA_RES_INSTALL_FAILED: return "install_failed";
    case OTA_RES_ROLLED_BACK: return "rolled_back";
    case OTA_RES_NO_BACKUP: return "no_backup";
    default: return "none";
  }
}

static inline void ota_wdg_kick() { IWDG->KR = 0xAAAA; }  // harmless if not running

// ----------------------------------------------------------------------------

void ota_begin() {
  g_ota.app_version = ota_parse_version(APP_VERSION);

  const ota_boot_info_t *bi = (const ota_boot_info_t *)BOOT_INFO_ADDR;
  if (bi->magic == BOOT_INFO_MAGIC) {
    g_ota.boot_version = bi->boot_version;
    snprintf(g_ota.key_fp, sizeof g_ota.key_fp, "%02X%02X%02X%02X",
             bi->key_fp[0], bi->key_fp[1], bi->key_fp[2], bi->key_fp[3]);
  }

  g_ota.flash_ok = g_ota_flash.begin() == W25Q_OK;
  if (!g_ota.flash_ok) {
    ERR.println("[OTA] External flash not found - updates disabled.");
    return;
  }
  ota_boot_meta_t m;
  if (ota_meta_load(g_ota_flash.dev(), &m)) {
    g_ota.meta_state = m.state;
    g_ota.last_result = m.last_result;
    g_ota.trial = m.state == OTA_STATE_TRIAL;
    if (m.last_result == OTA_RES_ROLLED_BACK) {
      ota_image_header_t h;   // the image that failed is still in the download slot
      if (w25q_read(g_ota_flash.dev(), EXT_SLOT_DOWNLOAD, &h, sizeof h) == W25Q_OK &&
          h.magic == OTA_HDR_MAGIC) g_ota.bad_version = h.fw_version;
    }
  }
  char v[12], b[12];
  ota_version_str(g_ota.app_version, v, sizeof v);
  ota_version_str(g_ota.boot_version, b, sizeof b);
  INFO.print("[OTA] app ");
  INFO.print(v);
  INFO.print(", bootloader ");
  INFO.print(g_ota.boot_version ? b : "none");
  INFO.print(", key ");
  INFO.print(g_ota.key_fp);
  INFO.print(", state ");
  INFO.print(ota_state_str(g_ota.meta_state));
  INFO.print(", last ");
  INFO.println(ota_result_name(g_ota.last_result));
  if (g_ota.trial) INFO.println("[OTA] Trial boot: confirming after the first accepted upload.");
  g_ota.trial_last_ms = millis();
}

// Unconfirmed trial firmware: restart after OTA_TRIAL_ONLINE_LIMIT_MS online.
static void ota_trial_deadline() {
  if (!g_ota.trial || g_ota.confirmed) return;
  const uint32_t now = millis();
  if (g_modem.ready()) g_ota.trial_online_ms += now - g_ota.trial_last_ms;
  g_ota.trial_last_ms = now;
  if (g_ota.trial_online_ms < OTA_TRIAL_ONLINE_LIMIT_MS) return;
  ERR.println("[OTA] New firmware not confirmed after 30 min online - restarting (failed trial boot).");
  persist_steps();
  SerialDBG.flush();
  NVIC_SystemReset();
}

// Called from service_background(): watchdog, trial deadline, debug-UART commands.
void ota_service() {
  ota_wdg_kick();
  ota_trial_deadline();
  while (SerialDBG.available()) {
    int c = SerialDBG.read();
    if (c == 'o') {
      g_ota.force_check = true;
      INFO.println("[OTA] Update check requested.");
    } else if (c == 'p') {
      g_ota_probe = true;
      INFO.println("[PROBE] Download probe requested (runs between uploads).");
    } else if (c == 'O') {
      char a[12];
      ota_version_str(g_ota.avail_version, a, sizeof a);
      INFO.print("[OTA] state ");
      INFO.print(ota_state_str(g_ota.meta_state));
      INFO.print(", trial ");
      INFO.print(g_ota.trial ? (g_ota.confirmed ? "confirmed" : "yes") : "no");
      INFO.print(", available ");
      INFO.print(g_ota.avail_version ? a : "-");
      INFO.print(", download ");
      INFO.print(g_ota.dl_offset);
      INFO.print("/");
      INFO.println(g_ota.dl_size);
    }
  }
}

// Called after the server accepted an upload (HTTP 201).
void ota_on_upload_accepted() {
  if (!g_ota.trial || g_ota.confirmed || !g_ota.flash_ok) return;
  if (ota_meta_confirm(g_ota_flash.dev()) == W25Q_OK) {
    g_ota.confirmed = true;
    g_ota.meta_state = OTA_STATE_IDLE;
    g_ota.last_result = OTA_RES_CONFIRMED;
    INFO.println("[OTA] New firmware confirmed (first upload accepted).");
  } else {
    ERR.println("[OTA] Confirm failed: metadata write error.");
  }
}

// ,"fw":{...} for the upload payload.
void ota_append_status(JsonWriter &json) {
  char app[12], boot[12], avail[12];
  ota_version_str(g_ota.app_version, app, sizeof app);
  ota_version_str(g_ota.boot_version, boot, sizeof boot);
  ota_version_str(g_ota.avail_version, avail, sizeof avail);
  unsigned pct = g_ota.dl_size ? (unsigned)((uint64_t)g_ota.dl_offset * 100u / g_ota.dl_size) : 0u;
  json.format(",\"fw\":{\"app\":\"%s\",\"boot\":\"%s\",\"key\":\"%s\",\"ota\":\"%s\",\"last\":\"%s\","
              "\"avail\":\"%s\",\"dl\":%u}",
              app, g_ota.boot_version ? boot : "", g_ota.key_fp,
              g_ota.trial && !g_ota.confirmed ? "TRIAL" : ota_state_str(g_ota.meta_state),
              ota_result_name(g_ota.last_result), g_ota.avail_version ? avail : "", pct);
}

// ----------------------------------------------------------------------------
//  HTTP GET through the N706B
// ----------------------------------------------------------------------------

static void ota_print_escaped(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t ch = p[i];
    if (ch == '\r') INFO.print("\\r");
    else if (ch == '\n') INFO.print("\\n");
    else if (ch >= 32 && ch < 127) INFO.write(ch);
    else { char hx[5]; snprintf(hx, sizeof hx, "\\x%02X", ch); INFO.print(hx); }
  }
}

// GET path (Range if len > 0). On success body points into g_ota_rx.
// Returns the HTTP status, or 0 on a transport failure.
static int ota_http_get(const char *path, uint32_t from, uint32_t len,
                        const uint8_t **body, size_t *body_len) {
  char req[220], cmd[160];
  int n;
  *body = nullptr;
  *body_len = 0;
  // Bytes left over from an earlier, cut-off reply would be read as the start
  // of this one.
  while (SerialGSM.available()) SerialGSM.read();
  if (len) n = snprintf(req, sizeof req,
                        "GET %s HTTP/1.1\r\nHost: " OTA_HOST "\r\nRange: bytes=%lu-%lu\r\n"
                        "User-Agent: stepwhere/" APP_VERSION "\r\nConnection: close\r\n\r\n",
                        path, (unsigned long)from, (unsigned long)(from + len - 1));
  else n = snprintf(req, sizeof req,
                    "GET %s HTTP/1.1\r\nHost: " OTA_HOST "\r\n"
                    "User-Agent: stepwhere/" APP_VERSION "\r\nConnection: close\r\n\r\n", path);
  if (n <= 0 || n >= (int)sizeof req) return 0;

  snprintf(cmd, sizeof cmd, "AT+HTTPPARA=url,\"http://" OTA_HOST "%s\"", path);
  int status = 0;
  g_ota_step = "HTTPPARA url";
  if (gsm_send_at(cmd, "OK", AT_TIMEOUT) &&
      (g_ota_step = "HTTPPARA port", gsm_send_at("AT+HTTPPARA=port,80", "OK", AT_TIMEOUT)) &&
      (g_ota_step = "HTTPSETUP", gsm_send_at("AT+HTTPSETUP", "OK", AT_LONG_TIMEOUT))) {
    snprintf(cmd, sizeof cmd, "AT+HTTPACTION=99,%d", n);
    g_ota_step = "HTTPACTION prompt";
    if (gsm_send_at(cmd, ">", AT_LONG_TIMEOUT)) {
      g_ota_step = "reply";
      SerialGSM.write((const uint8_t *)req, (size_t)n);
      size_t got = 0;
      const uint32_t started = millis();
      uint32_t last = millis();
      const char *ended = "timeout";
      while (g_modem.rail_on && millis() - started < OTA_HTTP_TIMEOUT_MS) {
        while (SerialGSM.available() && got < sizeof g_ota_rx) {
          g_ota_rx[got++] = (uint8_t)SerialGSM.read();
          last = millis();
        }
        if (got >= 12 && ota_find(g_ota_rx, got, "+HTTPCLOSED", got > 600 ? got - 600 : 0) >= 0) {
          ended = "closed";
          break;
        }
        if (got >= sizeof g_ota_rx) { ended = "buffer full"; break; }
        if (got && millis() - last > 5000) { ended = "quiet 5 s"; break; }   // quiet for 5 s
        // Run the other jobs (GPS, steps, minute log) only while the line is
        // quiet: they can take longer than the ~90 ms the 1 KB UART buffer
        // holds at 115200 baud, and bytes were lost that way.
        if (!got || millis() - last > 50) service_background();
      }
      memcpy(g_ota_rx_copy, g_ota_rx, got < sizeof g_ota_rx_copy ? got : sizeof g_ota_rx_copy);
      size_t tail = got < sizeof g_ota_rx_tail ? got : sizeof g_ota_rx_tail;
      memcpy(g_ota_rx_tail, g_ota_rx + got - tail, tail);
      size_t plain = ota_unframe(g_ota_rx, got);
      long st = ota_find(g_ota_rx, plain, "HTTP/1.", 0);
      long hdr_end = ota_find(g_ota_rx, plain, "\r\n\r\n", 0);
      if (st >= 0 && hdr_end > st && (size_t)st + 12 <= plain) {
        status = atoi((const char *)g_ota_rx + st + 9);
        size_t b = (size_t)hdr_end + 4;
        size_t bl = plain - b;
        long cl = ota_find(g_ota_rx, (size_t)hdr_end, "Content-Length:", 0);
        if (cl < 0) cl = ota_find(g_ota_rx, (size_t)hdr_end, "content-length:", 0);
        if (cl >= 0) {
          unsigned long want = strtoul((const char *)g_ota_rx + cl + 15, nullptr, 10);
          if (want < bl) bl = want;
        }
        *body = g_ota_rx + b;
        *body_len = bl;
      }
      // Not a usable reply (no status, unexpected status, or a short/long
      // body): show how the read ended and the raw start and end of what the
      // modem sent, escaped, so the +HTTPRECV framing can be seen on the TTL.
      // A plain 404 (no per-device file) is expected and not dumped.
      const bool hex = strstr(path, "enc=hex") != nullptr;
      bool usable = (status == 200 || status == 206) && (!len || *body_len == (hex ? 2 * len : len));
      if (g_ota_dump_all || (!usable && status != 404)) {
        INFO.print("[OTA] Raw modem reply (");
        INFO.print((unsigned)got);
        INFO.print(" bytes, body ");
        INFO.print((unsigned)*body_len);
        INFO.print(", ended: ");
        INFO.print(ended);
        INFO.print("): ");
        ota_print_escaped(g_ota_rx_copy, got < sizeof g_ota_rx_copy ? got : sizeof g_ota_rx_copy);
        if (got > sizeof g_ota_rx_copy) {
          INFO.print(" ... ");
          ota_print_escaped(g_ota_rx_tail, tail);
        }
        INFO.println();
      }
      DEBUG.print("[OTA] GET ");
      DEBUG.print(path);
      DEBUG.print(" -> ");
      DEBUG.print(status);
      DEBUG.print(", ");
      DEBUG.print(got);
      DEBUG.println(" raw bytes");
    }
  }
  gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
  mark_data_activity();
  return status;
}

// ----------------------------------------------------------------------------
//  Manifest
// ----------------------------------------------------------------------------

static void ota_check() {
  char path[96], val[100];
  const uint8_t *body = nullptr;
  size_t blen = 0;

  INFO.println("[OTA] Checking for an update ...");
  snprintf(path, sizeof path, OTA_BASE "devices/%s.json", g_modem.imei);
  int st = ota_http_get(path, 0, 0, &body, &blen);
  if (st != 200) {
    snprintf(path, sizeof path, OTA_BASE "manifest.json");
    st = ota_http_get(path, 0, 0, &body, &blen);
  }
  if (st != 200 || blen == 0 || blen > 1000) {
    WARN.print("[OTA] No manifest (HTTP ");
    WARN.print(st);
    WARN.println("), retry later.");
    g_ota.next_check_at = millis() + OTA_RETRY_MS;
    return;
  }
  char man[1024];
  memcpy(man, body, blen);
  man[blen] = 0;

  uint32_t ver = ota_json_get(man, "version_code", val, sizeof val) ? strtoul(val, nullptr, 0) : 0;
  uint32_t size = ota_json_get(man, "size", val, sizeof val) ? strtoul(val, nullptr, 0) : 0;
  uint32_t hw = ota_json_get(man, "hw_id", val, sizeof val) ? strtoul(val, nullptr, 0) : 0;
  unsigned rollout = ota_json_get(man, "rollout", val, sizeof val) ? (unsigned)atoi(val) : 100u;
  bool have_url = ota_json_get(man, "url", val, sizeof val);
  g_ota.next_check_at = millis() + OTA_CHECK_INTERVAL_MS;
  if (ver) g_ota.avail_version = ver;

  char v[12];
  ota_version_str(ver, v, sizeof v);
  const char *skip = nullptr;
  if (!ver || !size || !have_url) skip = "manifest incomplete";
  else if (hw != OTA_HW_ID_GSM_PCB_R1) skip = "other hardware";
  else if (ver <= g_ota.app_version) skip = "not newer";
  else if (ver == g_ota.bad_version) skip = "rolled back before";
  else if (ver == g_ota.rejected_version) skip = "image rejected before (retried after a restart)";
  else if (size > EXT_SLOT_SIZE || size <= OTA_HDR_SIZE) skip = "bad size";
  else if (ota_crc32(g_modem.imei, strlen(g_modem.imei)) % 100u >= rollout) skip = "not in rollout";
  INFO.print("[OTA] Manifest ");
  INFO.print(path);
  INFO.print(": v");
  INFO.print(v);
  INFO.print(skip ? " - skipped: " : " - downloading");
  INFO.println(skip ? skip : "");
  if (skip) return;

  if (strncmp(val, "http", 4) == 0) {   // absolute URL: keep only the path
    const char *p = strchr(val + 7, '/');
    snprintf(g_ota.dl_path, sizeof g_ota.dl_path, "%s", p ? p : "/");
  } else {
    snprintf(g_ota.dl_path, sizeof g_ota.dl_path, OTA_BASE "%s", val[0] == '/' ? val + 1 : val);
  }
  if (g_ota.dl_version != ver || g_ota.dl_size != size) {   // new download (else resume)
    g_ota.dl_version = ver;
    g_ota.dl_size = size;
    g_ota.dl_offset = 0;
  }
  g_ota.dl_fails = 0;
  g_ota.dl_next_at = millis();
  g_ota.phase = OTA_PH_DOWNLOAD;
}

static void ota_finish_download() {
  ota_image_header_t h;
  static uint8_t scratch[256];
  g_ota.phase = OTA_PH_IDLE;
  ota_img_status_t st = ota_image_check_slot(g_ota_flash.dev(), EXT_SLOT_DOWNLOAD, &h, scratch,
                                             sizeof scratch, OTA_PUBLIC_KEY);
  if (st != OTA_IMG_OK || h.fw_version != g_ota.dl_version) {
    ERR.print("[OTA] Downloaded image REJECTED: ");
    ERR.println(st != OTA_IMG_OK ? ota_img_status_str(st) : "version differs from manifest");
    // Every part was read back after writing, so the file itself is bad:
    // downloading it again would only spend data.
    g_ota.rejected_version = g_ota.dl_version;
    g_ota.dl_version = 0;
    g_ota.dl_size = g_ota.dl_offset = 0;
    g_ota.next_check_at = millis() + OTA_RETRY_MS;
    return;
  }
  if (g_power.bat_volt < OTA_MIN_BATTERY_V || g_power.battery_low) {
    WARN.print("[OTA] Image OK but battery ");
    WARN.print(g_power.bat_volt, 2);
    WARN.println(" V is too low to install; will retry.");
    g_ota.phase = OTA_PH_DOWNLOAD;          // image stays; re-check later
    g_ota.next_check_at = millis() + OTA_RETRY_MS;
    return;
  }
  if (ota_meta_set_pending(g_ota_flash.dev(), h.fw_version) != W25Q_OK) {
    ERR.println("[OTA] Could not mark the update PENDING.");
    return;
  }
  char v[12];
  ota_version_str(h.fw_version, v, sizeof v);
  INFO.print("[OTA] v");
  INFO.print(v);
  INFO.println(" verified and PENDING. Restarting to install.");
  persist_steps();
  SerialDBG.flush();
  NVIC_SystemReset();
}

static void ota_download_chunk() {
  if (g_ota.dl_offset >= g_ota.dl_size) {
    if ((int32_t)(millis() - g_ota.next_check_at) >= 0 || g_ota.force_check) {
      g_ota.force_check = false;
      ota_finish_download();   // waiting for battery: try again
    }
    return;
  }
  if ((int32_t)(millis() - g_ota.dl_next_at) < 0) return;   // pause after a failed part
  // 1 KB per request (OTA_PART), written straight into the slot. A 4 KB sector
  // is erased when its first part is due, so later parts land on erased flash.
  uint32_t n = g_ota.dl_size - g_ota.dl_offset;
  if (n > OTA_PART) n = OTA_PART;
  const uint32_t addr = EXT_SLOT_DOWNLOAD + g_ota.dl_offset;
  if (g_ota.dl_offset % OTA_CHUNK == 0 && w25q_erase_sector(g_ota_flash.dev(), addr) != W25Q_OK) return;
  const uint8_t *body = nullptr;
  size_t blen = 0;
  char path[sizeof g_ota.dl_path + 8];
  snprintf(path, sizeof path, "%s?enc=hex", g_ota.dl_path);
  int st = ota_http_get(path, g_ota.dl_offset, n, &body, &blen);
  // The body is 2*n hex characters; decode in place (body points into g_ota_rx).
  bool got_ok = (st == 206 || (st == 200 && g_ota.dl_offset == 0 && n == g_ota.dl_size)) &&
                blen == 2 * n && ota_hex_decode(body, n, (uint8_t *)body);
  bool ok = got_ok && w25q_write_verify(g_ota_flash.dev(), addr, body, n) == W25Q_OK;
  if (!ok) {
    WARN.print("[OTA] Part at ");
    WARN.print(g_ota.dl_offset);
    WARN.print(" failed (HTTP ");
    WARN.print(st);
    WARN.print(", ");
    WARN.print((unsigned)blen);
    WARN.println(got_ok ? " bytes, flash write failed)." : " bytes).");
    // A failed write leaves the sector dirty: restart it from its first part,
    // which erases it again. A failed download wrote nothing, so just retry.
    if (got_ok) g_ota.dl_offset -= g_ota.dl_offset % OTA_CHUNK;
    g_ota.dl_next_at = millis() + OTA_PART_RETRY_MS;
    if (++g_ota.dl_fails >= OTA_MAX_CHUNK_FAILS) {
      WARN.println("[OTA] Download paused; resumes at the next check.");
      g_ota.phase = OTA_PH_IDLE;
      g_ota.next_check_at = millis() + OTA_RETRY_MS;
    }
    return;
  }
  g_ota.dl_fails = 0;
  g_ota.dl_offset += n;
  if (g_ota.dl_offset % OTA_CHUNK == 0 || g_ota.dl_offset >= g_ota.dl_size) {
    INFO.print("[OTA] Downloaded ");
    INFO.print(g_ota.dl_offset);
    INFO.print("/");
    INFO.println(g_ota.dl_size);
  }
  if (g_ota.dl_offset >= g_ota.dl_size) ota_finish_download();
}

// Download probe: which replies does the N706B pass on? Text vs binary, with
// and without Range, small vs 1 KB. Reads only; writes nothing to flash.
static void ota_probe() {
  struct Probe { const char *what; const char *path; uint32_t from, len; };
  static const Probe probes[] = {
    { "manifest, Range 0-99 (text, 206)", OTA_BASE "manifest.json", 0, 100 },
    { "image, Range 0-99 (binary header)", OTA_BASE "releases/2.1.1/stepwhere.ota", 0, 100 },
    { "image, Range 0-255 (whole header)", OTA_BASE "releases/2.1.1/stepwhere.ota", 0, 256 },
    { "image, Range 0-1023 (one part)", OTA_BASE "releases/2.1.1/stepwhere.ota", 0, 1024 },
    { "image, Range 2048-2147 (program code)", OTA_BASE "releases/2.1.1/stepwhere.ota", 2048, 100 },
    { "image as hex, Range 0-511 (the download path)", OTA_BASE "releases/2.1.1/stepwhere.ota?enc=hex", 0, 512 },
  };
  const size_t count = sizeof probes / sizeof probes[0];
  INFO.println("[PROBE] ---- start ----");
  g_ota_dump_all = true;
  for (size_t i = 0; i < count; i++) {
    const uint8_t *body = nullptr;
    size_t blen = 0;
    INFO.print("[PROBE] ");
    INFO.print((unsigned)(i + 1));
    INFO.print("/");
    INFO.print((unsigned)count);
    INFO.print(" ");
    INFO.println(probes[i].what);
    const uint32_t t0 = millis();
    int st = ota_http_get(probes[i].path, probes[i].from, probes[i].len, &body, &blen);
    INFO.print("[PROBE] ");
    INFO.print((unsigned)(i + 1));
    INFO.print(" result: HTTP ");
    INFO.print(st);
    INFO.print(", body ");
    INFO.print((unsigned)blen);
    INFO.print("/");
    INFO.print((unsigned)probes[i].len);
    INFO.print(", ");
    INFO.print(millis() - t0);
    INFO.print(" ms, last step: ");
    INFO.println(g_ota_step);
    if (body && blen) {
      INFO.print("[PROBE] first bytes: ");
      for (size_t k = 0; k < blen && k < 16; k++) {
        char hx[4];
        snprintf(hx, sizeof hx, "%02X ", body[k]);
        INFO.print(hx);
      }
      INFO.println();
    }
    wait_ms(3000);
  }
  g_ota_dump_all = false;
  INFO.println("[PROBE] ---- end ----");
}

// Called from loop() when the modem is ready and no upload is due.
void ota_tick() {
  if (!g_ota.flash_ok || !g_modem.imei[0]) return;
  if (g_ota_probe) {
    g_ota_probe = false;
    ota_probe();
    return;
  }
  if (g_ota.trial && !g_ota.confirmed) return;   // never update an unconfirmed firmware
  if (g_ota.phase == OTA_PH_DOWNLOAD) {
    ota_download_chunk();
    return;
  }
  if (g_ota.force_check || (int32_t)(millis() - g_ota.next_check_at) >= 0) {
    g_ota.force_check = false;
    ota_check();
  }
}
