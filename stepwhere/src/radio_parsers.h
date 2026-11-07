#pragma once

// Parsers for the N706 serving-cell (AT+NETDMSG) and Wi-Fi scan
// (AT+WIFIAPSCAN) replies. Pure C++ so the host tests in test/ run them.
// Ranges match the server's ingest schema: an out-of-range value would get the
// whole upload rejected, so such fields are dropped here instead.

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace stepwhere {

struct ServingCell {
  bool valid;
  uint16_t mobile_country_code;
  uint16_t mobile_network_code;
  uint16_t tracking_area_code;
  uint32_t cell_id;
  bool has_physical_cell_id;
  uint16_t physical_cell_id;
  bool has_band;
  char band[41];
  bool has_earfcn;
  uint32_t earfcn;
  bool has_rx_dbm;
  int16_t rx_dbm;
  bool has_tx_dbm;
  int16_t tx_dbm;
  bool has_rsrp;
  int16_t rsrp_tenths_dbm;
  bool has_rsrq;
  int16_t rsrq_tenths_db;
  bool has_sinr;
  int16_t sinr_tenths_db;
  char duplex_mode[4];
};

namespace detail {

constexpr size_t kNetdmsgFields = 15;
constexpr size_t kFieldSize = 48;

inline bool parse_unsigned(const char *value, uint32_t maximum, uint32_t &result) {
  if (value[0] == '\0' || value[0] == '-') return false;
  const int base = strlen(value) > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X') ? 16 : 10;
  errno = 0;
  char *end = nullptr;
  const unsigned long parsed = strtoul(value, &end, base);
  if (errno == ERANGE || end == value || *end != '\0' || parsed > maximum) return false;
  result = (uint32_t)parsed;
  return true;
}

inline bool parse_signed(const char *value, int32_t minimum, int32_t maximum, int32_t &result) {
  if (value[0] == '\0') return false;
  errno = 0;
  char *end = nullptr;
  const long parsed = strtol(value, &end, 10);
  if (errno == ERANGE || end == value || *end != '\0' || parsed < minimum || parsed > maximum) return false;
  result = (int32_t)parsed;
  return true;
}

// Copies [begin, end) without surrounding whitespace; false if it does not fit.
inline bool copy_trimmed(const char *begin, const char *end, char *out, size_t out_size) {
  while (begin < end && isspace((unsigned char)*begin)) ++begin;
  while (end > begin && isspace((unsigned char)end[-1])) --end;
  const size_t length = (size_t)(end - begin);
  if (length >= out_size) return false;
  memcpy(out, begin, length);
  out[length] = '\0';
  return true;
}

// Splits on commas outside double quotes; quotes are dropped. Requires exactly
// kNetdmsgFields fields.
inline bool split_fields(const char *payload, char fields[kNetdmsgFields][kFieldSize]) {
  char field[kFieldSize];
  size_t length = 0, count = 0;
  bool inside_quotes = false;
  for (const char *c = payload;; ++c) {
    if (*c == '"') {
      inside_quotes = !inside_quotes;
      continue;
    }
    if (*c == '\0' || (*c == ',' && !inside_quotes)) {
      if (count >= kNetdmsgFields) return false;
      if (!copy_trimmed(field, field + length, fields[count++], kFieldSize)) return false;
      length = 0;
      if (*c == '\0') break;
      continue;
    }
    if (length >= kFieldSize - 1) return false;
    field[length++] = *c;
  }
  return !inside_quotes && count == kNetdmsgFields;
}

}  // namespace detail

// Parses the +NETDMSG line of an AT+NETDMSG reply. On failure `error` names the
// reason and `cell.valid` is false.
inline bool parse_netdmsg(const char *response, ServingCell &cell, const char *&error) {
  memset(&cell, 0, sizeof(cell));
  error = nullptr;

  const char prefix[] = "+NETDMSG:";
  const char *start = strstr(response, prefix);
  if (!start) {
    error = "response did not contain +NETDMSG";
    return false;
  }
  start += sizeof(prefix) - 1;
  const char *end = strchr(start, '\n');
  if (!end) end = start + strlen(start);

  char payload[256];
  if (!detail::copy_trimmed(start, end, payload, sizeof(payload))) {
    error = "+NETDMSG line too long";
    return false;
  }

  char fields[detail::kNetdmsgFields][detail::kFieldSize];
  if (!detail::split_fields(payload, fields)) {
    error = "expected 15 comma-separated fields";
    return false;
  }

  char *separator = strchr(fields[0], '+');
  if (!separator || separator == fields[0] || separator[1] == '\0') {
    error = "invalid MCC+MNC field";
    return false;
  }
  *separator = '\0';
  uint32_t mcc, mnc;
  if (!detail::parse_unsigned(fields[0], 999, mcc) || !detail::parse_unsigned(separator + 1, 999, mnc)) {
    error = "MCC or MNC was outside the backend range";
    return false;
  }

  if (strcasecmp(fields[4], "FDD LTE") == 0) {
    memcpy(cell.duplex_mode, "fdd", 4);
  } else if (strcasecmp(fields[4], "TDD LTE") == 0) {
    memcpy(cell.duplex_mode, "tdd", 4);
  } else {
    error = "network mode was not LTE";
    return false;
  }

  uint32_t tac, cell_id;
  if (!detail::parse_unsigned(fields[1], 65535, tac)) {
    error = "invalid LTE tracking area code";
    return false;
  }
  if (!detail::parse_unsigned(fields[2], 268435455UL, cell_id)) {
    error = "invalid LTE cell ID";
    return false;
  }
  cell.mobile_country_code = (uint16_t)mcc;
  cell.mobile_network_code = (uint16_t)mnc;
  cell.tracking_area_code = (uint16_t)tac;
  cell.cell_id = cell_id;

  uint32_t unsigned_value;
  if (detail::parse_unsigned(fields[3], 503, unsigned_value)) {
    cell.has_physical_cell_id = true;
    cell.physical_cell_id = (uint16_t)unsigned_value;
  }
  const size_t band_length = strlen(fields[5]);
  if (band_length > 0 && band_length < sizeof(cell.band)) {
    cell.has_band = true;
    memcpy(cell.band, fields[5], band_length + 1);
  }
  if (detail::parse_unsigned(fields[6], UINT32_MAX, unsigned_value)) {
    cell.has_earfcn = true;
    cell.earfcn = unsigned_value;
  }

  int32_t signed_value;
  if (detail::parse_signed(fields[7], -200, 100, signed_value)) {
    cell.has_rx_dbm = true;
    cell.rx_dbm = (int16_t)signed_value;
  }
  if (detail::parse_signed(fields[8], -200, 100, signed_value)) {
    cell.has_tx_dbm = true;
    cell.tx_dbm = (int16_t)signed_value;
  }
  if (detail::parse_signed(fields[12], -2000, 1000, signed_value)) {
    cell.has_rsrp = true;
    cell.rsrp_tenths_dbm = (int16_t)signed_value;
  }
  if (detail::parse_signed(fields[13], -1000, 1000, signed_value)) {
    cell.has_rsrq = true;
    cell.rsrq_tenths_db = (int16_t)signed_value;
  }
  if (detail::parse_signed(fields[14], -1000, 1000, signed_value)) {
    cell.has_sinr = true;
    cell.sinr_tenths_db = (int16_t)signed_value;
  }

  cell.valid = true;
  return true;
}

constexpr uint8_t kMaxWifiAccessPoints = 16;

struct WifiAccessPoint {
  char mac[18];
  int16_t rssi;
  uint8_t channel;
  uint32_t observed_at_ms;
};

// Strongest unique access points from one scan.
struct WifiScanResult {
  WifiAccessPoint points[kMaxWifiAccessPoints];
  uint8_t count;
  uint16_t lines_seen;

  void clear() {
    count = 0;
    lines_seen = 0;
  }
};

// Accepts 12 hex digits or colon-separated pairs; writes upper-case AA:BB:...
inline bool normalize_wifi_mac(const char *input, char *output) {
  char hex[12];
  uint8_t count = 0;
  const size_t length = strlen(input);
  if (length != 12 && length != 17) return false;
  for (size_t i = 0; i < length; ++i) {
    if (length == 17 && i % 3 == 2) {
      if (input[i] != ':') return false;
      continue;
    }
    if (!isxdigit((unsigned char)input[i]) || count >= 12) return false;
    hex[count++] = (char)toupper((unsigned char)input[i]);
  }
  if (count != 12) return false;
  for (uint8_t i = 0; i < 6; ++i) {
    output[i * 3] = hex[i * 2];
    output[i * 3 + 1] = hex[i * 2 + 1];
    output[i * 3 + 2] = i == 5 ? '\0' : ':';
  }
  return true;
}

// Adds one "+WIFIAPSCAN: <mac>,<rssi>,<channel>" line. Duplicate MACs keep the
// strongest reading; a full list replaces its weakest entry. Returns false for
// malformed lines.
inline bool parse_wifi_ap_line(const char *line, WifiScanResult &result, uint32_t now_ms) {
  if (strncmp(line, "+WIFIAPSCAN:", 12) != 0) return false;
  const char *value = line + 12;
  while (*value == ' ' || *value == '\t') ++value;
  const char *comma = strchr(value, ',');
  char input_mac[18], mac[18];
  if (!comma || (size_t)(comma - value) >= sizeof(input_mac)) return false;
  memcpy(input_mac, value, comma - value);
  input_mac[comma - value] = '\0';
  if (!normalize_wifi_mac(input_mac, mac)) return false;

  char *tail;
  errno = 0;
  const long rssi = strtol(comma + 1, &tail, 10);
  if (errno == ERANGE || tail == comma + 1 || *tail != ',' || rssi < -128 || rssi > 0) return false;
  value = tail + 1;
  errno = 0;
  const long channel = strtol(value, &tail, 10);
  if (errno == ERANGE || tail == value || channel < 1 || channel > 14) return false;
  while (*tail == ' ' || *tail == '\t' || *tail == '\r') ++tail;
  if (*tail != '\0') return false;

  if (result.lines_seen < 65535) ++result.lines_seen;
  uint8_t index = result.count;
  for (uint8_t i = 0; i < result.count; ++i) {
    if (strcmp(result.points[i].mac, mac) == 0) {
      if (rssi <= result.points[i].rssi) return true;
      index = i;
      break;
    }
  }
  if (index == result.count && result.count == kMaxWifiAccessPoints) {
    index = 0;
    for (uint8_t i = 1; i < kMaxWifiAccessPoints; ++i) {
      if (result.points[i].rssi < result.points[index].rssi) index = i;
    }
    if (rssi <= result.points[index].rssi) return true;
  } else if (index == result.count) {
    ++result.count;
  }
  memcpy(result.points[index].mac, mac, sizeof(mac));
  result.points[index].rssi = (int16_t)rssi;
  result.points[index].channel = (uint8_t)channel;
  result.points[index].observed_at_ms = now_ms;
  return true;
}

}  // namespace stepwhere
