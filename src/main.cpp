#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstdint>
#include <cstring>
#include <esp_heap_caps.h>
#include <mbedtls/md.h>
#include <time.h>

#include "CloudConfig.h"
#include "secrets.h"

extern "C" {
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
}

namespace {

char lineBuf[96];
size_t lineLen = 0;

static constexpr uint32_t kWifiConnectTimeoutMs = 20000;
static constexpr uint32_t kTimeSyncTimeoutMs = 15000;
static constexpr uint32_t kHttpTimeoutMs = 15000;
static constexpr time_t kMinValidUnixTime = 1735689600;  // 2025-01-01T00:00:00Z
static constexpr const char* kNtpServer1 = "pool.ntp.org";
static constexpr const char* kNtpServer2 = "time.cloudflare.com";
static constexpr const char* kNtpServer3 = "time.nist.gov";
static constexpr const char* kBatchSchema = "tm.batch.v1";
static constexpr const char* kBatchProfile = "tm.v1.vw8_shzk16_env_power";
static constexpr const char* kIngestHmacScheme = "tm-hmac-v1";
static constexpr const char* kIngestSignaturePrefix = "v1=";
static constexpr size_t kIngestSignaturePrefixChars = 3;
static constexpr uint32_t kSamplePeriodSeconds = 900;
static constexpr const char* kFirmwareVersion = "0.1.0";
static constexpr const char* kHardwareVersion = "esp32-s3-wroom-n16r8";
static constexpr size_t kSha256DigestBytes = 32;
static constexpr size_t kSha256HexChars = kSha256DigestBytes * 2;
static constexpr size_t kBatchBodyCap = 256 * 1024;

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "ssid_not_found";
    case WL_SCAN_COMPLETED:
      return "scan_completed";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect_failed";
    case WL_CONNECTION_LOST:
      return "connection_lost";
    case WL_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

bool ensureWifi(uint32_t timeoutMs = kWifiConnectTimeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  if (strcmp(WIFI_SSID, "your-wifi-ssid") == 0) {
    Serial.println("wifi: configure include/secrets.h before connecting");
    return false;
  }

  Serial.printf("wifi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false, 1000);
  delay(100);

  const wl_status_t beginStatus = WiFi.begin(WIFI_SSID, WIFI_PASS);
  if (beginStatus == WL_CONNECT_FAILED) {
    Serial.println("wifi: begin failed");
    return false;
  }

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi: connect failed");
    WiFi.disconnect(false, false, 1000);
    return false;
  }

  Serial.print("wifi: connected ip=");
  Serial.println(WiFi.localIP());
  return true;
}

void printWifiDetails() {
  const wl_status_t status = WiFi.status();
  Serial.print("wifi: state=");
  Serial.println(wifiStatusName(status));

  if (status != WL_CONNECTED) {
    return;
  }

  Serial.print("wifi: ssid=");
  Serial.print(WIFI_SSID);
  Serial.print(" bssid=");
  Serial.print(WiFi.BSSIDstr());
  Serial.print(" channel=");
  Serial.print(WiFi.channel());
  Serial.print(" rssi=");
  Serial.println(WiFi.RSSI());

  Serial.print("wifi: ip=");
  Serial.print(WiFi.localIP());
  Serial.print(" subnet=");
  Serial.print(WiFi.subnetMask());
  Serial.print(" gateway=");
  Serial.println(WiFi.gatewayIP());

  Serial.print("wifi: dns1=");
  Serial.print(WiFi.dnsIP(0));
  Serial.print(" dns2=");
  Serial.println(WiFi.dnsIP(1));
}

bool timeLooksValid() {
  return time(nullptr) >= kMinValidUnixTime;
}

void printUtcTime(const char* prefix) {
  const time_t now = time(nullptr);
  if (now < kMinValidUnixTime) {
    Serial.print(prefix);
    Serial.println("not set");
    return;
  }

  struct tm tmUtc;
  gmtime_r(&now, &tmUtc);
  char text[32];
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
  Serial.print(prefix);
  Serial.println(text);
}

bool ensureTime(uint32_t timeoutMs = kTimeSyncTimeoutMs) {
  if (timeLooksValid()) return true;

  Serial.println("time: syncing SNTP");
  configTime(0, 0, kNtpServer1, kNtpServer2, kNtpServer3);

  const uint32_t start = millis();
  while (!timeLooksValid() && millis() - start < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (!timeLooksValid()) {
    Serial.println("time: sync failed");
    return false;
  }

  printUtcTime("time: synced utc=");
  return true;
}

size_t caBundleSize() {
  const uintptr_t start =
      reinterpret_cast<uintptr_t>(x509_crt_bundle_start);
  const uintptr_t end = reinterpret_cast<uintptr_t>(x509_crt_bundle_end);
  if (end <= start) return 0;
  return static_cast<size_t>(end - start);
}

bool configureTlsClient(WiFiClientSecure& client) {
  const size_t bundleSize = caBundleSize();
  if (bundleSize == 0) {
    Serial.println("tls: CA bundle is empty");
    return false;
  }

  client.setCACertBundle(x509_crt_bundle_start, bundleSize);
  client.setTimeout(kHttpTimeoutMs);
  Serial.printf("tls: using ESP-IDF CA bundle bytes=%u\n",
                static_cast<unsigned>(bundleSize));
  return true;
}

String compactResponse(String response) {
  response.replace("\r", "");
  response.replace("\n", " ");
  response.trim();
  if (response.length() > 180) {
    response = response.substring(0, 180) + "...";
  }
  return response;
}

bool bytesToHex(const uint8_t* bytes, size_t len, char* out, size_t outCap) {
  static constexpr char kHex[] = "0123456789abcdef";

  if (outCap < len * 2 + 1) {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kHex[bytes[i] >> 4];
    out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
  }
  out[len * 2] = '\0';
  return true;
}

bool sha256Hex(const char* data, size_t dataLen, char* hexOut,
               size_t hexOutCap) {
  const mbedtls_md_info_t* mdInfo =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mdInfo == nullptr) {
    Serial.println("crypto: SHA-256 unavailable");
    return false;
  }

  uint8_t digest[kSha256DigestBytes];
  const int rc = mbedtls_md(
      mdInfo, reinterpret_cast<const unsigned char*>(data), dataLen, digest);
  if (rc != 0) {
    Serial.printf("crypto: SHA-256 failed rc=%d\n", rc);
    return false;
  }

  return bytesToHex(digest, sizeof(digest), hexOut, hexOutCap);
}

bool hmacSha256Hex(const char* secret, const char* data, size_t dataLen,
                   char* hexOut, size_t hexOutCap) {
  const mbedtls_md_info_t* mdInfo =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mdInfo == nullptr) {
    Serial.println("crypto: SHA-256 unavailable");
    return false;
  }

