/*
 * HTTPS for https://beacon-timer.local.
 *
 * The board generates its own certificate authority and a short-lived server certificate
 * the first time it boots, and keeps them in LittleFS so a firmware update does not
 * replace the authority the phone already trusts. Port 443 accepts the connection and
 * repeats the request to the existing page on port 80, so there is one set of routes.
 */
#include "tlshttps.h"
#include "certwindow.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <esp_system.h>
#include <string.h>

#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

static const char* CA_SUBJ = "CN=Beacon Timer,O=Beacon Timer";
static const char* SRV_SUBJ = "CN=beacon-timer.local,O=Beacon Timer";
static const unsigned char EKU_SERVER[] = {
  0x30, 0x0a, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01
};

static httpd_handle_t https = nullptr;

static int rng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

static bool writeFile(const char* path, const char* data) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  size_t n = strlen(data);
  size_t w = f.write((const uint8_t*)data, n);
  f.close();
  return w == n;
}

static bool readFile(const char* path, char* buf, size_t cap) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t n = f.read((uint8_t*)buf, cap - 1);
  f.close();
  if (n == 0 || n >= cap - 1) return false;
  buf[n] = 0;
  return true;
}

static bool pairOk(const char* crtPath, const char* keyPath) {
  char crtPem[2048], keyPem[2048];
  if (!readFile(crtPath, crtPem, sizeof crtPem) || !readFile(keyPath, keyPem, sizeof keyPem)) return false;
  mbedtls_x509_crt crt;
  mbedtls_pk_context key;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&key);
  bool ok = mbedtls_x509_crt_parse(&crt, (const unsigned char*)crtPem, strlen(crtPem) + 1) == 0
         && mbedtls_pk_parse_key(&key, (const unsigned char*)keyPem, strlen(keyPem) + 1, nullptr, 0) == 0
         && mbedtls_pk_check_pair(&crt.pk, &key) == 0;
  mbedtls_x509_crt_free(&crt);
  mbedtls_pk_free(&key);
  return ok;
}

static int sanBytes(unsigned char* out, size_t cap, const char* dns) {
  size_t n = strlen(dns);
  if (n < 1 || n > 100 || cap < n + 4) return -1;
  out[0] = 0x30;
  out[1] = (unsigned char)(n + 2);
  out[2] = 0x82;
  out[3] = (unsigned char)n;
  memcpy(out + 4, dns, n);
  return (int)(n + 4);
}

static bool makeCert(mbedtls_pk_context* subjectKey, mbedtls_pk_context* issuerKey,
                     const char* subject, const char* issuer, int isCa,
                     const char* notBefore, const char* notAfter,
                     const char* dns, char* pemOut, size_t pemCap) {
  mbedtls_mpi serial;
  mbedtls_x509write_cert crt;
  mbedtls_mpi_init(&serial);
  mbedtls_x509write_crt_init(&crt);
  unsigned char raw[16];
  rng(nullptr, raw, sizeof raw);
  raw[0] &= 0x7f;
  if (raw[0] == 0) raw[0] = 1;
  bool ok = false;
  unsigned char san[128];
  int sanLen = 0;
  if (mbedtls_mpi_read_binary(&serial, raw, sizeof raw) != 0) goto done;
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, subjectKey);
  mbedtls_x509write_crt_set_issuer_key(&crt, issuerKey);
  if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0) goto done;
  if (mbedtls_x509write_crt_set_issuer_name(&crt, issuer) != 0) goto done;
  if (mbedtls_x509write_crt_set_serial(&crt, &serial) != 0) goto done;
  if (mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter) != 0) goto done;
  if (mbedtls_x509write_crt_set_basic_constraints(&crt, isCa, -1) != 0) goto done;
  if (mbedtls_x509write_crt_set_key_usage(&crt, isCa
        ? (MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN)
        : MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0) goto done;
  if (mbedtls_x509write_crt_set_subject_key_identifier(&crt) != 0) goto done;
  if (mbedtls_x509write_crt_set_authority_key_identifier(&crt) != 0) goto done;
  if (!isCa) {
    sanLen = sanBytes(san, sizeof san, dns);
    if (sanLen < 0) goto done;
    if (mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME,
          MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, (size_t)sanLen) != 0) goto done;
    if (mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_EXTENDED_KEY_USAGE,
          MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, EKU_SERVER, sizeof EKU_SERVER) != 0) goto done;
  }
  if (mbedtls_x509write_crt_pem(&crt, (unsigned char*)pemOut, pemCap, rng, nullptr) != 0) goto done;
  ok = true;
