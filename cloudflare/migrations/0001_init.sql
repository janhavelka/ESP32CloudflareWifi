CREATE TABLE IF NOT EXISTS devices (
  device_id TEXT PRIMARY KEY,
  hmac_secret TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS ingest_samples (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  sample_id TEXT NOT NULL,
  seq INTEGER NOT NULL,
  ts TEXT NOT NULL,
  channel TEXT NOT NULL,
  value_text TEXT NOT NULL,
  unit TEXT NOT NULL,
  quality TEXT NOT NULL,
  r2_key TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE(device_id, sample_id)
);

INSERT OR REPLACE INTO devices (device_id, hmac_secret, enabled)
VALUES (
  'tm-dev-001',
  '36dca50bf39fe7d1e474f0f33ddb6fc743e705beacdf02dbb034f0939adfd7b5',
  1
);
