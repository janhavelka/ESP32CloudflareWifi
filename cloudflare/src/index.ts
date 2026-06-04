export interface Env {
  DB: D1Database;
  RAW_BUCKET: R2Bucket;
}

const enc = new TextEncoder();

function hex(bytes: ArrayBuffer): string {
  return [...new Uint8Array(bytes)]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

async function sha256Hex(text: string): Promise<string> {
  return hex(await crypto.subtle.digest("SHA-256", enc.encode(text)));
}

async function hmacSha256Hex(
  secretTextBytes: string,
  text: string,
): Promise<string> {
  const key = await crypto.subtle.importKey(
    "raw",
    enc.encode(secretTextBytes),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  return hex(await crypto.subtle.sign("HMAC", key, enc.encode(text)));
}

function timingSafeHexEqual(a: string, b: string): boolean {
  if (!/^[0-9a-f]{64}$/i.test(a) || !/^[0-9a-f]{64}$/i.test(b)) return false;
  const aa = a.toLowerCase();
  const bb = b.toLowerCase();
  let diff = 0;
  for (let i = 0; i < 64; i++) diff |= aa.charCodeAt(i) ^ bb.charCodeAt(i);
  return diff === 0;
}

function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function requireString(obj: Record<string, unknown>, key: string): string {
  const value = obj[key];
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`missing ${key}`);
  }
  return value;
}

function canonicalValue(value: unknown): string {
  if (typeof value === "number" && Number.isFinite(value)) return String(value);
  if (typeof value === "boolean") return value ? "true" : "false";
  if (typeof value === "string") return value;
  throw new Error("invalid value");
}

function buildManualCanonical(sample: Record<string, unknown>): string {
  const auth = sample.auth;
  if (typeof auth !== "object" || auth === null || Array.isArray(auth)) {
    throw new Error("missing auth");
  }
  const a = auth as Record<string, unknown>;
  return [
    requireString(sample, "schema"),
    requireString(sample, "device_id"),
    requireString(sample, "sample_id"),
    String(sample.seq),
    requireString(sample, "ts"),
    requireString(sample, "channel"),
    canonicalValue(sample.value),
    requireString(sample, "unit"),
    requireString(sample, "quality"),
    requireString(a, "t"),
    requireString(a, "n"),
  ].join("\n");
}

async function handleIngest(request: Request, env: Env): Promise<Response> {
  if (request.method !== "POST") {
    return json({ ok: false, error: "method_not_allowed" }, 405);
  }

  const contentType = request.headers.get("Content-Type") || "";
  if (!contentType.toLowerCase().includes("application/json")) {
    return json({ ok: false, error: "content_type" }, 415);
  }

  const raw = await request.text();
  if (raw.length === 0 || raw.length > 4096) {
    return json({ ok: false, error: "body_size" }, 413);
  }

  let sample: Record<string, unknown>;
  try {
    sample = JSON.parse(raw) as Record<string, unknown>;
  } catch {
    return json({ ok: false, error: "bad_json" }, 400);
  }

  try {
    if (requireString(sample, "schema") !== "tm.sample.v1") {
      return json({ ok: false, error: "schema" }, 400);
    }

    const deviceId = requireString(sample, "device_id");
    const sampleId = requireString(sample, "sample_id");
    const seq = Number(sample.seq);
    if (!Number.isSafeInteger(seq) || seq < 0) {
      return json({ ok: false, error: "seq" }, 400);
    }

    const auth = sample.auth as Record<string, unknown>;
    if (typeof auth !== "object" || auth === null || Array.isArray(auth)) {
      return json({ ok: false, error: "auth" }, 400);
    }
    const givenHash = requireString(auth, "h");
    const givenSig = requireString(auth, "s");

    const row = await env.DB.prepare(
      "SELECT hmac_secret FROM devices WHERE device_id = ? AND enabled = 1",
    ).bind(deviceId).first<{ hmac_secret: string }>();
    if (!row) {
      return json({ ok: false, error: "unknown_device" }, 401);
    }

    const canonical = buildManualCanonical(sample);
    const expectedHash = await sha256Hex(canonical);
    if (!timingSafeHexEqual(givenHash, expectedHash)) {
      return json({ ok: false, error: "bad_hash" }, 401);
    }

    const expectedSig = await hmacSha256Hex(row.hmac_secret, canonical);
    if (!timingSafeHexEqual(givenSig, expectedSig)) {
      return json({ ok: false, error: "bad_signature" }, 401);
    }

    const existing = await env.DB.prepare(
      "SELECT r2_key FROM ingest_samples WHERE device_id = ? AND sample_id = ?",
    ).bind(deviceId, sampleId).first<{ r2_key: string }>();
    if (existing) {
      return json({
        status: "duplicate",
        device_id: deviceId,
        sample_id: sampleId,
        seq,
        r2_key: existing.r2_key,
      });
    }

    const safeSampleId = sampleId.replace(/[^A-Za-z0-9_.-]/g, "_");
    const r2Key = `raw/${deviceId}/${safeSampleId}.json`;

    await env.RAW_BUCKET.put(r2Key, raw, {
      httpMetadata: { contentType: "application/json" },
      customMetadata: { device_id: deviceId, sample_id: sampleId },
    });

    await env.DB.prepare(
      `INSERT INTO ingest_samples
       (device_id, sample_id, seq, ts, channel, value_text, unit, quality, r2_key)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    ).bind(
      deviceId,
      sampleId,
      seq,
      requireString(sample, "ts"),
      requireString(sample, "channel"),
      canonicalValue(sample.value),
      requireString(sample, "unit"),
      requireString(sample, "quality"),
      r2Key,
    ).run();

    return json({
      status: "accepted",
      device_id: deviceId,
      sample_id: sampleId,
      seq,
      r2_key: r2Key,
      cloud_time: new Date().toISOString(),
    });
  } catch (err) {
    return json({
      ok: false,
      error: "invalid_request",
      detail: err instanceof Error ? err.message : "unknown",
    }, 400);
  }
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === "/health") {
      return json({
        ok: true,
        service: "tm-dev-ingest",
        cloud_time: new Date().toISOString(),
      });
    }

    if (url.pathname === "/v1/ingest") {
      return handleIngest(request, env);
    }

    return json({ ok: false, error: "not_found" }, 404);
  },
};
