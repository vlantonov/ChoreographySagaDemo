# Project Status — Choreography Saga Demo

Project Manager running the SDLC agent chain for a new portfolio project.

## Pipeline

New-project chain: **requirements-analyst → system-architect → developer → qa-engineer → release-engineer → technical-writer**

| Stage | Owner | Status | Notes |
|-------|-------|--------|-------|
| Requirements | Requirements Analyst | ✅ Done | `docs/requirements/SRS.md` (FR-1…26, NFR-1…7, C-1…16, AC-1…13, OQ-1…9) |
| Design / Tech Stack | System Architect | ⬜ Not started | Must resolve OQ-1…9 and create `docs/tech-stack.md` |
| Implementation | Developer | ⬜ Not started | |
| QA / Verification | QA Engineer | ⬜ Not started | |
| Release / Packaging | Release Engineer | ⬜ Not started | |
| Documentation | Technical Writer | ⬜ Not started | |

## Open questions carried forward
- OQ-1…9 from the SRS — most critical: language-to-service mapping (C++/Python/Go), SQL engine choice, concrete SLO/alert thresholds, Kafka event schema/versioning, forced-failure trigger mechanism, order-trigger interface. Owned by System Architect.

## Commit log (per-stage, semver-classified)
- Requirements stage: pending commit.
