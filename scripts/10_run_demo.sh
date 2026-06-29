#!/usr/bin/env bash
# scripts/10_run_demo.sh - run supported demos or mark E2E as not verified

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

DEMO="${1:-baseline}"
SUP="${BUILD_DIR}/userspace/bin/safety-supervisord"
FIXTURE="${ROOT}/tests/fixtures/fault_log.bin"
VERIFY_DIR="${BUILD_DIR}/verification"

mark_not_verified() {
	ensure_dir "${VERIFY_DIR}"
	printf 'not verified: %s\n' "$*" >"${VERIFY_DIR}/demo_${DEMO}.not_verified"
	warn "Demo ${DEMO} not verified: $*"
}

run_host_mock_replay() {
	"${ROOT}/scripts/05_build_userspace.sh" host
	ensure_dir "${REPORTS_DIR}"
	require_file "${FIXTURE}" "test fixture is missing."
	"${SUP}" --mock-device "${FIXTURE}" --report-dir "${REPORTS_DIR}"
	"${SUP}" --replay "${REPORTS_DIR}/events.jsonl" --report-dir "${REPORTS_DIR}"
}

main() {
	section "Run demo (${DEMO})"
	case "${DEMO}" in
		baseline)
			run_host_mock_replay
			log "Host baseline mock/replay completed. Full QEMU baseline requires scripts/07, 08, 09 orchestration and is not implied."
			;;
		heartbeat-timeout|checksum-error)
			if [[ ! -s "${BUILD_DIR}/run/linux_uart_pty.txt" || ! -s "${BUILD_DIR}/run/zephyr_uart_pty.txt" ]]; then
				mark_not_verified "QEMU PTY paths are missing; start Linux and Zephyr QEMU first."
				exit 0
			fi
			mark_not_verified "automatic E2E scenario orchestration is not yet proven in this host session"
			;;
		*)
			die "unknown demo '${DEMO}'. Use baseline, heartbeat-timeout, or checksum-error."
			;;
	esac
	section "Done"
}

main "$@"