  uint8_t digest[kSha256DigestBytes];
  const int rc = mbedtls_md_hmac(
      mdInfo, reinterpret_cast<const unsigned char*>(secret), strlen(secret),
      reinterpret_cast<const unsigned char*>(data), dataLen, digest);
  if (rc != 0) {
    Serial.printf("crypto: HMAC-SHA256 failed rc=%d\n", rc);
    return false;
  }

  return bytesToHex(digest, sizeof(digest), hexOut, hexOutCap);
}

char* allocateBatchBody(size_t cap) {
  void* ptr = nullptr;

#if defined(BOARD_HAS_PSRAM)
  ptr = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr != nullptr) {
    return static_cast<char*>(ptr);
  }
  Serial.println("batch: PSRAM body allocation failed, trying internal heap");
#endif

  ptr = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (ptr == nullptr) {
    Serial.printf("batch: body allocation failed bytes=%u\n",
                  static_cast<unsigned>(cap));
    return nullptr;
  }
  return static_cast<char*>(ptr);
}

void freeBatchBody(char* body) {
  if (body != nullptr) {
    heap_caps_free(body);
  }
}

bool buildIngestAuthHeaders(const char* body, size_t bodyLen, char* timestampOut,
                            size_t timestampOutCap, char* contentSha256Out,
                            size_t contentSha256OutCap, char* signatureOut,
                            size_t signatureOutCap) {
  if (strcmp(CLOUD_DEVICE_HMAC_SECRET, "your-device-hmac-secret") == 0 ||
      strlen(CLOUD_DEVICE_HMAC_SECRET) == 0) {
    Serial.println(
        "cloud: configure CLOUD_DEVICE_HMAC_SECRET in include/secrets.h");
    return false;
  }

  const long long timestamp = static_cast<long long>(time(nullptr));
  const int timestampLen =
      snprintf(timestampOut, timestampOutCap, "%lld", timestamp);
  if (timestampLen <= 0 ||
      static_cast<size_t>(timestampLen) >= timestampOutCap) {
    Serial.println("cloud: timestamp buffer too small");
    return false;
  }

  if (!sha256Hex(body, bodyLen, contentSha256Out, contentSha256OutCap)) {
    return false;
  }

  char canonical[320];
  const int canonicalLen = snprintf(
      canonical, sizeof(canonical), "%s\nPOST\n%s\n%s\n%s\n%s",
      kIngestHmacScheme, CLOUD_INGEST_PATH, CLOUD_DEVICE_ID, timestampOut,
      contentSha256Out);
  if (canonicalLen <= 0 ||
      static_cast<size_t>(canonicalLen) >= sizeof(canonical)) {
    Serial.println("cloud: HMAC canonical string too long");
    return false;
  }

  char signatureHex[kSha256HexChars + 1];
  if (!hmacSha256Hex(CLOUD_DEVICE_HMAC_SECRET, canonical,
                     static_cast<size_t>(canonicalLen), signatureHex,
                     sizeof(signatureHex))) {
    return false;
  }

  const int signatureLen = snprintf(signatureOut, signatureOutCap, "%s%s",
                                    kIngestSignaturePrefix, signatureHex);
  if (signatureLen <= 0 ||
      static_cast<size_t>(signatureLen) >= signatureOutCap) {
    Serial.println("cloud: signature buffer too small");
    return false;
  }

  return true;
}

