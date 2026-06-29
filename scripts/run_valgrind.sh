#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

BINARY="${BUILD_DIR}/userspace/bin/safety-supervisord"
FIXTURE="${REPORTS_DIR}/events.jsonl"
REPORT="${REPORTS_DIR}/valgrind_report.txt"

main() {
	section "Valgrind replay validation"
	require_cmd valgrind valgrind
	ensure_dir "${REPORTS_DIR}"

	if [[ ! -x "${BINARY}" ]]; then
		log "Host safety-supervisord is missing; building it first."
		"${ROOT}/scripts/05_build_userspace.sh" host
	fi
	if [[ ! -s "${FIXTURE}" ]]; then
		die "replay fixture '${FIXTURE}' is missing or empty. Generate a non-empty events.jsonl before running Valgrind."
	fi

	valgrind \
		--tool=memcheck \
		--leak-check=full \
		--show-leak-kinds=all \
		--track-origins=yes \
		--errors-for-leak-kinds=definite,indirect \
		--error-exitcode=99 \
		--log-file="${REPORT}" \
		"${BINARY}" --replay "${FIXTURE}" --report-dir "${REPORTS_DIR}"

	grep -q "ERROR SUMMARY: 0 errors" "${REPORT}" \
		|| die "Valgrind reported memory errors. Inspect ${REPORT}."
	if ! grep -q "All heap blocks were freed" "${REPORT}" \
		&& ! grep -q "definitely lost: 0 bytes in 0 blocks" "${REPORT}"; then
		die "Valgrind reported definitely lost memory. Inspect ${REPORT}."
	fi
	log "Valgrind replay validation passed: ${REPORT}"
}

main "$@"
