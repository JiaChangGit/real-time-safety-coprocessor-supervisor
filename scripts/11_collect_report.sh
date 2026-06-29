#!/usr/bin/env bash
# scripts/11_collect_report.sh - collect verification status into reports/verification.md

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

OUT="${REPORTS_DIR}/verification.md"
VERIFY_DIR="${BUILD_DIR}/verification"

row_file() {
	local item="$1"
	local file="$2"
	local detail="$3"
	if [[ -e "${file}" ]]; then
		printf '| %s | pass | %s |\n' "${item}" "${detail}"
	else
		printf '| %s | not verified | missing %s |\n' "${item}" "${file#${ROOT}/}"
	fi
}

row_artifact() {
	local item="$1"
	local key="$2"
	local file="$3"
	local detail="$4"
	local marker="${VERIFY_DIR}/${key}.not_verified"
	if [[ -s "${marker}" ]]; then
		printf '| %s | not verified | %s |\n' "${item}" "$(tr '\n' ' ' <"${marker}")"
	elif [[ -e "${file}" ]]; then
		printf '| %s | pass | %s |\n' "${item}" "${detail}"
	else
		printf '| %s | not verified | missing %s |\n' "${item}" "${file#${ROOT}/}"
	fi
}

main() {
	section "Collect report"
	ensure_dir "${REPORTS_DIR}"
	{
		printf '# Verification Report\n\n'
		printf '| Item | Status | Detail |\n'
		printf '| -- | -- | -- |\n'
		if [[ -x "${BUILD_DIR}/userspace/bin/safety-supervisord" ]]; then
			printf '| host userspace | pass | binaries present |\n'
		else
			printf '| host userspace | not verified | build/userspace/bin missing |\n'
		fi
		if [[ -x "${BUILD_DIR}/userspace-arm64/bin/safety-supervisord" && \
		      -x "${BUILD_DIR}/userspace-arm64/bin/safety-linkd" && \
		      -x "${BUILD_DIR}/userspace-arm64/bin/safetyctl" ]]; then
			printf '| ARM64 userspace | pass | guest binaries present |\n'
		else
			printf '| ARM64 userspace | not verified | build/userspace-arm64/bin missing |\n'
		fi
		if [[ -s "${REPORTS_DIR}/events.jsonl" ]]; then
			printf '| mock events | pass | reports/events.jsonl present |\n'
		else
			printf '| mock events | not verified | reports/events.jsonl missing |\n'
		fi
		if [[ -s "${REPORTS_DIR}/replay_events.jsonl" ]]; then
			printf '| replay | pass | reports/replay_events.jsonl present |\n'
		else
			printf '| replay | not verified | reports/replay_events.jsonl missing |\n'
		fi
		row_artifact "Linux kernel Image" "linux_kernel" "${BUILD_DIR}/linux/arch/arm64/boot/Image" "build/linux/arch/arm64/boot/Image present"
		row_artifact "initramfs" "initramfs" "${BUILD_DIR}/initramfs/rootfs.cpio.gz" "build/initramfs/rootfs.cpio.gz present"
		row_artifact "Zephyr firmware" "zephyr" "${BUILD_DIR}/zephyr/zephyr/zephyr.elf" "build/zephyr/zephyr/zephyr.elf present"
		row_artifact "Linux QEMU PTY" "linux_qemu" "${BUILD_DIR}/run/linux_uart_pty.txt" "Linux link UART PTY captured"
		row_artifact "Zephyr QEMU PTY" "zephyr_qemu" "${BUILD_DIR}/run/zephyr_uart_pty.txt" "Zephyr UART PTY captured"
		if [[ -d "${VERIFY_DIR}" ]]; then
			for f in "${VERIFY_DIR}"/*.not_verified; do
				[[ -e "${f}" ]] || continue
				case "$(basename "${f}" .not_verified)" in
					initramfs|zephyr|linux_kernel|linux_qemu|zephyr_qemu) continue ;;
				esac
				printf '| %s | not verified | %s |\n' "$(basename "${f}" .not_verified)" "$(tr '\n' ' ' <"${f}")"
			done
		fi
	} >"${OUT}"
	log "Verification report written: ${OUT}"
	section "Done"
}

main "$@"
