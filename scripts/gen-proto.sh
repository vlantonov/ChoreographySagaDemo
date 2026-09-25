#!/usr/bin/env bash
#
# Reproducible gRPC/protobuf code generation for all three services.
#
# Preferred path: `buf generate` (uses buf.yaml + buf.gen.yaml, remote plugins,
# and the googleapis dependency for grpc-gateway annotations).
#
# Fallback path (when buf is unavailable, as in some CI/dev images): local
# `protoc` + language plugins. The google/api annotation protos required by
# grpc-gateway are fetched on demand into a temporary include directory so we do
# not vendor Google-owned sources into this repository.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

if command -v buf >/dev/null 2>&1; then
  echo ">> Generating with buf"
  buf generate
  exit 0
fi

echo ">> buf not found; falling back to protoc"
command -v protoc >/dev/null 2>&1 || { echo "protoc is required for the fallback"; exit 1; }

# --- fetch googleapis annotation protos (Apache-2.0) into an include dir ---
GAPI_DIR="$(mktemp -d)"
trap 'rm -rf "${GAPI_DIR}"' EXIT
mkdir -p "${GAPI_DIR}/google/api"
GAPI_BASE="https://raw.githubusercontent.com/googleapis/googleapis/master/google/api"
for f in annotations.proto http.proto; do
  if [[ ! -f "${GAPI_DIR}/google/api/${f}" ]]; then
    echo ">> fetching google/api/${f}"
    curl -fsSL "${GAPI_BASE}/${f}" -o "${GAPI_DIR}/google/api/${f}"
  fi
done

INCLUDES=(-I proto -I "${GAPI_DIR}")

# --- Go (Order) ---
GO_OUT="services/order/internal/pb"
mkdir -p "${GO_OUT}"
echo ">> protoc: Go order stubs"
protoc "${INCLUDES[@]}" \
  --go_out="${GO_OUT}" --go_opt=paths=source_relative \
  --go-grpc_out="${GO_OUT}" --go-grpc_opt=paths=source_relative \
  --grpc-gateway_out="${GO_OUT}" --grpc-gateway_opt=paths=source_relative \
  proto/order/v1/order.proto

# --- Python (Payment) needs the Inventory gRPC client ---
PY_OUT="services/payment/src/payment/pb"
mkdir -p "${PY_OUT}"
echo ">> protoc: Python inventory client stubs"
python3 -m grpc_tools.protoc "${INCLUDES[@]}" \
  --python_out="${PY_OUT}" \
  --grpc_python_out="${PY_OUT}" \
  proto/inventory/v1/inventory.proto
touch "${PY_OUT}/__init__.py"
# grpc_tools emits absolute-style imports; rewrite to package-relative.
find "${PY_OUT}" -name '*_pb2*.py' -exec \
  sed -i -E 's/^from inventory\.v1/from payment.pb.inventory.v1/' {} + || true

# --- C++ (Inventory) server stubs ---
CPP_OUT="services/inventory/generated"
mkdir -p "${CPP_OUT}"
GRPC_CPP_PLUGIN="$(command -v grpc_cpp_plugin || true)"
echo ">> protoc: C++ inventory stubs"
protoc "${INCLUDES[@]}" --cpp_out="${CPP_OUT}" proto/inventory/v1/inventory.proto
if [[ -n "${GRPC_CPP_PLUGIN}" ]]; then
  protoc "${INCLUDES[@]}" \
    --grpc_out="${CPP_OUT}" --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN}" \
    proto/inventory/v1/inventory.proto
else
  echo "!! grpc_cpp_plugin not found; C++ gRPC stubs skipped (message stubs only)."
fi

echo ">> Done."
