# Tech Stack Reference

This file is the single source of truth for language- and toolchain-specific
details that the SDLC agents (`project-planner`, `requirements-analyst`,
`system-architect`, `developer`, `qa-engineer`, `release-engineer`,
`maintenance-engineer`, `technical-writer`) need but should not hardcode.
Every agent reads this file (or asks for it to be filled in) before doing
language-specific work. Keep one of these per repo, at `docs/tech-stack.md`.

Fill in each section for the current project. Leave a field as `TBD` and
flag it as an open question rather than guessing.

## Language & Standard
- Primary language(s): <e.g. C++20, Go 1.22, Python 3.12, TypeScript 5>
- Secondary/glue languages, if any:

## Build System
- Tool: <CMake, go build, poetry/hatch, npm/pnpm, cargo, Makefile, ...>
- Entry point(s)/commands to configure and build:
- Build output layout:

## Dependency / Package Management
- Tool: <Conan, go modules, pip/poetry, npm/yarn/pnpm, cargo, NuGet, ...>
- Manifest file(s): <conanfile.py, go.mod, pyproject.toml, package.json, Cargo.toml, ...>
- Internal convention for pinning versions / lockfiles:
- How to add a new dependency (command + where it's declared):

## Test Framework
- Unit test framework: <GoogleTest/Catch2, testing+testify, pytest, Jest/Vitest, cargo test, ...>
- Command to run the full suite:
- Command to run a single test:
- Coverage tool, if used:

## Static Analysis / Linting / Formatting
- Linter(s): <clang-tidy/cppcheck, golangci-lint, ruff/pylint, eslint, clippy, ...>
- Formatter(s): <clang-format, gofmt, black/ruff format, prettier, rustfmt, ...>
- Memory/concurrency checkers, if applicable: <ASan/UBSan/TSan, go race detector, valgrind, ...>
  (Only relevant for languages without built-in memory/thread safety guarantees.)

## Style & Idioms
- Ownership/resource-management convention: <RAII + smart pointers, GC, ref-counting, borrow checker, ...>
- Error-handling convention: <exceptions, error codes/Result types, panics+recover, ...>
- Naming/style guide reference:
- Module/package layout convention:

## CI/CD
- Platform: <GitHub Actions, GitLab CI, ...>
- Caching strategy for dependencies:
- Packaging/release mechanism: <CPack, goreleaser, twine/PyPI, npm publish, cargo publish, container image, ...>
- Versioning scheme: <SemVer, CalVer, ...>

## Documentation Conventions
- API doc generator: <Doxygen, godoc, Sphinx, TypeDoc, rustdoc, ...>
- README/CHANGELOG format already in use:

---

**Usage note for agents:** if this file doesn't exist yet or has unfilled
`TBD` fields that block your stage, flag it as an open question back to
`requirements-analyst` (for a new project) or ask the user directly, rather
than assuming a specific language's toolchain.
