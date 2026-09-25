"""Synchronous gRPC client for the Inventory ReserveStock RPC — the single
synchronous leg of the saga (tech-stack §6, ARCHITECTURE §7, C-10, FR-15)."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

import grpc

from payment.pb.inventory.v1 import inventory_pb2, inventory_pb2_grpc


class ReserveStatus(Enum):
    UNSPECIFIED = 0
    RESERVED = 1
    INSUFFICIENT_STOCK = 2


@dataclass
class ReserveResult:
    status: ReserveStatus
    reservation_id: str
    message: str


class InventoryClient:
    def __init__(self, target: str) -> None:
        self._channel = grpc.insecure_channel(target)
        self._stub = inventory_pb2_grpc.InventoryServiceStub(self._channel)

    def reserve_stock(
        self, saga_id: str, idempotency_key: str, sku: str, quantity: int, traceparent: str
    ) -> ReserveResult:
        """Call ReserveStock. Raises grpc.RpcError on transport failure."""
        request = inventory_pb2.ReserveStockRequest(
            saga_id=saga_id,
            idempotency_key=idempotency_key,
            sku=sku,
            quantity=quantity,
        )
        metadata = [("traceparent", traceparent)] if traceparent else None
        response = self._stub.ReserveStock(request, metadata=metadata, timeout=5.0)
        return ReserveResult(
            status=ReserveStatus(response.status),
            reservation_id=response.reservation_id,
            message=response.message,
        )

    def close(self) -> None:
        self._channel.close()
