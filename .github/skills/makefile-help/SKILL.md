---
name: makefile-help
description: 'Add a self-documenting "help" target to a Makefile. Use when adding help, adding make help, documenting Makefile targets, or generating target descriptions from inline comments.'
argument-hint: 'Optional: path to the Makefile (default: ./Makefile)'
user-invocable: true
disable-model-invocation: false
---

# Makefile Help Target

Add a self-documenting `help` target that lists all Makefile targets with descriptions parsed from inline `##` comments.

## When to Use
- Adding `make help` to a project for the first time
- Documenting existing Makefile targets
- Updating a Makefile so developers can discover available commands
- Any request to add a help target, help message, or usage output to a Makefile

## How It Works

Each target that should appear in the help output gets a trailing `## Short description` comment on the same line as the rule. The `help` target greps for that pattern and pretty-prints it.

```makefile
build: ## Compile the binary
test:  ## Run all tests with race detector
```

Running `make help` (or just `make`) then prints:

```
Usage:
  make <target>

Targets:
  build                Compile the binary
  test                 Run all tests with race detector
  ...
```

## Procedure

### Step 1 — Read the Makefile
1. Open the target Makefile (default `./Makefile`, or the path passed as the argument).
2. List every `.PHONY` target and every rule target already present.

### Step 2 — Add `##` Comments to Existing Targets
For each target that lacks a `## description` comment:
1. Propose a concise, imperative-mood description (≤ 60 characters).
2. Append it on the same line as the rule, separated by a space: `target: ## Description`.
3. Preserve all existing recipe lines unchanged.

### Step 3 — Insert the `help` Target
Add `help` to the `.PHONY` declaration (create one if absent), then insert the snippet from [assets/help-target.mk](./assets/help-target.mk) **before** all other targets so that `make` (with no arguments) prints help by default.

Set `help` as the **first real target** in the file so it becomes the default goal.

### Step 4 — Validate
1. Run `make help` (or `make -f <path> help`) and confirm every documented target appears.
2. Verify no existing recipe was altered.
3. Check that the `.PHONY` line includes `help`.

## Reference Template

See [assets/help-target.mk](./assets/help-target.mk) for the exact snippet to insert.
