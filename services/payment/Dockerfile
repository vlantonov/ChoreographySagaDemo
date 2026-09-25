# syntax=docker/dockerfile:1
# Multi-stage build for the Payment service (C-12, tech-stack §13).

# ---- build stage: install into a self-contained venv with uv ----
FROM python:3.12-slim AS build
COPY --from=ghcr.io/astral-sh/uv:latest /uv /bin/uv
ENV UV_LINK_MODE=copy \
    VIRTUAL_ENV=/opt/venv \
    PATH="/opt/venv/bin:$PATH"
WORKDIR /src
RUN uv venv "$VIRTUAL_ENV"
# Copy metadata first so dependency install layers cache across source edits.
COPY pyproject.toml README.md ./
COPY src ./src
RUN uv pip install --no-cache .

# ---- runtime stage ----
FROM python:3.12-slim AS runtime
# librdkafka (confluent-kafka) and libpq (psycopg[binary]) ship inside the
# wheels; only CA certificates are needed for OTLP/gRPC + Kafka TLS.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/venv /opt/venv
ENV VIRTUAL_ENV=/opt/venv \
    PATH="/opt/venv/bin:$PATH" \
    PYTHONUNBUFFERED=1
WORKDIR /app
# Migrations shipped for the Release Engineer's migrate job.
COPY migrations /app/migrations
RUN useradd --system --uid 10001 payment
USER payment
ENTRYPOINT ["python", "-m", "payment"]
