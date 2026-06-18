# ESP32 Batch Ingest Worker Handoff

## Purpose

This document summarizes the proven Cloudflare Worker ingest contract for a real ESP32 firmware implementation.

The existing proof of concept only demonstrated that the ESP32 can connect over WiFi/TLS and send authenticated `tm.batch.v1` data successfully. Production firmware should use its own sensor pipeline, persistent queue, retry logic, and RTC module.

## Worker Target

- Worker base URL: `https://tunnel-monitor.jnhavelka.workers.dev`
- Health: `GET /health`
- Readiness: `GET /health/ready`
- Ingest: `POST /api/v1/ingest`
- Admin UI: `GET /admin`
- Admin API: `/api/v1/admin/*`, protected by `X-TM-Admin-Token`

The active Worker accepts batch ingest and requires HMAC request signing on every ingest POST.

## Test Device

- Device id: `tm-test-mcu-1`
- Device HMAC secret: create or rotate it through the Worker admin API, store it only in local firmware secrets, and do not commit it.

Ingest auth uses headers:

```http
Content-Type: application/json
X-TM-Device: tm-test-mcu-1
X-TM-Timestamp: <unix_seconds>
X-TM-Content-SHA256: <lowercase_sha256_hex_of_raw_body>
X-TM-Signature: v1=<lowercase_hmac_sha256_hex>
```

The Worker verifies the signature before parsing or archiving the request body. The signature is HMAC-SHA256 using the registered device HMAC secret over this canonical string, with no trailing newline:

```text
tm-hmac-v1
POST
/api/v1/ingest
<device_id>
<unix_seconds>
<lowercase_sha256_hex_of_raw_body>
```

The raw HMAC secret is never sent to the Worker. Existing devices from the earlier token-authenticated proof need a registered `hmac_secret` before they can ingest.

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

## Acceptance Checks

With a registered active device and matching HMAC secret, firmware and workstation ingest tests should confirm:

- WiFi connection works.
- DNS resolves `tunnel-monitor.jnhavelka.workers.dev`.
- TLS certificate validation works.
- `/health` returns HTTP 200.
- `/health/ready` returns database and storage reachable.
- `POST /api/v1/ingest` returns HTTP 200 with `status: accepted` when HMAC headers match a registered active device.
- Duplicate replay of the exact same raw JSON is idempotently accepted when the retry recomputes fresh HMAC headers.
- Duplicate conflict returns HTTP 409.
- Invalid payload returns HTTP 400.

## Production Firmware Guidance

Implement the real firmware around this contract:

- Use the real RTC module for `created_at` and sample `t`.
- Use monotonic persistent sequence numbers, not Unix time as sequence.
- Generate stable batch ids from device id and sequence range.
- Only include real samples that should become measurement rows.
- Do not send null placeholder samples unless the backend should preserve an explicit missing-sample record.
- Fill channel arrays from real vibrating wire, temperature, SHZK, environment, and power readings.
- Fill `state`, `net`, `queue`, and `events` from real diagnostics.
- Add persistent queue/retry logic that can replay the exact same raw JSON for an unacknowledged batch.
- Recompute `X-TM-Timestamp`, `X-TM-Content-SHA256`, and `X-TM-Signature` for each upload attempt from the exact raw JSON being sent.
- Treat HTTP 200 accepted as the durable ack for all samples through `accepted_seq_last`.
- Treat HTTP 401/403 as credential/config errors.
- Treat HTTP 400 as firmware payload bug or schema mismatch.
- Treat HTTP 409 as duplicate batch id conflict caused by reusing a batch id with changed contents.
- Retry transient network/5xx failures without changing the batch body or batch id.
