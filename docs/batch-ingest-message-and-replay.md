# Batch Ingest Message And Replay Layout

## Transport

Data is sent as compact JSON text over HTTPS:

```text
POST https://tunnel-monitor.jnhavelka.workers.dev/api/v1/ingest
```

The device sends one HTTP POST per batch. Each batch contains 1 to 16 samples.

## Headers

Required request headers:

```http
Content-Type: application/json
X-TM-Device: tm-test-mcu-1
X-TM-Timestamp: <unix_seconds>
X-TM-Content-SHA256: <lowercase_sha256_hex_of_raw_body>
X-TM-Signature: v1=<lowercase_hmac_sha256_hex>
```

Notes:

- `X-TM-Device` must match the JSON `device_id`.
- `X-TM-Timestamp` is Unix seconds and must be within the Worker's five-minute clock-skew window.
- `X-TM-Content-SHA256` is SHA-256 over the exact raw UTF-8 JSON body being sent.
- `X-TM-Signature` is an HMAC-SHA256 signature using the device's registered HMAC secret as UTF-8 key material.
- The raw HMAC secret is never sent in JSON or HTTP headers and must not be logged.

The signed canonical string is:

```text
tm-hmac-v1
POST
/api/v1/ingest
<device_id>
<unix_seconds>
<lowercase_sha256_hex_of_raw_body>
```

Do not add a trailing newline. For every send attempt, compute:

```text
content_sha256 = SHA256(raw_json)
signature = "v1=" + HMAC_SHA256_HEX(device_hmac_secret, canonical_string)
```

For a retry, reuse the exact same `raw_json` and recompute `X-TM-Timestamp`, `X-TM-Content-SHA256`, and `X-TM-Signature` from that body. The content hash stays the same if the body is unchanged; the timestamp and signature normally change.

## Batch Envelope

Every POST body is one JSON object:

```json
{
  "schema": "tm.batch.v1",
  "profile": "tm.v1.vw8_shzk16_env_power",
  "device_id": "tm-test-mcu-1",
  "batch_id": "tm-test-mcu-1-1000-1015",
  "seq_first": 1000,
  "seq_last": 1015,
  "sample_period_s": 900,
  "created_at": 1781360000,
  "boot_id": "boot-0001",
  "fw": "1.0.0",
  "hw": "esp32-s3",
  "samples": [],
  "state": {},
  "net": {},
  "queue": {},
  "events": []
}
```

Important fields:

| Field | Meaning |
|---|---|
| `batch_id` | Idempotency key. Reusing it with changed JSON causes HTTP 409. |
| `seq_first` | First sample sequence in the batch. |
| `seq_last` | Last sample sequence in the batch. |
| `created_at` | Unix seconds when the batch was assembled. |
| `samples` | Array of sample objects. |
| `state`, `net`, `queue`, `events` | Diagnostics captured at batch assembly time. |

## Sample Object

Each saved sample should be serializable to this shape:

```json
{
  "seq": 1000,
  "t": 1781360000,
  "vw_f": [1240.52, 1241.52, 1242.52, 1243.52, 1244.52, 1245.52, 1246.52, 1247.52],
  "vw_t": [18.6, 18.7, 18.8, 18.9, 19.0, 19.1, 19.2, 19.3],
  "shzk_t": [21.2, 21.4, 21.6, 21.8, 22.0, 22.2, 22.4, 22.6, 22.8, 23.0, 23.2, 23.4, 23.6, 23.8, 24.0, 24.2],
  "env": {
    "t_c": 20.8,
    "rh_pct": 74.2,
    "p_pa": 98420
  },
  "pwr": {
    "vin_v": 12.41,
    "iin_a": 0.083
  }
}
```

Use JSON `null` only for a channel value that was genuinely missing:

```json
"vw_f": [1240.52, null, 1242.52, null, null, null, null, null]
```

Do not add an all-null placeholder sample unless the backend should preserve that missing sample as an empty record.

## Firmware Sample Storage

Store samples in a replay queue before trying to upload them.

Recommended per-sample fields:

