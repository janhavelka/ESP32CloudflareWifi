# ESP32-S3 WiFi Cloudflare Batch Ingest PoC

Small proof repository for an ESP32-S3-WROOM-N16R8 using Arduino, WiFi station mode, and a USB serial CLI to POST a token-authenticated `tm.batch.v1` payload to a Cloudflare Worker.

This intentionally does not include real sensors, SD logging, GSM, display, RS485, or replay logic. The payload uses fixed test measurements and current device time.

## Firmware

1. Copy the WiFi placeholder file and edit local credentials:

   ```bash
   cp include/secrets.example.h include/secrets.h
   ```

2. Build and upload:

   ```bash
   pio run -e tunnelmonitor_s3_wifi_cloud_poc
   pio run -e tunnelmonitor_s3_wifi_cloud_poc -t upload
   pio device monitor -e tunnelmonitor_s3_wifi_cloud_poc
   ```

3. In the serial monitor at 115200 baud:

   ```text
   wifi
   time
   status
   health
   ready
   payload
   batch
   batch
   ```

`payload` prints the JSON that will be sent. `batch` sends it to the Worker with `X-TM-Device` and `X-TM-Token` headers.

HTTPS uses the ESP-IDF x509 certificate bundle through Arduino
`WiFiClientSecure::setCACertBundle(...)`. The device must sync UTC time with
SNTP before TLS certificate validation can succeed.

Use `status` or `conn` to check real cloud connectivity. It reports the WiFi
association, IP/DNS/gateway details, SNTP clock state, DNS resolution for the
Cloudflare Worker host, and an HTTPS `/health` probe. This catches cases where
the ESP32 is connected to a WiFi router but the router's GSM/LTE uplink is down.

## Cloudflare Worker

The firmware targets:

```text
https://tunnel-monitor.jnhavelka.workers.dev/api/v1/ingest
```

The attached bundled Worker in `docs/worker/worker.md` exposes `TunnelMonitor` version `0.6.0`, public health/readiness endpoints, `/api/v1/ingest`, `/admin`, and admin API routes under `/api/v1/admin/*`.

The checked-in `cloudflare/src` folder still contains the earlier single-sample HMAC Worker. Use the attached batch Worker, or replace the local Worker source before deploying this newer protocol.

Repeatable PowerShell upload tests from the PC:

```powershell
$env:TM_DEVICE_TOKEN = "<device token>"

.\cloudflare\scripts\Send-IngestTests.ps1 `
  -Mode Batch `
  -BaseUrl "https://tunnel-monitor.jnhavelka.workers.dev" `
  -DeviceId "tm-test-mcu-1" `
  -Seq 1234 `
  -New 1 `
  -Dupes 1 `
  -Conflict `
  -InvalidMissingField
```
