#!/usr/bin/env bash
# scripts/99_cleanup.sh - remove generated build/report/runtime artifacts only

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

main() {
	section "Cleanup generated artifacts"
	rm -rf "${BUILD_DIR:?}"/*
	rm -rf "${REPORTS_DIR:?}"/*
	ensure_dir "${BUILD_DIR}"
	ensure_dir "${REPORTS_DIR}"
	touch "${BUILD_DIR}/.gitkeep" "${REPORTS_DIR}/.gitkeep"
	if [[ -f "${ROOT}/ebpf/Makefile" ]]; then
		make -C "${ROOT}/ebpf" clean >/dev/null 2>&1 || true
	fi
	log "Cleanup complete. Source files were not removed."
	section "Done"
}

main "$@"