done:
  mbedtls_x509write_crt_free(&crt);
  mbedtls_mpi_free(&serial);
  return ok;
}

static bool makeKey(mbedtls_pk_context* key, char* pemOut, size_t pemCap) {
  mbedtls_pk_init(key);
  if (mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return false;
  if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*key), rng, nullptr) != 0) return false;
  return mbedtls_pk_write_key_pem(key, (unsigned char*)pemOut, pemCap) == 0;
}

static bool loadKey(mbedtls_pk_context* key, const char* path, char* pem, size_t cap) {
  mbedtls_pk_init(key);
  if (!readFile(path, pem, cap)) return false;
  return mbedtls_pk_parse_key(key, (const unsigned char*)pem, strlen(pem) + 1, nullptr, 0) == 0;
}

static void wipe(char* p, size_t n) { if (p) memset(p, 0, n); }

static bool issueServer(mbedtls_pk_context* caKey) {
  mbedtls_pk_context srv;
  char* srvPem = (char*)malloc(2048);
  char* keyPem = (char*)malloc(2048);
  bool ok = false;
  bool keyed = false;
  if (!srvPem || !keyPem) goto done;
  keyed = true;
  if (!makeKey(&srv, keyPem, 2048)) { Serial.println("cert: server key failed"); goto done; }
  if (!makeCert(&srv, caKey, SRV_SUBJ, CA_SUBJ, 0, CERT_NOT_BEFORE, CERT_NOT_AFTER, TLS_HOST, srvPem, 2048)) {
    Serial.println("cert: server certificate failed");
    goto done;
  }
  if (!writeFile("/srv.key.tmp", keyPem) || !writeFile("/srv.crt.tmp", srvPem) || !writeFile("/srv.dates.tmp", CERT_NOT_AFTER)) goto done;
  LittleFS.remove("/srv.key");
  LittleFS.remove("/srv.crt");
  LittleFS.remove("/srv.dates");
  ok = LittleFS.rename("/srv.key.tmp", "/srv.key") && LittleFS.rename("/srv.crt.tmp", "/srv.crt") && LittleFS.rename("/srv.dates.tmp", "/srv.dates");
done:
  wipe(keyPem, keyPem ? 2048 : 0);
  free(srvPem);
  free(keyPem);
  if (keyed) mbedtls_pk_free(&srv);
  return ok;
}

static bool issueCa() {
  mbedtls_pk_context ca;
  char* crtPem = (char*)malloc(2048);
  char* keyPem = (char*)malloc(2048);
  bool ok = false;
  bool keyed = false;
  if (!crtPem || !keyPem) goto done;
  keyed = true;
  if (!makeKey(&ca, keyPem, 2048)) { Serial.println("cert: authority key failed"); goto done; }
  if (!makeCert(&ca, &ca, CA_SUBJ, CA_SUBJ, 1, "20200101000000", "20400101000000", nullptr, crtPem, 2048)) {
    Serial.println("cert: authority certificate failed");
    goto done;
  }
  if (!writeFile("/ca.key.tmp", keyPem) || !writeFile("/ca.crt.tmp", crtPem)) goto done;
  LittleFS.remove("/ca.key");
  LittleFS.remove("/ca.crt");
  ok = LittleFS.rename("/ca.key.tmp", "/ca.key") && LittleFS.rename("/ca.crt.tmp", "/ca.crt");
done:
  wipe(keyPem, keyPem ? 2048 : 0);
  free(crtPem);
  free(keyPem);
  if (keyed) mbedtls_pk_free(&ca);
  return ok;
}

static void tlsPrepareBody() {
  if (!pairOk("/ca.crt", "/ca.key")) {
    Serial.println("cert: creating a new authority");
    if (!issueCa()) { Serial.println("cert: authority was not stored"); return; }
    LittleFS.remove("/srv.crt");
    LittleFS.remove("/srv.key");
    LittleFS.remove("/srv.dates");
  }
  char dates[20];
  bool datesOk = readFile("/srv.dates", dates, sizeof dates) && strcmp(dates, CERT_NOT_AFTER) == 0;
  if (pairOk("/srv.crt", "/srv.key") && datesOk) {
    Serial.println("cert: already on the board");
    return;
  }
  mbedtls_pk_context ca;
  char caPem[2048];
  Serial.println("cert: signing the server certificate");
  if (!loadKey(&ca, "/ca.key", caPem, sizeof caPem) || !issueServer(&ca)) Serial.println("cert: server certificate was not stored");
  else Serial.println("cert: ready");
  wipe(caPem, sizeof caPem);
  mbedtls_pk_free(&ca);
}

