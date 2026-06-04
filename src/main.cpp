#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstdint>
#include <cstring>
#include <time.h>

#include "CloudConfig.h"
#include "secrets.h"

#include "mbedtls/md.h"

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

void bytesToHex(const unsigned char* in, size_t len, char* out, size_t outCap) {
  static constexpr char kHex[] = "0123456789abcdef";
  if (outCap < (len * 2U + 1U)) {
    if (outCap > 0) out[0] = '\0';
    return;
  }
  for (size_t i = 0; i < len; ++i) {
    out[i * 2U] = kHex[(in[i] >> 4) & 0x0F];
    out[i * 2U + 1U] = kHex[in[i] & 0x0F];
  }
  out[len * 2U] = '\0';
}

bool sha256Hex(const char* text, char* out, size_t outCap) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) return false;
  unsigned char digest[32];
  const int rc = mbedtls_md(
      info, reinterpret_cast<const unsigned char*>(text), strlen(text), digest);
  if (rc != 0) return false;
  bytesToHex(digest, sizeof(digest), out, outCap);
  return out[0] != '\0';
}

bool hmacSha256Hex(const char* secret, const char* text, char* out,
                   size_t outCap) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) return false;
  unsigned char digest[32];
  const int rc = mbedtls_md_hmac(
      info, reinterpret_cast<const unsigned char*>(secret), strlen(secret),
      reinterpret_cast<const unsigned char*>(text), strlen(text), digest);
  if (rc != 0) return false;
  bytesToHex(digest, sizeof(digest), out, outCap);
  return out[0] != '\0';
}

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
                 String* responseOut) {
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
    http.addHeader("Content-Type", "application/json");
    code = http.POST(String(body));
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
  Serial.println("  vector");
  Serial.println("  sample");
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
  const int code = httpsRequest("GET", CLOUD_HEALTH_URL, nullptr, &response);
  Serial.printf("health: http=%d\n", code);
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
  const int code = httpsRequest("GET", CLOUD_HEALTH_URL, nullptr, &response);
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

bool buildSample(char* body, size_t bodyCap, char* hashOut, size_t hashCap,
                 char* sigOut, size_t sigCap) {
  static constexpr const char* kSchema = "tm.sample.v1";
  static constexpr const char* kChannel = "cloud.test";
  static constexpr const char* kUnit = "bool";
  static constexpr const char* kQuality = "ok";

  char canonical[384];
  const int canonicalLen = snprintf(
      canonical, sizeof(canonical), "%s\n%s\n%s\n1\n%s\n%s\n1\n%s\n%s\n%s\n%s",
      kSchema, CLOUD_DEVICE_ID, CLOUD_SAMPLE_ID, CLOUD_TIMESTAMP, kChannel,
      kUnit, kQuality, CLOUD_TIMESTAMP, CLOUD_NONCE);
  if (canonicalLen <= 0 ||
      static_cast<size_t>(canonicalLen) >= sizeof(canonical)) {
    Serial.println("sample: canonical too long");
    return false;
  }

  if (!sha256Hex(canonical, hashOut, hashCap)) {
    Serial.println("sample: sha256 failed");
    return false;
  }
  if (!hmacSha256Hex(CLOUD_HMAC_SECRET, canonical, sigOut, sigCap)) {
    Serial.println("sample: hmac failed");
    return false;
  }

  const int bodyLen = snprintf(
      body, bodyCap,
      "{\"schema\":\"%s\","
      "\"device_id\":\"%s\","
      "\"sample_id\":\"%s\","
      "\"seq\":1,"
      "\"ts\":\"%s\","
      "\"channel\":\"%s\","
      "\"value\":1,"
      "\"unit\":\"%s\","
      "\"quality\":\"%s\","
      "\"auth\":{\"t\":\"%s\",\"n\":\"%s\",\"h\":\"%s\",\"s\":\"%s\"}}",
      kSchema, CLOUD_DEVICE_ID, CLOUD_SAMPLE_ID, CLOUD_TIMESTAMP, kChannel,
      kUnit, kQuality, CLOUD_TIMESTAMP, CLOUD_NONCE, hashOut, sigOut);
  if (bodyLen <= 0 || static_cast<size_t>(bodyLen) >= bodyCap) {
    Serial.println("sample: JSON body too long");
    return false;
  }
  return true;
}

void commandVector() {
  char body[768];
  char hash[65];
  char sig[65];
  if (!buildSample(body, sizeof(body), hash, sizeof(hash), sig, sizeof(sig))) {
    return;
  }
  Serial.print("h=");
  Serial.println(hash);
  Serial.print("s=");
  Serial.println(sig);
  Serial.println(body);
}

void commandSample() {
  char body[768];
  char hash[65];
  char sig[65];
  if (!buildSample(body, sizeof(body), hash, sizeof(hash), sig, sizeof(sig))) {
    return;
  }

  String response;
  const int code = httpsRequest("POST", CLOUD_INGEST_URL, body, &response);
  Serial.printf("sample: http=%d h=%s s=%s\n", code, hash, sig);
  Serial.println(response);

  if (code >= 200 && code < 300 &&
      (response.indexOf("\"accepted\"") >= 0 ||
       response.indexOf("\"duplicate\"") >= 0)) {
    Serial.println("sample: OK");
  } else {
    Serial.println("sample: FAIL");
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
  } else if (strcmp(line, "vector") == 0) {
    commandVector();
  } else if (strcmp(line, "sample") == 0) {
    commandSample();
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
  Serial.println("ESP32-S3 WiFi Cloudflare PoC");
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
