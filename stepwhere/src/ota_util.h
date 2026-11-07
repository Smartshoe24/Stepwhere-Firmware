/*
 * ota_util.h - Parsing helpers of the OTA client (ota.ino), kept free of
 * Arduino types so tests/host/test_ota_util.c can test them on a PC.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ota_layout.h>

static uint32_t ota_parse_version(const char *s) {
  unsigned a = 0, b = 0, c = 0;
  if (*s == 'v' || *s == 'V') s++;
  int n = sscanf(s, "%u.%u.%u", &a, &b, &c);
  if (n < 2) return 0;
  return OTA_VERSION(a, b, n == 3 ? c : 0);
}

static long ota_find(const uint8_t *buf, size_t len, const char *pat, size_t from) {
  size_t n = strlen(pat);
  for (size_t i = from; i + n <= len; i++)
    if (memcmp(buf + i, pat, n) == 0) return (long)i;
  return -1;
}

// Removes the modem framing: every "+HTTPRECV:[ ]<n>," is followed by n data
// bytes; a "+HTTPRECV:" without a length is followed by data up to
// "+HTTPCLOSED". Leaves the plain HTTP response in buf, returns its length.
static size_t ota_unframe(uint8_t *buf, size_t len) {
  size_t out = 0;
  long p = ota_find(buf, len, "+HTTPRECV:", 0);
  while (p >= 0) {
    size_t q = (size_t)p + 10;
    while (q < len && buf[q] == ' ') q++;
    size_t digits = q, n = 0;
    while (q < len && buf[q] >= '0' && buf[q] <= '9') n = n * 10 + (buf[q++] - '0');
    size_t start, end;
    if (q > digits && q < len && buf[q] == ',') {
      start = q + 1;
      end = start + n <= len ? start + n : len;
    } else {
      start = q;
      while (start < len && (buf[start] == '\r' || buf[start] == '\n')) start++;
      long c = ota_find(buf, len, "+HTTPCLOSED", start);
      end = c >= 0 ? (size_t)c : len;
      while (end > start && (buf[end - 1] == '\r' || buf[end - 1] == '\n')) end--;
    }
    memmove(buf + out, buf + start, end - start);
    out += end - start;
    p = ota_find(buf, len, "+HTTPRECV:", end);
  }
  return out;
}

// Value of "key" in a flat JSON object; copies a string or number into out.
static bool ota_json_get(const char *json, const char *key, char *out, size_t n) {
  char pat[32];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ') p++;
  size_t i = 0;
  if (*p == '"') {
    p++;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
  } else {
    while (*p && *p != ',' && *p != '}' && *p != ' ' && i + 1 < n) out[i++] = *p++;
  }
  out[i] = 0;
  return i > 0;
}