static SemaphoreHandle_t certDone = nullptr;

static void certTask(void*) {
  tlsPrepareBody();
  xSemaphoreGive(certDone);
  vTaskDelete(nullptr);
}

void tlsPrepare() {
  certDone = xSemaphoreCreateBinary();
  if (!certDone || xTaskCreate(certTask, "cert", 24576, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("cert: skipped, not enough memory");
    if (certDone) vSemaphoreDelete(certDone);
    certDone = nullptr;
    return;
  }
  xSemaphoreTake(certDone, pdMS_TO_TICKS(60000));
  vSemaphoreDelete(certDone);
  certDone = nullptr;
}

// ---- port 443 repeats the request to the page on port 80 -------------------------------------

static bool matchAny(const char*, const char*, size_t) { return true; }

static const char* methodName(int m) {
  switch (m) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_HEAD: return "HEAD";
    default: return nullptr;
  }
}

static bool headerIs(const char* name, const char* want) {
  while (*name && *want) {
    char a = (*name >= 'A' && *name <= 'Z') ? (char)(*name + 32) : *name;
    char b = (*want >= 'A' && *want <= 'Z') ? (char)(*want + 32) : *want;
    if (a != b) return false;
    name++;
    want++;
  }
  return *name == 0 && *want == 0;
}

