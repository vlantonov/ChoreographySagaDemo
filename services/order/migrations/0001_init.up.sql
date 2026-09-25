-- Order service schema (ARCHITECTURE §4.1, tech-stack §8).

CREATE TABLE IF NOT EXISTS orders (
    id           UUID PRIMARY KEY,
    customer_id  TEXT        NOT NULL,
    sku          TEXT        NOT NULL,
    quantity     INT         NOT NULL,
    amount       NUMERIC     NOT NULL,
    status       TEXT        NOT NULL,
    payment_id   UUID,
    reservation_id UUID,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
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
