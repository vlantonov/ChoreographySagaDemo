"""Forced-failure business-rule tests (tech-stack §10, FR-19, FR-20, AC-4)."""

from __future__ import annotations

from payment.domain import MAGIC_FAILURE_AMOUNT, decide_payment, force_failure_from_env


def test_magic_amount_forces_failure() -> None:
    decision = decide_payment(MAGIC_FAILURE_AMOUNT, force_failure=False)
    assert not decision.approved
    assert "66.06" in decision.reason


def test_normal_amount_is_approved() -> None:
    assert decide_payment(10.0, force_failure=False).approved


def test_force_failure_flag_overrides_amount() -> None:
    decision = decide_payment(10.0, force_failure=True)
    assert not decision.approved
    assert "flag" in decision.reason


def test_force_failure_from_env_truthy() -> None:
    assert force_failure_from_env({"FORCE_PAYMENT_FAILURE": "true"})
    assert force_failure_from_env({"FORCE_PAYMENT_FAILURE": "1"})
    assert not force_failure_from_env({"FORCE_PAYMENT_FAILURE": "no"})
    assert not force_failure_from_env({})