bool resolveCloudHost(IPAddress& addressOut) {
  Serial.print("dns: resolving ");
  Serial.println(CLOUD_HOST);

  const int rc = WiFi.hostByName(CLOUD_HOST, addressOut);
  if (rc != 1) {
    Serial.printf("dns: failed rc=%d\n", rc);
    return false;
  }

  Serial.print("dns: resolved ip=");
  Serial.println(addressOut);
  return true;
}

int httpsRequest(const char* method, const char* url, const char* body,
                 size_t bodyLen, String* responseOut,
                 bool includeDeviceAuth = false) {
  if (!ensureWifi()) return -1;
  if (!ensureTime()) return -1;

  WiFiClientSecure client;
  if (!configureTlsClient(client)) return -1;

  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(client, url)) {
    Serial.println("http: begin failed");
    return -1;
  }

  int code = -1;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else {
    if (body == nullptr || bodyLen == 0) {
      Serial.println("http: POST body is empty");
      http.end();
      return -1;
    }

    http.addHeader("Content-Type", "application/json");
    if (includeDeviceAuth) {
      char timestamp[24];
      char contentSha256[kSha256HexChars + 1];
      char signature[kIngestSignaturePrefixChars + kSha256HexChars + 1];
      if (!buildIngestAuthHeaders(body, bodyLen, timestamp, sizeof(timestamp),
                                  contentSha256, sizeof(contentSha256),
                                  signature, sizeof(signature))) {
        http.end();
        return -1;
      }
      http.addHeader("X-TM-Device", CLOUD_DEVICE_ID);
      http.addHeader("X-TM-Timestamp", timestamp);
      http.addHeader("X-TM-Content-SHA256", contentSha256);
      http.addHeader("X-TM-Signature", signature);
    }
    code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body)),
                     bodyLen);
  }

  if (responseOut != nullptr) {
    *responseOut = http.getString();
  }
  http.end();
  return code;
}

void printHelp() {
  Serial.println("commands:");
  Serial.println("  help");
  Serial.println("  wifi");
  Serial.println("  time");
  Serial.println("  status");
  Serial.println("  conn");
  Serial.println("  health");
  Serial.println("  ready");
  Serial.println("  payload");
  Serial.println("  batch");
}

void commandWifi() {
  if (!ensureWifi()) return;
  printWifiDetails();
}

void commandTime() {
  if (timeLooksValid()) {
    printUtcTime("time: utc=");
    return;
  }

  if (!ensureWifi()) return;
  if (!ensureTime()) return;
  printUtcTime("time: utc=");
}

