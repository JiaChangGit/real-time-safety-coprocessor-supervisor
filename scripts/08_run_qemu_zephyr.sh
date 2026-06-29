#!/usr/bin/env bash
# scripts/08_run_qemu_zephyr.sh - run Zephyr QEMU guest and record UART PTY

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

RUN_DIR="${BUILD_DIR}/run"
PTY_FILE="${RUN_DIR}/zephyr_uart_pty.txt"
ELF="${BUILD_DIR}/zephyr/zephyr/zephyr.elf"

main() {
	section "Run Zephyr QEMU"
	require_cmd qemu-system-aarch64 qemu-system-arm
	require_file "${ELF}" "run scripts/04_build_zephyr.sh formal first."
	ensure_dir "${RUN_DIR}"
	rm -f "${PTY_FILE}"

	warn "This direct QEMU command is a minimal fallback; west runner parity is not verified."
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a53 \
		-smp 2 \
		-m 256M \
		-nographic \
		-kernel "${ELF}" \
		-serial pty \
		-nic none \
		2> >(while IFS= read -r line; do
			printf '%s\n' "${line}" >&2
			case "${line}" in
				*"/dev/pts/"*) printf '%s\n' "$(printf '%s\n' "${line}" | grep -o '/dev/pts/[0-9]*' | head -n 1)" >"${PTY_FILE}" ;;
			esac
		done)
}

main "$@"