static esp_err_t proxyReq(httpd_req_t* req) {
  const char* method = methodName(req->method);
  if (!method || !req->uri) return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "method");

  char query[200];
  query[0] = 0;
  int qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0 && qlen < (int)sizeof query) httpd_req_get_url_query_str(req, query, sizeof query);

  char host[80];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK) strncpy(host, TLS_HOST, sizeof host);

  char ctype[80];
  ctype[0] = 0;
  httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof ctype);

  WiFiClient client;
  client.setTimeout(15000);
  if (!client.connect(WiFi.localIP(), 80)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "page unavailable");

  char head[900];
  int hlen = snprintf(head, sizeof head, "%s %s%s%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                      method, req->uri, query[0] ? "?" : "", query, host);
  if (hlen < 0 || hlen >= (int)sizeof head) { client.stop(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request"); }
  if (req->content_len) {
    int n = snprintf(head + hlen, sizeof head - hlen, "Content-Type: %s\r\nContent-Length: %u\r\n",
                     ctype[0] ? ctype : "application/octet-stream", (unsigned)req->content_len);
    if (n < 0 || hlen + n >= (int)sizeof head) { client.stop(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request"); }
    hlen += n;
  }
  if (hlen + 2 >= (int)sizeof head) { client.stop(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request"); }
  memcpy(head + hlen, "\r\n", 2);
  hlen += 2;
  if (client.write((const uint8_t*)head, hlen) != (size_t)hlen) { client.stop(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "request"); }

  uint8_t* buf = (uint8_t*)malloc(1024);
  if (!buf) { client.stop(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memory"); }
  size_t left = req->content_len;
  while (left) {
    int want = left > 1024 ? 1024 : (int)left;
    int n = httpd_req_recv(req, (char*)buf, want);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) { free(buf); client.stop(); return ESP_FAIL; }
    int off = 0;
    while (off < n) {
      int w = client.write(buf + off, n - off);
      if (w <= 0) { free(buf); client.stop(); return ESP_FAIL; }
      off += w;
    }
    left -= (size_t)n;
  }

  char* hdr = (char*)malloc(2048);
  if (!hdr) { free(buf); client.stop(); return ESP_FAIL; }
  size_t hn = 0;
  uint32_t deadline = millis() + 15000;
  bool gotHdr = false;
  while (hn + 1 < 2048 && (int32_t)(millis() - deadline) < 0) {
    if (!client.available()) {
      if (!client.connected()) break;
      delay(2);
      continue;
    }
    int b = client.read();
    if (b < 0) break;
    hdr[hn++] = (char)b;
    if (hn >= 4 && memcmp(hdr + hn - 4, "\r\n\r\n", 4) == 0) { gotHdr = true; break; }
  }
  hdr[hn] = 0;
  if (!gotHdr) { free(hdr); free(buf); client.stop(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no response"); }

  char* lineEnd = strstr(hdr, "\r\n");
  char* status = strchr(hdr, ' ');
  if (!lineEnd || !status || status > lineEnd) { free(hdr); free(buf); client.stop(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bad response"); }
  *lineEnd = 0;
  httpd_resp_set_status(req, status + 1);

  long bodyLen = -1;
  char hname[8][32];
  char hval[8][128];
  int hcount = 0;
  char* p = lineEnd + 2;
  while (*p && *p != '\r') {
    char* nl = strstr(p, "\r\n");
    if (!nl) break;
    *nl = 0;
    char* colon = strchr(p, ':');
    if (colon) {
      *colon = 0;
      char* val = colon + 1;
      while (*val == ' ') val++;
      if (headerIs(p, "content-length")) bodyLen = strtol(val, nullptr, 10);
      else if (!headerIs(p, "connection") && !headerIs(p, "keep-alive") && !headerIs(p, "transfer-encoding")) {
        if (headerIs(p, "content-type")) httpd_resp_set_type(req, val);
        else if (hcount < 8 && strlen(p) < sizeof hname[0] && strlen(val) < sizeof hval[0]) {
          strncpy(hname[hcount], p, sizeof hname[0]);
          strncpy(hval[hcount], val, sizeof hval[0]);
          httpd_resp_set_hdr(req, hname[hcount], hval[hcount]);
          hcount++;
        }
      }
    }
    p = nl + 2;
  }

  bool headOnly = req->method == HTTP_HEAD;
  if (headOnly || bodyLen == 0) {
    httpd_resp_send(req, "", 0);
  } else {
    size_t remain = bodyLen > 0 ? (size_t)bodyLen : (size_t)-1;
    while (remain) {
      if (!client.available()) {
        if (!client.connected()) break;
        if ((int32_t)(millis() - deadline) >= 0) break;
        delay(2);
        continue;
      }
      int want = remain < 1024 ? (int)remain : 1024;
      int n = client.read(buf, want);
      if (n <= 0) break;
      if (httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) break;
      if (bodyLen > 0) remain -= (size_t)n;
      deadline = millis() + 15000;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
  }
  free(hdr);
  free(buf);
  client.stop();
  return ESP_OK;
}

bool tlsStart() {
  if (https) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  char* crt = (char*)malloc(4096);
  char* key = (char*)malloc(2048);
  char* ca = (char*)malloc(2048);
  bool started = false;
  if (!crt || !key || !ca) goto done;
  if (!readFile("/srv.crt", crt, 3000) || !readFile("/srv.key", key, 2048) || !readFile("/ca.crt", ca, 2048)) {
    Serial.println("https: no certificate");
    goto done;
  }
  if (strlen(crt) + strlen(ca) + 2 >= 4096) goto done;
  strcat(crt, ca);   // leaf, then the authority, so the phone can build the chain

  {
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.uri_match_fn = matchAny;
    conf.httpd.max_open_sockets = 2;
    conf.httpd.max_uri_handlers = 8;
    conf.httpd.stack_size = 20480;
    conf.httpd.ctrl_port = 32769;
    conf.httpd.recv_wait_timeout = 15;
    conf.httpd.send_wait_timeout = 15;
    conf.httpd.lru_purge_enable = true;
    conf.cacert_pem = (const uint8_t*)crt;
    conf.cacert_len = strlen(crt) + 1;
    conf.prvtkey_pem = (const uint8_t*)key;
    conf.prvtkey_len = strlen(key) + 1;

    httpd_handle_t handle = nullptr;
    if (httpd_ssl_start(&handle, &conf) != ESP_OK) {
      Serial.println("https: server did not start");
      goto done;
    }
    httpd_uri_t route = {};
    route.uri = "/";
    route.handler = proxyReq;
    const httpd_method_t methods[] = { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_HEAD };
    for (httpd_method_t m : methods) {
      route.method = m;
      if (httpd_register_uri_handler(handle, &route) != ESP_OK) {
        httpd_ssl_stop(handle);
        Serial.println("https: route failed");
        goto done;
      }
    }
    https = handle;
    started = true;
    Serial.println("https: listening on 443");
  }
done:
  wipe(key, key ? 2048 : 0);
  free(crt);
  free(key);
  free(ca);
  return started;
}

void tlsStop() {
  if (!https) return;
  httpd_ssl_stop(https);
  https = nullptr;
}
