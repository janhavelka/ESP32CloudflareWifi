export default {
  async fetch(request, env) {
    try {
      const url = new URL(request.url);

      if (url.pathname === "/health") {
        return json({
          ok: true,
          service: "tm-dev-ingest",
          auth: "http+hmac-body",
          time: new Date().toISOString()
        });
      }

      if (url.pathname !== "/v1/ingest" || request.method !== "POST") {
        return json({ error: "not_found" }, 404);
      }

      return await handleIngest(request, env);
    } catch (err) {
      return json(
        {
          error: "internal_error",
          detail: String(err && err.message ? err.message : err)
        },
        500
      );
    }
  }
};

async function handleIngest(request, env) {
  if (!env.DB) {
    return json({ error: "missing_db_binding" }, 500);
  }

  if (!env.RAW) {
    return json({ error: "missing_r2_binding" }, 500);
  }

  const rawBody = await request.text();

  if (rawBody.length > 8192) {
    return json({ error: "body_too_large", max_bytes: 8192 }, 413);
  }

  let parsed;
  try {
    parsed = JSON.parse(rawBody);
  } catch {
    return json({ error: "invalid_json" }, 400);
  }

  const normalized = normalizeSingleSample(parsed);
  if (!normalized.ok) {
    return json({ error: normalized.error }, 400);
  }

  const sample = normalized.sample;

  const auth = parsed.auth && typeof parsed.auth === "object" ? parsed.auth : {};
  const authTimestamp = auth.t ? String(auth.t) : "";
  const authNonce = auth.n ? String(auth.n) : "";
  const signature = auth.s ? String(auth.s).toLowerCase().replace(/^sha256=/, "") : "";

  if (!authTimestamp) {
    return json({ error: "missing_auth_timestamp" }, 401);
  }

  if (!authNonce) {
    return json({ error: "missing_auth_nonce" }, 401);
  }

  if (!signature) {
    return json({ error: "missing_auth_signature" }, 401);
  }

  const device = await env.DB
    .prepare(
      "SELECT device_id, enabled, hmac_secret, token_hash FROM devices WHERE device_id = ?"
    )
    .bind(sample.device_id)
    .first();

  if (!device) {
    return json({ error: "unknown_device", device_id: sample.device_id }, 403);
  }

  if (Number(device.enabled) !== 1) {
    return json({ error: "device_disabled", device_id: sample.device_id }, 403);
  }

  const secret = device.hmac_secret || device.token_hash || "";

  if (!secret || secret.startsWith("TEMP_") || secret.length < 16) {
    return json(
      { error: "device_secret_not_configured", device_id: sample.device_id },
      500
    );
  }

  const canonical = buildCanonicalSample(sample, authTimestamp, authNonce);
  const expectedSignature = await hmacSha256Hex(secret, canonical);

  if (!timingSafeEqualHex(expectedSignature, signature)) {
    return json({ error: "bad_signature" }, 401);
  }

  const bodyHash = await sha256Hex(rawBody);

  const existing = await env.DB
    .prepare("SELECT sample_id, r2_key FROM ingest_samples WHERE sample_id = ?")
    .bind(sample.sample_id)
    .first();

  if (existing) {
    return json({
      status: "duplicate",
      device_id: sample.device_id,
      sample_id: existing.sample_id,
      batch_id: existing.sample_id,
      r2_key: existing.r2_key
    });
  }

  const now = new Date().toISOString();
  const day = now.slice(0, 10);

  const r2Key =
    `raw/device=${safe(sample.device_id)}/day=${day}/${safe(sample.sample_id)}.json`;

  await env.RAW.put(r2Key, rawBody, {
    httpMetadata: { contentType: "application/json" },
    customMetadata: {
      device_id: sample.device_id,
      sample_id: sample.sample_id,
      body_sha256: bodyHash,
      schema: sample.schema_version || ""
    }
  });

  await env.DB.prepare(
    `
    INSERT INTO ingest_samples
    (
      sample_id,
      device_id,
      seq,
      ts,
      channel,
      value_num,
      value_text,
      unit,
      quality,
      schema_version,
      fw_version,
      r2_key,
      body_sha256,
      received_at,
      status
    )
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    `
  )
    .bind(
      sample.sample_id,
      sample.device_id,
      sample.seq,
      sample.ts,
      sample.channel,
      sample.value_num,
      sample.value_text,
      sample.unit,
      sample.quality,
      sample.schema_version,
      sample.fw_version,
      r2Key,
      bodyHash,
      now,
      "accepted"
    )
    .run();

  await env.DB.prepare(
    `
    INSERT INTO latest_device_state
    (device_id, last_seen_at, last_batch_id, last_r2_key)
    VALUES (?, ?, ?, ?)
    ON CONFLICT(device_id) DO UPDATE SET
      last_seen_at = excluded.last_seen_at,
      last_batch_id = excluded.last_batch_id,
      last_r2_key = excluded.last_r2_key
    `
  )
    .bind(sample.device_id, now, sample.sample_id, r2Key)
    .run();

  return json({
    status: "accepted",
    device_id: sample.device_id,
    sample_id: sample.sample_id,
    batch_id: sample.sample_id,
    accepted_seq: sample.seq,
    r2_key: r2Key,
    cloud_time: now
  });
}