void commandHealth() {
  String response;
  const int code = httpsRequest("GET", CLOUD_HEALTH_URL, nullptr, 0, &response);
  Serial.printf("health: http=%d\n", code);
  Serial.println(response);
}

void commandReadiness() {
  String response;
  const int code =
      httpsRequest("GET", CLOUD_READINESS_URL, nullptr, 0, &response);
  Serial.printf("ready: http=%d\n", code);
  Serial.println(response);
}

void commandStatus() {
  Serial.println("status: checking real cloud connectivity");

  const bool wifiOk = ensureWifi();
  printWifiDetails();
  if (!wifiOk) {
    Serial.println("status: OFFLINE reason=wifi_not_connected");
    return;
  }

  IPAddress cloudIp;
  const bool dnsOk = resolveCloudHost(cloudIp);

  const bool timeOk = ensureTime();
  if (timeOk) {
    printUtcTime("time: utc=");
  }

  if (!dnsOk) {
    Serial.println("status: OFFLINE reason=dns_or_wan_unreachable");
    return;
  }

  if (!timeOk) {
    Serial.println("status: OFFLINE reason=time_sync_failed");
    return;
  }

  String response;
  const int code = httpsRequest("GET", CLOUD_HEALTH_URL, nullptr, 0, &response);
  const bool cloudOk = code >= 200 && code < 300;
  Serial.printf("cloud: health http=%d\n", code);
  if (response.length() > 0) {
    Serial.print("cloud: body=");
    Serial.println(compactResponse(response));
  }

  if (cloudOk) {
    Serial.println("status: ONLINE cloud_reachable=yes");
  } else {
    Serial.println("status: OFFLINE reason=cloud_health_failed");
  }
}

long long nextSeqFirst() {
  static long long lastSeqFirst = 0;

  long long candidate = static_cast<long long>(time(nullptr));
  if (candidate <= lastSeqFirst + 1) {
    candidate = lastSeqFirst + 2;
  }
  lastSeqFirst = candidate;
  return candidate;
}

bool buildBatch(char* body, size_t bodyCap, char* batchId, size_t batchIdCap,
                size_t* bodyLenOut, long long* seqLastOut) {
  const long long seqFirst = nextSeqFirst();
  const long long seqLast = seqFirst + 1;
  const long long now = static_cast<long long>(time(nullptr));
  const unsigned long uptimeSeconds = millis() / 1000UL;
  const int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;

  const int batchIdLen = snprintf(
      batchId, batchIdCap, "%s-%lld-%lld", CLOUD_DEVICE_ID, seqFirst, seqLast);
  if (batchIdLen <= 0 || static_cast<size_t>(batchIdLen) >= batchIdCap) {
    Serial.println("batch: id too long");
    return false;
  }

  const int bodyLen = snprintf(
      body, bodyCap,
      "{\"schema\":\"%s\","
      "\"profile\":\"%s\","
      "\"device_id\":\"%s\","
      "\"batch_id\":\"%s\","
      "\"seq_first\":%lld,"
      "\"seq_last\":%lld,"
      "\"sample_period_s\":%u,"
      "\"created_at\":%lld,"
      "\"boot_id\":\"esp32s3-test\","
      "\"fw\":\"%s\","
      "\"hw\":\"%s\","
      "\"samples\":["
      "{\"seq\":%lld,\"t\":%lld,"
      "\"vw_f\":[1240.52,1241.52,1242.52,1243.52,1244.52,1245.52,1246.52,1247.52],"
      "\"vw_t\":[18.6,18.7,18.8,18.9,19,19.1,19.2,19.3],"
      "\"shzk_t\":[21.2,21.4,21.6,21.8,22,22.2,22.4,22.6,22.8,23,23.2,23.4,23.6,23.8,24,24.2],"
      "\"env\":{\"t_c\":20.8,\"rh_pct\":74.2,\"p_pa\":98420},"
      "\"pwr\":{\"vin_v\":12.41,\"iin_a\":0.083}},"
      "{\"seq\":%lld,\"t\":null,"
      "\"vw_f\":[null,null,null,null,null,null,null,null],"
      "\"vw_t\":[null,null,null,null,null,null,null,null],"
      "\"shzk_t\":[null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null],"
      "\"env\":{\"t_c\":null,\"rh_pct\":null,\"p_pa\":null},"
      "\"pwr\":{\"vin_v\":null,\"iin_a\":null}}],"
      "\"state\":{\"uptime_s\":%lu,\"system\":{\"ok\":true,\"status\":\"OK\"},"
      "\"cloud\":{\"ok\":true,\"status\":\"OK\"}},"
      "\"net\":{\"connected\":true,\"rssi\":%d},"
      "\"queue\":{\"records\":2,\"unsent_samples\":2,"
      "\"oldest_unsent_seq\":%lld,\"cursor_start\":0,\"cursor_end\":0,\"bytes\":0},"
      "\"events\":[]}",
      kBatchSchema, kBatchProfile, CLOUD_DEVICE_ID, batchId, seqFirst, seqLast,
      static_cast<unsigned>(kSamplePeriodSeconds), now, kFirmwareVersion,
      kHardwareVersion, seqFirst, now, seqLast, uptimeSeconds, rssi, seqFirst);

  if (bodyLen <= 0 || static_cast<size_t>(bodyLen) >= bodyCap) {
    Serial.println("batch: JSON body too long");
    return false;
  }

  if (bodyLenOut != nullptr) {
    *bodyLenOut = static_cast<size_t>(bodyLen);
  }

  if (seqLastOut != nullptr) {
    *seqLastOut = seqLast;
  }
  return true;
}

