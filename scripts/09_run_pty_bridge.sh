#!/usr/bin/env bash
# scripts/09_run_pty_bridge.sh - connect Zephyr and Linux QEMU PTYs with pty_bridge

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

RUN_DIR="${BUILD_DIR}/run"
ZEPHYR_PTY_FILE="${RUN_DIR}/zephyr_uart_pty.txt"
LINUX_PTY_FILE="${RUN_DIR}/linux_uart_pty.txt"
BRIDGE="${BUILD_DIR}/userspace/bin/pty_bridge"

main() {
	section "Run PTY bridge"
	if [[ ! -x "${BRIDGE}" ]]; then
		"${ROOT}/scripts/05_build_userspace.sh" host
	fi
	require_file "${ZEPHYR_PTY_FILE}" "start Zephyr QEMU first or write its PTY path here."
	require_file "${LINUX_PTY_FILE}" "start Linux QEMU first or write its PTY path here."
	ensure_dir "${REPORTS_DIR}"

	local left right
	left="$(tr -d '\n' <"${ZEPHYR_PTY_FILE}")"
	right="$(tr -d '\n' <"${LINUX_PTY_FILE}")"
	require_file "${left}" "Zephyr PTY path is invalid."
	require_file "${right}" "Linux PTY path is invalid."

	"${BRIDGE}" \
		--left "${left}" \
		--right "${right}" \
		--log "${REPORTS_DIR}/bridge.csv" \
		${BRIDGE_ARGS:-}
}

main "$@"
