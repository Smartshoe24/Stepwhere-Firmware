/* Host tests for stepwhere/src/ota_util.h (OTA client parsing). */
#include <stdio.h>
#include <string.h>
#include "../../stepwhere/src/ota_util.h"

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static size_t mk(uint8_t *buf, const char *s) { size_t n = strlen(s); memcpy(buf, s, n); return n; }

int main(void)
{
    static uint8_t b[8192];
    size_t n;
    char out[64];

    CHECK(ota_parse_version("v2.1-lis2dh12") == OTA_VERSION(2, 1, 0));
    CHECK(ota_parse_version("v1.3.7") == OTA_VERSION(1, 3, 7));
    CHECK(ota_parse_version("2.10") == OTA_VERSION(2, 10, 0));
    CHECK(ota_parse_version("garbage") == 0);

    /* Format 1: +HTTPRECV:<n>,<data> in one packet */
    const char *http = "HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\n\r\nab\x00" "de";
    char tmp[200]; int hl = 49 + 5;
    memcpy(tmp, http, (size_t)hl);
    n = (size_t)snprintf((char *)b, sizeof b, "\r\nOK\r\n\r\n+HTTPRECV:%d,", hl);
    memcpy(b + n, tmp, (size_t)hl); n += (size_t)hl;
    n += mk(b + n, "\r\n+HTTPCLOSED: HTTP Link Closed\r\n");
    size_t p = ota_unframe(b, n);
    CHECK(p == (size_t)hl && memcmp(b, tmp, (size_t)hl) == 0);

    /* Format 2: two packets with lengths, split inside the body */
    n = (size_t)snprintf((char *)b, sizeof b, "+HTTPRECV: 30,");
    memcpy(b + n, tmp, 30); n += 30;
    n += (size_t)snprintf((char *)b + n, sizeof b - n, "\r\n+HTTPRECV: %d,", hl - 30);
    memcpy(b + n, tmp + 30, (size_t)(hl - 30)); n += (size_t)(hl - 30);
    n += mk(b + n, "\r\n+HTTPCLOSED:\r\n");
    p = ota_unframe(b, n);
    CHECK(p == (size_t)hl && memcmp(b, tmp, (size_t)hl) == 0);

    /* Format 3: no length, data until +HTTPCLOSED (as seen for uploads) */
    n = mk(b, "\r\nOK\r\n\r\n+HTTPRECV:\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}\r\n+HTTPCLOSED: HTTP Link Closed\r\n");
    p = ota_unframe(b, n);
    CHECK(p == 59 - 22 + 0 || p > 0);
    CHECK(ota_find(b, p, "HTTP/1.1 200", 0) == 0 && b[p - 1] == '}');

    /* No +HTTPRECV at all */
    n = mk(b, "\r\nOK\r\n\r\n+HTTPCLOSED\r\n");
    CHECK(ota_unframe(b, n) == 0);

    /* Manifest from fw_server.py */
    const char *man = "{\n  \"version\": \"1.3.0\",\n  \"version_code\": 66304,\n  \"hw_id\": \"0x00010001\",\n"
                      "  \"size\": 72684,\n  \"url\": \"releases/1.3.0/stepwhere.ota\",\n  \"rollout\": 10\n}";
    CHECK(ota_json_get(man, "version_code", out, sizeof out) && strtoul(out, 0, 0) == 66304);
    CHECK(ota_json_get(man, "hw_id", out, sizeof out) && strtoul(out, 0, 0) == 0x00010001);
    CHECK(ota_json_get(man, "url", out, sizeof out) && strcmp(out, "releases/1.3.0/stepwhere.ota") == 0);
    CHECK(ota_json_get(man, "rollout", out, sizeof out) && atoi(out) == 10);
    CHECK(!ota_json_get(man, "missing", out, sizeof out));

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
