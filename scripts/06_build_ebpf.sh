#!/usr/bin/env bash
# scripts/06_build_ebpf.sh - build optional eBPF tracing agent

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

VERIFY_DIR="${BUILD_DIR}/verification"

not_verified() {
	ensure_dir "${VERIFY_DIR}"
	printf 'not verified: %s\n' "$*" >"${VERIFY_DIR}/ebpf.not_verified"
	warn "eBPF not verified: $*"
}

main() {
	section "eBPF build"
	if ! command -v clang >/dev/null 2>&1; then not_verified "clang missing"; exit 0; fi
	if ! command -v llvm-strip >/dev/null 2>&1; then not_verified "llvm-strip missing"; exit 0; fi
	if ! command -v bpftool >/dev/null 2>&1; then not_verified "bpftool missing"; exit 0; fi
	if [[ ! -r /sys/kernel/btf/vmlinux ]]; then not_verified "running kernel BTF is unavailable"; exit 0; fi
	if [[ ! -f /usr/include/bpf/libbpf.h && ! -f /usr/include/bpf/bpf.h ]]; then not_verified "libbpf headers missing"; exit 0; fi

	make -C "${ROOT}/ebpf" ARCH="${ARCH:-arm64}"
	log "eBPF build complete. Runtime attach still requires the target safety_copro tracepoints."
	section "Done"
}

main "$@"
