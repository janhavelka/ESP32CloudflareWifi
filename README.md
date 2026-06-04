# ESP32-S3 WiFi Cloudflare Ingest PoC

Small proof repository for an ESP32-S3-WROOM-N16R8 using Arduino, WiFi station mode, and a USB serial CLI to POST one HMAC-signed sample to a Cloudflare Worker.

This intentionally does not include sensors, SD logging, GSM, display, RS485, batching, or replay logic.

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
   vector
   sample
   sample
   ```

Expected `vector` output:

```text
h=42eccaf49e970e85163ffebf92b4f28b28e838ae533323406c16465e5918564f
s=b4fd8c0a886b09137f25534078bdc8fc5abef4f159076d7fe7b30f71e217fe9b
```

Repeated uploads of the fixed sample may return `duplicate`; that is valid.

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
https://tm-dev-ingest.jnhavelka.workers.dev/v1/ingest
```

Deploying the Worker name `tm-dev-ingest` in the same Cloudflare account will update that existing workers.dev route.

Setup:

```bash
cd cloudflare
npm install
npx wrangler login
npx wrangler d1 create tm-dev-ingest-db
npx wrangler r2 bucket create tm-dev-ingest-raw
```

Paste the generated D1 `database_id` into `cloudflare/wrangler.jsonc`, then run:

```bash
npx wrangler d1 migrations apply DB --local
npx wrangler d1 migrations apply DB --remote
npm run deploy
```

PC POST test:

```bash
curl -i \
  -X POST "https://tm-dev-ingest.jnhavelka.workers.dev/v1/ingest" \
  -H "Content-Type: application/json" \
  --data '{"schema":"tm.sample.v1","device_id":"tm-dev-001","sample_id":"tm-dev-001-manual-test-000001","seq":1,"ts":"2026-06-04T06:30:00Z","channel":"cloud.test","value":1,"unit":"bool","quality":"ok","auth":{"t":"2026-06-04T06:30:00Z","n":"tm-dev-001-manual-test-000001","h":"42eccaf49e970e85163ffebf92b4f28b28e838ae533323406c16465e5918564f","s":"b4fd8c0a886b09137f25534078bdc8fc5abef4f159076d7fe7b30f71e217fe9b"}}'
```

Check remote D1:

```bash
npx wrangler d1 execute DB --remote --command "SELECT device_id, sample_id, seq, r2_key, created_at FROM ingest_samples ORDER BY id DESC LIMIT 5"
```

Check remote R2:

```bash
npx wrangler r2 object get tm-dev-ingest-raw/raw/tm-dev-001/tm-dev-001-manual-test-000001.json --remote
```