void commandPayload() {
  if (!ensureWifi()) return;
  if (!ensureTime()) return;

  char* body = allocateBatchBody(kBatchBodyCap);
  if (body == nullptr) return;

  char batchId[96];
  size_t bodyLen = 0;
  long long seqLast = 0;
  if (!buildBatch(body, kBatchBodyCap, batchId, sizeof(batchId), &bodyLen,
                  &seqLast)) {
    freeBatchBody(body);
    return;
  }

  Serial.print("batch_id=");
  Serial.println(batchId);
  Serial.printf("accepted_seq_last=%lld\n", seqLast);
  Serial.println(body);
  freeBatchBody(body);
}

void commandBatch() {
  if (!ensureWifi()) return;
  if (!ensureTime()) return;

  char* body = allocateBatchBody(kBatchBodyCap);
  if (body == nullptr) return;

  char batchId[96];
  size_t bodyLen = 0;
  long long seqLast = 0;
  if (!buildBatch(body, kBatchBodyCap, batchId, sizeof(batchId), &bodyLen,
                  &seqLast)) {
    freeBatchBody(body);
    return;
  }

  String response;
  const int code =
      httpsRequest("POST", CLOUD_INGEST_URL, body, bodyLen, &response, true);
  freeBatchBody(body);
  Serial.printf("batch: http=%d batch_id=%s accepted_seq_last=%lld\n", code,
                batchId, seqLast);
  Serial.println(response);
  if (code >= 200 && code < 300 &&
      response.indexOf("\"status\":\"accepted\"") >= 0) {
    Serial.println("batch: OK");
  } else {
    Serial.println("batch: FAIL");
  }
}

void dispatch(char* line) {
  while (*line == ' ' || *line == '\t') ++line;
  if (*line == '\0') return;

  if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
    printHelp();
  } else if (strcmp(line, "wifi") == 0) {
    commandWifi();
  } else if (strcmp(line, "time") == 0) {
    commandTime();
  } else if (strcmp(line, "status") == 0 || strcmp(line, "conn") == 0) {
    commandStatus();
  } else if (strcmp(line, "health") == 0) {
    commandHealth();
  } else if (strcmp(line, "ready") == 0) {
    commandReadiness();
  } else if (strcmp(line, "payload") == 0) {
    commandPayload();
  } else if (strcmp(line, "batch") == 0 || strcmp(line, "sample") == 0) {
    commandBatch();
  } else {
    Serial.print("unknown command: ");
    Serial.println(line);
    printHelp();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("ESP32-S3 WiFi Cloudflare Batch PoC");
  printHelp();
}

void loop() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      dispatch(lineBuf);
      lineLen = 0;
    } else if (lineLen + 1U < sizeof(lineBuf)) {
      lineBuf[lineLen++] = c;
    }
  }
}