function normalizeSingleSample(parsed) {
  const schemaVersion = String(parsed.schema || "tm.sample.v1");
  const deviceId = parsed.device_id ? String(parsed.device_id) : "";
  const sampleId = parsed.sample_id ? String(parsed.sample_id) : "";

  if (!deviceId) {
    return { ok: false, error: "missing_device_id" };
  }

  if (!sampleId) {
    return { ok: false, error: "missing_sample_id" };
  }

  const seq = Number(parsed.seq);
  if (!Number.isFinite(seq)) {
    return { ok: false, error: "missing_or_invalid_seq" };
  }

  const channel = parsed.channel ? String(parsed.channel) : "";
  if (!channel) {
    return { ok: false, error: "missing_channel" };
  }

  const value = parsed.value;
  let valueNum = null;
  let valueText = null;

  if (typeof value === "number" && Number.isFinite(value)) {
    valueNum = value;
    valueText = String(value);
  } else if (value !== undefined && value !== null) {
    valueText = String(value);
  } else {
    return { ok: false, error: "missing_value" };
  }

  return {
    ok: true,
    sample: {
      schema_version: schemaVersion,
      device_id: deviceId,
      sample_id: sampleId,
      seq,
      ts: parsed.ts ? String(parsed.ts) : "",
      channel,
      value_num: valueNum,
      value_text: valueText,
      unit: parsed.unit ? String(parsed.unit) : "",
      quality: parsed.quality ? String(parsed.quality) : "ok",
      fw_version: parsed.fw_version ? String(parsed.fw_version) : ""
    }
  };
}

/*
  Firmware must build exactly this same canonical string:

  schema
  device_id
  sample_id
  seq
  ts
  channel
  value_text
  unit
  quality
  auth.t
  auth.n

  HMAC:
  hex(HMAC_SHA256(device_secret_as_text, canonical_string))

  Important:
  - Do not include auth.s in the canonical string.
  - The 64-character secret is treated as a text string, not decoded hex bytes.
*/
function buildCanonicalSample(sample, authTimestamp, authNonce) {
  return [
    sample.schema_version,
    sample.device_id,
    sample.sample_id,
    String(sample.seq),
    sample.ts,
    sample.channel,
    sample.value_text,
    sample.unit,
    sample.quality,
    authTimestamp,
    authNonce
  ].join("\n");
}

function json(data, status = 200) {
  return new Response(JSON.stringify(data, null, 2), {
    status,
    headers: { "content-type": "application/json; charset=utf-8" }
  });
}

function safe(s) {
  return String(s).replace(/[^a-zA-Z0-9._=-]/g, "_");
}

async function sha256Hex(text) {
  const data = new TextEncoder().encode(text);
  const hash = await crypto.subtle.digest("SHA-256", data);
  return hex(new Uint8Array(hash));
}

async function hmacSha256Hex(secret, message) {
  const enc = new TextEncoder();

  const key = await crypto.subtle.importKey(
    "raw",
    enc.encode(secret),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"]
  );

  const sig = await crypto.subtle.sign("HMAC", key, enc.encode(message));
  return hex(new Uint8Array(sig));
}

function hex(bytes) {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function timingSafeEqualHex(a, b) {
  if (typeof a !== "string" || typeof b !== "string") return false;
  if (a.length !== b.length) return false;

  let out = 0;

  for (let i = 0; i < a.length; i++) {
    out |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }

  return out === 0;
}