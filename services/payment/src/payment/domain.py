"""Business rules for the Payment service, including the forced-failure trigger.

Pure functions only (no I/O) so they are unit-testable in isolation
(tech-stack §10, FR-19, FR-20, AC-4).
"""

from __future__ import annotations

import os
from dataclasses import dataclass

# Magic amount that deterministically forces a payment failure (tech-stack §10).
MAGIC_FAILURE_AMOUNT = 66.06
_AMOUNT_EPSILON = 0.005


@dataclass(frozen=True)
class PaymentDecision:
    """Outcome of evaluating whether a charge should succeed."""

    approved: bool
    reason: str


def decide_payment(amount: float, force_failure: bool) -> PaymentDecision:
    """Decide a payment outcome.

    A charge fails when the global ``force_failure`` flag is set or when the
    order amount equals the magic value ``66.06`` (within a cent). Otherwise the
    charge is approved. This is the only place failure is injected.
    """
    if force_failure:
        return PaymentDecision(approved=False, reason="forced failure flag enabled")
    if abs(amount - MAGIC_FAILURE_AMOUNT) < _AMOUNT_EPSILON:
        return PaymentDecision(approved=False, reason="magic failure amount 66.06")
    return PaymentDecision(approved=True, reason="approved")


def force_failure_from_env(environ: dict[str, str] | None = None) -> bool:
    """Read the FORCE_PAYMENT_FAILURE flag (tech-stack §10)."""
    env = environ if environ is not None else dict(os.environ)
    return env.get("FORCE_PAYMENT_FAILURE", "").strip().lower() in {"1", "true", "yes"}
