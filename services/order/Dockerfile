# syntax=docker/dockerfile:1
# Multi-stage build for the Order service (C-12).

# ---- build stage ----
FROM golang:1.23-alpine AS build
WORKDIR /src
RUN apk add --no-cache ca-certificates
# Cache modules first for faster rebuilds.
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" \
    -o /out/order ./cmd/order

# ---- runtime stage ----
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /app
COPY --from=build /out/order /app/order
# Migrations are shipped for the Release Engineer's migrate job.
COPY --from=build /src/migrations /app/migrations
USER nonroot:nonroot
EXPOSE 50051 8080
ENTRYPOINT ["/app/order"]
