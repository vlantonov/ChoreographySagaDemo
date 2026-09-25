# Inventory Service (C++20)

The Inventory service owns stock and reservations and provides the **single
synchronous gRPC leg** of the choreography saga (tech-stack §6, ARCHITECTURE §7):

- `InventoryService.ReserveStock` — called by Payment after a successful charge.
  Within one DB transaction it checks/decrements stock and emits
  `inventory.reserved`; the poison SKU `SKU-DEADBEEF` (or insufficient stock)
  returns `INSUFFICIENT_STOCK` and emits `inventory.reservation_failed`.
- `InventoryService.ReleaseStock` — compensation: restores stock, marks the
  reservation `RELEASED`, and emits `inventory.released`.
- Kafka consumer for `order.cancelled` — compensation trigger that releases the
  saga's reservation idempotently.

All event publishing goes through a transactional **outbox** relayed with
`FOR UPDATE SKIP LOCKED`, mirroring the Order (Go) and Payment (Python) services.
Idempotency uses `processed_messages` and the unique `reservations.saga_id`.

## Layout

```
services/inventory/
├── CMakeLists.txt        # CMake ≥ 3.25; vcpkg manifest mode (tech-stack §13)
├── vcpkg.json            # grpc, protobuf, librdkafka, libpqxx, opentelemetry-cpp, gtest, nlohmann-json
├── Dockerfile            # multi-stage (vcpkg build → slim runtime)
├── include/inventory/    # public headers
├── src/                  # implementation (core + server-only adapters)
├── test/                 # GoogleTest unit tests with in-memory fakes
└── migrations/           # 0001_init.{up,down}.sql
```

The **core** library (`domain`, `envelope`, `outbox` relay, application
`service`, structured logging) has no gRPC/Kafka/Postgres/OTel dependency, so the
unit tests run without any live infrastructure. The **server** binary adds the
libpqxx repository, the librdkafka producer/consumer, the gRPC adapter, and the
OpenTelemetry OTLP wiring.

## Build & test

### Full build (server + tests) with vcpkg

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
ctest --test-dir build --output-on-failure
```

### Core + unit tests only (no vcpkg required)

The unit tests need only GoogleTest and nlohmann-json; CMake falls back to
`FetchContent` for nlohmann-json when it is not found:

```bash
cmake -S . -B build -G Ninja -DINVENTORY_BUILD_SERVER=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

### Container

```bash
# Build context is the repository root (the proto module is shared).
docker build -f services/inventory/Dockerfile -t inventory:dev .
```

## Configuration (environment variables)

| Variable | Default |
| --- | --- |
| `INVENTORY_DATABASE_URL` | `postgresql://inventory:inventory@inventory-db:5432/inventory` |
| `KAFKA_BROKERS` | `kafka:9092` |
| `KAFKA_CONSUMER_GROUP` | `inventory-service` |
| `GRPC_LISTEN_ADDR` | `0.0.0.0:50052` |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | `http://otel-collector:4317` |
| `OTEL_SERVICE_NAME` | `inventory` |
| `OUTBOX_RELAY_INTERVAL_MS` | `500` |
| `OUTBOX_RELAY_BATCH` | `100` |
