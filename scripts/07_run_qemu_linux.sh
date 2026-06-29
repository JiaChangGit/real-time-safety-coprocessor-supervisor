#!/usr/bin/env bash
# scripts/07_run_qemu_linux.sh - run ARM64 Linux QEMU guest and record link UART PTY

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

RUN_DIR="${BUILD_DIR}/run"
PTY_FILE="${RUN_DIR}/linux_uart_pty.txt"
KERNEL="${BUILD_DIR}/linux/arch/arm64/boot/Image"
INITRD="${BUILD_DIR}/initramfs/rootfs.cpio.gz"

main() {
	section "Run Linux QEMU"
	require_cmd qemu-system-aarch64 qemu-system-arm
	require_file "${KERNEL}" "run scripts/02_build_linux_kernel.sh first."
	require_file "${INITRD}" "run scripts/03_build_initramfs.sh first."
	ensure_dir "${RUN_DIR}"
	rm -f "${PTY_FILE}"

	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a53 \
		-smp 4 \
		-m 1024M \
		-nographic \
		-kernel "${KERNEL}" \
		-initrd "${INITRD}" \
		-append "console=ttyAMA0 rdinit=/init ${LINUX_APPEND:-}" \
		-serial mon:stdio \
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
