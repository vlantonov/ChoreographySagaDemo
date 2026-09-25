# syntax=docker/dockerfile:1
# Multi-stage build for the Inventory service (C-12, tech-stack §13).

# ---- build stage: compile with CMake + vcpkg (manifest mode) ----
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive \
    VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_BINARY_CACHE=/opt/vcpkg-cache
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar pkg-config \
        ca-certificates linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*
# Bootstrap vcpkg (dependency manager per tech-stack §13).
RUN git clone --depth 1 https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && "$VCPKG_ROOT"/bootstrap-vcpkg.sh -disableMetrics
RUN mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE"

WORKDIR /src
# The proto module is shared across services and lives at the repo root, so the
# build context must be the repository root (see the compose/helm build config).
COPY proto ./proto
COPY services/inventory ./services/inventory

WORKDIR /src/services/inventory
RUN --mount=type=cache,target=/opt/vcpkg-cache \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DINVENTORY_BUILD_TESTS=OFF \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    && cmake --build build --target inventory_server

# ---- runtime stage: slim image with only the runtime shared libraries ----
FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libpq5 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/services/inventory/build/inventory_server /usr/local/bin/inventory_server
# Migrations shipped for the Release Engineer's migrate job.
COPY services/inventory/migrations /app/migrations
RUN useradd --system --uid 10001 inventory
USER inventory
EXPOSE 50052
ENTRYPOINT ["/usr/local/bin/inventory_server"]
