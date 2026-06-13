# ESP32 Batch Ingest POC Handoff

## Purpose

This repository is currently a proof of concept showing that an ESP32-S3 can connect over WiFi/TLS to the Cloudflare Worker in `docs/worker/worker.md` and successfully send authenticated `tm.batch.v1` data.

This is not final production firmware. The current code uses generated fake measurements, SNTP time, and simple serial commands. The real firmware should replace those pieces with the existing sensor pipeline, queue/retry storage, and the device RTC module.

## Worker Target

- Worker base URL: `https://tunnel-monitor.jnhavelka.workers.dev`
- Health: `GET /health`
- Readiness: `GET /health/ready`
- Ingest: `POST /api/v1/ingest`
- Admin UI: `GET /admin`
- Admin API: `/api/v1/admin/*`, protected by `X-TM-Admin-Token`

The active Worker accepts batch ingest, not the older sample/HMAC protocol in `cloudflare/src/index.ts`.

## Test Device

- Device id: `tm-test-mcu-1`
- Device token: stored locally in ignored `include/secrets.h` as `CLOUD_DEVICE_TOKEN`
- Do not put the token into tracked files.

Ingest auth uses headers:

```http
Content-Type: application/json
X-TM-Device: tm-test-mcu-1
X-TM-Token: <raw device token>
```

The Worker hashes the supplied token and compares it with the registered device token hash. The new batch protocol does not use per-payload HMAC signing.

## Payload Contract

Top-level schema:

```text
schema: "tm.batch.v1"
profile: "tm.v1.vw8_shzk16_env_power"
device_id: must match X-TM-Device
batch_id: unique per device, idempotency key
seq_first: first sequence in batch
seq_last: last sequence in batch
sample_period_s: integer
created_at: Unix seconds
boot_id: string
fw: firmware version string
hw: hardware version string
samples: array, min 1
state: diagnostic object
net: diagnostic object
queue: diagnostic object
events: array
```

Each sample requires:

```text
seq: integer within seq_first..seq_last
t: Unix seconds or null
vw_f: 8 values, number or null
vw_t: 8 values, number or null
shzk_t: 16 values, number or null
env: { t_c, rh_pct, p_pa }, number or null
pwr: { vin_v, iin_a }, number or null
```

The Worker expands each sample into measurement rows. Null-only samples currently become empty measurement records in the UI/database, which is expected for this POC but probably not desired for production.

## Current Firmware POC

Main files:

- `src/main.cpp`
- `include/CloudConfig.h`
- `include/secrets.h`
- `include/secrets.example.h`

Serial commands:

```text
wifi     Connect and print WiFi state
time     Sync/check SNTP time
status   WiFi + DNS + time + HTTPS health check
conn     Alias for status
health   GET /health
ready    GET /health/ready
payload  Print a generated tm.batch.v1 JSON body only
batch    Generate and POST a new batch
sample   Alias for batch for compatibility
```

Important implementation notes:

- TLS uses the ESP-IDF CA bundle through `WiFiClientSecure::setCACertBundle`.
- Time is currently obtained through SNTP using `pool.ntp.org`, `time.cloudflare.com`, and `time.nist.gov`.
- `payload` does not cache or send anything.
- `batch` always generates a fresh batch id and fresh sequence range from current Unix time.
- The POC sends 2 samples per batch: one full fake measurement sample and one null placeholder sample.

## Verified Results

PC tests and ESP32 serial tests confirmed:

- WiFi connection works.
- DNS resolves `tunnel-monitor.jnhavelka.workers.dev`.
- TLS certificate validation works.
- `/health` returns HTTP 200.
- `/health/ready` returns database and storage reachable.
- `POST /api/v1/ingest` returns HTTP 200 with `status: accepted`.
- Duplicate replay from the PC script is idempotently accepted.
- Duplicate conflict returns HTTP 409.
- Invalid payload returns HTTP 400.

Observed accepted test batches for `tm-test-mcu-1`:

```text
tm-test-mcu-1-1781354823-1781354824   PC script
tm-test-mcu-1-1781355091-1781355092   PC script
tm-test-mcu-1-1781360286-1781360287   ESP32
```

That equals 3 batches and 6 sample rows. The `seq_last` rows are empty because the second sample in each POC batch is intentionally null.

## Production Firmware Guidance

Replace the POC batch builder with real firmware integration:

- Use the real RTC module for `created_at` and sample `t`.
- Use monotonic persistent sequence numbers, not Unix time as sequence.
- Generate stable batch ids from device id and sequence range.
- Only include real samples that should become measurement rows.
- Do not send null placeholder samples unless the backend should preserve an explicit missing-sample record.
- Fill channel arrays from real vibrating wire, temperature, SHZK, environment, and power readings.
- Fill `state`, `net`, `queue`, and `events` from real diagnostics.
- Add persistent queue/retry logic that can replay the exact same raw JSON for an unacknowledged batch.
- Treat HTTP 200 accepted as the durable ack for all samples through `accepted_seq_last`.
- Treat HTTP 401/403 as credential/config errors.
- Treat HTTP 400 as firmware payload bug or schema mismatch.
- Treat HTTP 409 as duplicate batch id conflict caused by reusing a batch id with changed contents.
- Retry transient network/5xx failures without changing the batch body or batch id.

## Build

The firmware builds with:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e tunnelmonitor_s3_wifi_cloud_poc
```

`pio` may not be on PATH; the full PlatformIO path above worked in this environment.
