"""Enables ``python -m payment`` to run the service."""

from __future__ import annotations

from payment.main import main

if __name__ == "__main__":
    raise SystemExit(main())
