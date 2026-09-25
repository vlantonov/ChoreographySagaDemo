-- Payment service schema (ARCHITECTURE §4.2, tech-stack §8).
-- Applied by the Release Engineer's migrate job (plain SQL, mirrors the Order
-- service migration style).

CREATE TABLE IF NOT EXISTS payments (
    id         UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    saga_id    UUID        NOT NULL UNIQUE,
    amount     NUMERIC     NOT NULL,
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
