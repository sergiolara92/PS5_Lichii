# SPDX-License-Identifier: GPL-3.0-or-later
# ProsperoEden build. `make` builds the release files in dist/; `make help` lists every target.
# Missing dependencies are fetched at their pinned revisions (tools/deps.json); nothing that
# already exists is changed. The build runs on Linux (Ubuntu 26.04; WSL works).

SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.DEFAULT_GOAL := release
MAKEFLAGS += --no-print-directory

# Parallel compile jobs.
JOBS ?= $(shell nproc)
export EDEN_BUILD_JOBS := $(JOBS)
# make dev: the title ID (16 hex digits) the development build boots.
DEV_TITLE ?= $(shell cat .local/dev-profile-title 2>/dev/null)
# make install: the console's address, and the port of its FTP server.
PS5_HOST ?=
PS5_PORT ?= 2121

# The sibling checkouts (tools/deps.json: boilerplate, PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK, MihawkGen): beside this
# repository, or in the folder PROSPERO_SIBLINGS (or .local/siblings) names.
export PROSPERO_SIBLINGS ?= $(abspath $(or $(shell cat .local/siblings 2>/dev/null),$(CURDIR)/..))

# CachyOS/Arch: tools/setup-cachyos.sh writes the private LLVM 18 names here; only this build sees them.
TOOLCHAIN_BIN := $(wildcard $(CURDIR)/.local/toolchain/bin)
ifneq ($(TOOLCHAIN_BIN),)
export PROSPERO_TOOLCHAIN_BIN := $(TOOLCHAIN_BIN)
export PATH := $(TOOLCHAIN_BIN):$(PATH)
# The Payload SDK keeps the system LLVM other PS5 projects on this PC use, whatever llvm-config-NN exists.
export LLVM_CONFIG ?= /usr/bin/llvm-config
endif

PYTHON := python3 -B
RELEASE_APP := build/release/PPSA99008
# make install: which build goes to the console (APP=build/dev/PPSA99008 for make dev / ns2-dev).
APP ?= $(RELEASE_APP)

.PHONY: help release package ns2 ns2-dev ns2-host dev prepare deps deps-status toolchain test install clean distclean

help: ## List the targets and variables
	@echo 'ProsperoEden build - make [target] [VARIABLE=value]'
	@echo
	@awk 'BEGIN { FS = ":.*## " } /^[a-z][a-z0-9-]*:.*## / { printf "  %-12s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo
	@echo 'Variables:'
	@echo '  JOBS=$(JOBS)  parallel compile jobs'
	@echo '  DEV_TITLE=$(DEV_TITLE)  title ID the development build boots (make dev)'
	@echo '  PS5_HOST=$(PS5_HOST) PS5_PORT=$(PS5_PORT)  console for make install'

release: package ## Release files in dist/: ZIP, SHA256SUMS, release notes (default)
	$(PYTHON) tools/ci/make-dist.py $(RELEASE_APP)

package: prepare ## Build the app as released: build/release/PPSA99008
	bash tools/build-package.sh release

# The only source: Frizti/Fritchii v5.3 (1a7a3c0, an Eden-derived Switch 2 emulator, tools/deps.json) plus
# patches/fritchii (the PS5 layer's needs; <patch>.d holds ours on top). Games start through the direct
# loader (.eden descriptors). EDEN_VK_PORT_LENIENT=1 until the skipped Vulkan edits are re-targeted.
export EDEN_VK_PORT_LENIENT ?= 1
export PROSPERO_DIRECT_FRONTEND_ROUTE ?= ON
export PROSPERO_DIRECT_ALLOW_EXECUTE ?= ON

ns2: package ## Same as make package (older name)

ns2-dev: ## make dev for DEV_TITLE (Mario Kart World by default); FAST=1: no ThinLTO/PGO, quicker relinks, not for fps
	$(MAKE) dev DEV_TITLE=$(or $(DEV_TITLE),0400C3F00006E000)

ns2-host: prepare ## Linux (PC) build: build/ns2-host/eden-headless
	bash tools/build-ns2-host.sh

dev: prepare ## Development build that boots DEV_TITLE: build/dev/PPSA99008
	@[[ -n "$(DEV_TITLE)" ]] || { echo 'Set DEV_TITLE=<16-digit title ID> (or write it to .local/dev-profile-title)'; exit 2; }
	EDEN_FAST_BUILD=$(if $(FAST),1,0) bash tools/build-package.sh dev $(DEV_TITLE)

prepare: deps ## Build cache, Eden source, FFmpeg, packaging tool, driver stub, RADV (what is missing)
	bash tools/prepare-build.sh

deps: ## Fetch missing dependencies at their pinned revisions
	$(PYTHON) tools/deps.py fetch

deps-status: ## Show every dependency, where it lives and whether it matches its pin
	@$(PYTHON) tools/deps.py status

toolchain: ## Check the host tools the build needs
	@bash tools/check-toolchain.sh

test: prepare ## Host (Linux) build of the emulator and its test suites (ROMM_CHECK=1: also against RomM servers in Docker)
	bash tools/check-elevation-client.sh
	bash tools/build-headless-host.sh

install: ## Copy build/release/PPSA99008 (or APP=...) to a console over FTP (PS5_HOST=<address>)
	@[[ -n "$(PS5_HOST)" ]] || { echo 'Set PS5_HOST=<console address>'; exit 2; }
	$(PYTHON) tools/install-ftp.py $(PS5_HOST) $(APP) --port $(PS5_PORT)

clean: ## Remove build outputs (build/, dist/); dependencies and the build cache stay
	rm -rf build dist

distclean: clean ## Also remove the fetched dependencies in .deps and this checkout's build cache
	$(PYTHON) tools/deps.py clean
	@if [[ -f .local/headless-cache ]]; then \
		cache=$$(cat .local/headless-cache); \
		if [[ $$cache == */ps5-eden-headless.* && -f $$cache/owner && $$(cat $$cache/owner) == $(CURDIR) ]]; then \
			echo "rm -rf $$cache"; rm -rf "$$cache"; \
		fi; \
		rm -f .local/headless-cache; \
	fi
