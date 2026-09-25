-- Inventory service schema (ARCHITECTURE §4.3, tech-stack §8).
-- Applied by the Release Engineer's migrate job (plain SQL, mirrors the Order
-- and Payment service migration style).

CREATE TABLE IF NOT EXISTS stock (
    sku       TEXT PRIMARY KEY,
    available INT  NOT NULL,
    reserved  INT  NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS reservations (
    id         UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    saga_id    UUID        NOT NULL UNIQUE,
    sku        TEXT        NOT NULL,
    quantity   INT         NOT NULL,
    status     TEXT        NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS outbox (
    id             UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    aggregate_type TEXT        NOT NULL,
    aggregate_id   TEXT        NOT NULL,
    event_type     TEXT        NOT NULL,
    topic          TEXT        NOT NULL,
    payload        JSONB       NOT NULL,
    headers        JSONB       NOT NULL DEFAULT '{}'::jsonb,
    status         TEXT        NOT NULL DEFAULT 'PENDING',
    attempts       INT         NOT NULL DEFAULT 0,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    published_at   TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS outbox_pending_idx ON outbox (created_at) WHERE status = 'PENDING';

CREATE TABLE IF NOT EXISTS processed_messages (
    idempotency_key TEXT        NOT NULL,
    consumer        TEXT        NOT NULL,
    result_hash     TEXT,
    processed_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (idempotency_key, consumer)
);

-- Seed demo stock, including the poison SKU that forces reservation failure
-- for the compensation demo (tech-stack §10, SKU-DEADBEEF has zero stock).
INSERT INTO stock (sku, available, reserved) VALUES
    ('SKU-1',        100, 0),
    ('SKU-DEADBEEF',   0, 0)
ON CONFLICT (sku) DO NOTHING;
