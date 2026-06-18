# ESP32-S3 WiFi Cloudflare Batch Ingest PoC

Small proof repository for an ESP32-S3-WROOM-N16R8 using Arduino, WiFi station mode, and a USB serial CLI to POST an HMAC-signed `tm.batch.v1` payload to a Cloudflare Worker.

This intentionally does not include real sensors, SD logging, GSM, display, RS485, or replay logic. The payload uses fixed test measurements and current device time.

## Firmware

1. Copy the placeholder secrets file and edit local WiFi credentials plus the device HMAC secret issued by the Worker admin API:

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

`payload` prints the JSON that will be sent. `batch` sends it to the Worker with the current HMAC ingest envelope:

```http
X-TM-Device: tm-test-mcu-1
X-TM-Timestamp: <unix_seconds>
X-TM-Content-SHA256: <lowercase_sha256_hex_of_raw_body>
X-TM-Signature: v1=<lowercase_hmac_sha256_hex>
```

The signed canonical string is `tm-hmac-v1\nPOST\n/api/v1/ingest\n<device_id>\n<unix_seconds>\n<body_sha256>` with no trailing newline. The HMAC key is `CLOUD_DEVICE_HMAC_SECRET` from `include/secrets.h`; do not put that secret in the JSON body or in any HTTP header.

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

The current Worker implementation lives in the sibling repository:

```text
C:\Users\Honza\Documents\Projects\TunnelMonitor-Cloudflare
```

Its active MCU contract is documented in `docs/ingest-contract-tm-batch-v1.md` in that repository. The important firmware-facing change versus the older proof is that ingest no longer accepts `X-TM-Token`; every POST must be HMAC-signed.
