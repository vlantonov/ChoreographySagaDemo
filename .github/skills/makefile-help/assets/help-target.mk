# Drop this block into your Makefile BEFORE all other targets.
# Each target you want documented must have a trailing "## description" comment.
#
# Example:
#   build: ## Compile the binary
#   test:  ## Run all tests with race detector

.DEFAULT_GOAL := help

.PHONY: help
help: ## Show this help message
	@echo "Usage:"
	@echo "  make <target>"
	@echo ""
	@echo "Targets:"
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| sort \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'
