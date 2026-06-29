#!/usr/bin/env bash
# scripts/05_build_userspace.sh - build C++17 userspace (host or ARM64 cross)
#
# 用法：
#   scripts/05_build_userspace.sh [host|arm64]   （預設 host）
#
#   host  : 為本機建置，產出 build/userspace/bin/{safety-linkd,safety-supervisord,safetyctl,pty_bridge}
#   arm64 : 交叉建置給 QEMU guest，產出 build/userspace-arm64/bin/{safety-linkd,safety-supervisord,safetyctl}
#           （pty_bridge 為 host-only：它在兩個 QEMU 之間於 host 上轉送，故 arm64 不需要它，
#            但 CMake 仍會一併編出；我們只驗證 guest 需要的兩個 binary。）
#
# ARM64 連結策略：必須使用 -static，initramfs 不拷貝動態 linker 或共享庫。
#
# 冪等：CMake configure 會偵測既有 cache；重複執行只做增量編譯。

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

TARGET="${1:-host}"

SRC="${ROOT}/userspace"

check_protocol_headers() {
	section "Checking protocol header copies"
	cmp "${ROOT}/include/safety_protocol.h" \
		"${ROOT}/userspace/common/safety_protocol.h" \
		|| die "userspace protocol header copy differs from include/safety_protocol.h"
	cmp "${ROOT}/include/safety_protocol.h" \
		"${ROOT}/linux/drivers/safety_copro/safety_protocol.h" \
		|| die "Linux driver protocol header copy differs from include/safety_protocol.h"
	cmp "${ROOT}/include/safety_protocol.h" \
		"${ROOT}/zephyr/safety_copro_firmware/include/safety_protocol.h" \
		|| die "Zephyr protocol header copy differs from include/safety_protocol.h"
	log "Protocol headers are synchronized."
}

build_host() {
	section "Building userspace (host)"
	require_cmd cmake cmake
	require_cmd g++ build-essential
	local bdir="${BUILD_DIR}/userspace"
	ensure_dir "${bdir}"

	cmake -S "${SRC}" -B "${bdir}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		|| die "CMake configure (host) failed."
	cmake --build "${bdir}" -j"${JOBS}" \
		|| die "CMake build (host) failed."

	# ---- 驗證預期 binary 全部存在 ----
	local bin="${bdir}/bin"
	local expect=(safety-linkd safety-supervisord safetyctl pty_bridge)
	local f
	for f in "${expect[@]}"; do
		require_file "${bin}/${f}" "host build did not produce ${bin}/${f}."
	done
	log "Host userspace built. Binaries:"
	for f in "${expect[@]}"; do
		printf '       %s\n' "${bin}/${f}"
	done
}

build_arm64() {
	section "Building userspace (ARM64 cross)"
	require_cmd cmake cmake
	require_cmd "${CROSS_COMPILE}g++" g++-aarch64-linux-gnu
	require_cmd "${CROSS_COMPILE}gcc" gcc-aarch64-linux-gnu
	require_cmd "${CROSS_COMPILE}readelf" binutils-aarch64-linux-gnu
	local bdir="${BUILD_DIR}/userspace-arm64"
	ensure_dir "${bdir}"

	cmake -S "${SRC}" -B "${bdir}" \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" \
		-DCMAKE_CXX_COMPILER="${CROSS_COMPILE}g++" \
		-DCMAKE_EXE_LINKER_FLAGS=-static \
		|| die "CMake configure (arm64 static) failed."
	cmake --build "${bdir}" -j"${JOBS}" \
		|| die "CMake build (arm64 static) failed."

	# ---- guest 只需這三個 binary（bridge 為 host-only）----
	local bin="${bdir}/bin"
	local expect=(safety-linkd safety-supervisord safetyctl)
	local f
	for f in "${expect[@]}"; do
		require_file "${bin}/${f}" "arm64 build did not produce ${bin}/${f}."
		if "${CROSS_COMPILE}readelf" -l "${bin}/${f}" | grep -q 'INTERP'; then
			die "${bin}/${f} is dynamically linked; initramfs requires static ARM64 binaries."
		fi
	done

	log "ARM64 userspace built (static). Binaries:"
	for f in "${expect[@]}"; do
		printf '       %s\n' "${bin}/${f}"
	done
	# 額外提示 ELF 類型，方便人工確認 arch/連結方式。
	if command -v file >/dev/null 2>&1; then
		file "${bin}/safety-supervisord" || true
	fi
}

case "${TARGET}" in
	host) check_protocol_headers; build_host ;;
	arm64) check_protocol_headers; build_arm64 ;;
	*) die "unknown target '${TARGET}'. Usage: $0 [host|arm64]" ;;
esac

section "Done"
log "Userspace build (${TARGET}) complete."