| Field | Suggested type | Notes |
|---|---:|---|
| `seq` | `uint64_t` or persistent monotonic integer | Never reuse after ack. |
| `t` | Unix seconds from RTC, nullable | Use `null` only if time is invalid/unavailable. |
| `vw_f[8]` | float/double plus validity bits | Serialize invalid values as JSON `null`. |
| `vw_t[8]` | float/double plus validity bits | Same. |
| `shzk_t[16]` | float/double plus validity bits | Same. |
| `env_t_c` | float/double plus valid bit | JSON `env.t_c`. |
| `env_rh_pct` | float/double plus valid bit | JSON `env.rh_pct`. |
| `env_p_pa` | float/double plus valid bit | JSON `env.p_pa`. |
| `pwr_vin_v` | float/double plus valid bit | JSON `pwr.vin_v`. |
| `pwr_iin_a` | float/double plus valid bit | JSON `pwr.iin_a`. |
| `sample_status` | enum | `queued`, `batched`, `acked`, etc. |

The queue should preserve samples across resets and power loss.

## Pending Batch Storage

For reliable retry, store the exact batch body before the first POST.

Recommended pending-batch fields:

| Field | Purpose |
|---|---|
| `batch_id` | Stable idempotency key. |
| `seq_first` | First sequence in this batch. |
| `seq_last` | Last sequence in this batch. |
| `raw_json` | Exact UTF-8 JSON body to resend on retry. |
| `created_at` | Batch assembly time. |
| `attempt_count` | Retry tracking. |
| `last_attempt_at` | Retry/backoff tracking. |

Critical rule:

```text
If a batch was sent but not acknowledged, retry the exact same raw_json with the same batch_id.
```

Do not rebuild the JSON for a retry. Rebuilding can change timestamps, diagnostics, float formatting, field order, or whitespace. If the `batch_id` stays the same but the body changes, the Worker can reject it as a duplicate conflict. HMAC headers are transport metadata and should be regenerated for each attempt from the stored `raw_json`.

## Batch Assembly Flow

1. Read queued, unacknowledged samples.
2. Pick up to 16 samples.
3. Prefer contiguous sequence ranges.
4. Set:

```text
seq_first = first selected sample seq
seq_last = last selected sample seq
batch_id = <device_id>-<seq_first>-<seq_last>
created_at = current RTC Unix seconds
```

5. Serialize the full JSON batch.
6. Save the pending batch record with exact `raw_json`.
7. POST the saved `raw_json` over HTTPS with fresh HMAC auth headers.
8. On HTTP 200 `status: accepted`, mark all samples up to `accepted_seq_last` as acknowledged and delete the pending batch.
9. On network failure or HTTP 5xx, keep the pending batch and retry the same `raw_json` with a new timestamp/signature.
10. On HTTP 400, treat as firmware/schema bug.
11. On HTTP 401/403, treat as credential/device registration error.
12. On HTTP 409, treat as batch id reused with changed contents.

## Worker Limits

Current Worker limits:

```text
Max raw JSON body: 64 KiB
Samples per batch: 1 to 16
Max seq_last - seq_first: 255
Max device_id / boot_id length: 128
Max batch_id length: 192
Max fw / hw length: 64
Max diagnostic keys per object: 64
Max diagnostic JSON size: 8 KiB
Max events: 32
Max events JSON size: 8 KiB
```

## Message Size Notes

The payload is JSON text, not binary. Numeric firmware values become ASCII/UTF-8 JSON numbers:

```text
uint64 seq 1000      -> "seq":1000
float vin 12.41     -> "vin_v":12.41
missing value       -> null
```

The batch envelope is fixed overhead paid once per request. Adding more samples grows the JSON by only the per-sample object size, so a 16-sample batch is much more efficient than 16 one-sample HTTPS requests.

Observed POC sizes:

```text
2-sample compact batch: about 1.35 KiB JSON
16-sample compact batch: about 5.6 KiB JSON
```

TLS handshake overhead is usually larger than a small telemetry body, so batching samples is the correct direction.
