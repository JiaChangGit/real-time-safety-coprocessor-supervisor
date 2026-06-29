#!/usr/bin/env bash
# scripts/04_build_zephyr.sh - build Zephyr v4.4.0 safety co-processor firmware
#
# 用法：
#   scripts/04_build_zephyr.sh [formal|debug]   （預設 formal）
#
#   formal : 用 prj.conf（關 shell，純二進位協定）→ build/zephyr
#   debug  : 在 prj.conf 上疊 prj_debug.conf（開 shell，可人工注入）→ build/zephyr-debug
#
# Board: qemu_cortex_a53/qemu_cortex_a53/smp
#
# 前置：先跑 scripts/01_fetch_sources.sh 準備 west/Zephyr workspace，並安裝已驗證
# 的 minimal Zephyr SDK 到 build/tools/zephyr-sdk。
#
# 冪等：west build 對既有 build dir 做增量；變體建在不同目錄互不干擾。

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

VARIANT="${1:-formal}"
APP="${ROOT}/zephyr/safety_copro_firmware"
BOARD="qemu_cortex_a53/qemu_cortex_a53/smp"
LOCAL_WEST="${BUILD_DIR}/deps/zephyr-venv/bin/west"
LOCAL_ZEPHYR_BASE="${BUILD_DIR}/deps/zephyr-workspace/zephyr"
LOCAL_SDK="${BUILD_DIR}/tools/zephyr-sdk"
VERIFY_DIR="${BUILD_DIR}/verification"

not_verified() {
	ensure_dir "${VERIFY_DIR}"
	printf 'not verified: %s\n' "$*" >"${VERIFY_DIR}/zephyr.not_verified"
}

WEST="${WEST:-}"

check_prereqs() {
	section "Checking Zephyr prerequisites"

	if [[ -z "${WEST}" ]]; then
		if command -v west >/dev/null 2>&1; then
			WEST="$(command -v west)"
		elif [[ -x "${LOCAL_WEST}" ]]; then
			WEST="${LOCAL_WEST}"
		else
			not_verified "west is missing; run scripts/01_fetch_sources.sh"
			die "'west' not found. Run scripts/01_fetch_sources.sh first."
		fi
	fi

	if [[ -z "${ZEPHYR_BASE:-}" ]]; then
		if [[ -d "${LOCAL_ZEPHYR_BASE}" ]]; then
			export ZEPHYR_BASE="${LOCAL_ZEPHYR_BASE}"
		else
			not_verified "Zephyr v4.4.0 workspace is missing"
			die "ZEPHYR_BASE is not set and ${LOCAL_ZEPHYR_BASE} is missing. Run scripts/01_fetch_sources.sh first."
		fi
	fi
	if [[ ! -d "${ZEPHYR_BASE}" ]]; then
		not_verified "ZEPHYR_BASE does not exist: ${ZEPHYR_BASE}"
		die "ZEPHYR_BASE='${ZEPHYR_BASE}' does not exist. Point it at a valid Zephyr v4.4.0 checkout."
	fi

	if [[ -z "${ZEPHYR_SDK_INSTALL_DIR:-}" ]]; then
		if [[ -d "${LOCAL_SDK}" ]]; then
			export ZEPHYR_SDK_INSTALL_DIR="${LOCAL_SDK}"
		else
			not_verified "Zephyr SDK is not verified at ${LOCAL_SDK}"
			die "Zephyr SDK not verified at ${LOCAL_SDK}. Run scripts/01_fetch_sources.sh and install a minimal SDK for qemu_cortex_a53 only."
		fi
	fi
	require_file "${ZEPHYR_SDK_INSTALL_DIR}/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-gcc" \
		"minimal Zephyr SDK must include only the aarch64-zephyr-elf GNU target. Run scripts/01_fetch_sources.sh."
	export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
	unset CROSS_COMPILE

	require_file "${APP}/CMakeLists.txt" "Zephyr app expected at zephyr/safety_copro_firmware/."
	log "Zephyr prerequisites OK (west=${WEST}, ZEPHYR_BASE=${ZEPHYR_BASE}, SDK=${ZEPHYR_SDK_INSTALL_DIR}, toolchain=${ZEPHYR_TOOLCHAIN_VARIANT})."
}

run_west_build() {
	local bdir="$1"
	shift
	"${WEST}" -z "${ZEPHYR_BASE}" build -p always -b "${BOARD}" \
		-d "${bdir}" "${APP}" -- \
		-DZEPHYR_BASE="${ZEPHYR_BASE}" \
		-DZephyr_DIR="${ZEPHYR_BASE}/share/zephyr-package/cmake" "$@"
}

verify_config() {
	local cfg="$1"
	local forbidden
	grep -q '^CONFIG_ARM64=y$' "${cfg}" \
		|| die "Zephyr output is not ARM64: CONFIG_ARM64=y missing from ${cfg}."
	grep -q '^CONFIG_SMP=y$' "${cfg}" \
		|| die "Zephyr output is not SMP: CONFIG_SMP=y missing from ${cfg}."
	grep -q '^CONFIG_MP_MAX_NUM_CPUS=2$' "${cfg}" \
		|| die "Zephyr output does not configure two CPUs: ${cfg}."
	for forbidden in SHELL LOG PRINTK UART_CONSOLE CONSOLE BT USB NET NETWORKING WIFI FILE_SYSTEM SENSOR SETTINGS MBEDTLS NEWLIB_LIBC; do
		if grep -q "^CONFIG_${forbidden}=y$" "${cfg}"; then
			die "formal Zephyr build unexpectedly enables CONFIG_${forbidden}."
		fi
	done
}

build_formal() {
	section "Building Zephyr firmware (FORMAL, prj.conf)"
	local bdir="${BUILD_DIR}/zephyr"
	ensure_dir "${BUILD_DIR}"
	# 預設使用 prj.conf；-p always 避免舊 toolchain/workspace cache 污染 formal build。
	run_west_build "${bdir}" || die "west build (formal) failed."
	require_file "${bdir}/zephyr/zephyr.elf" "Zephyr build did not produce zephyr.elf."
	verify_config "${bdir}/zephyr/.config"
	cp -f "${bdir}/zephyr/zephyr.elf" "${BUILD_DIR}/zephyr.elf"
	log "FORMAL firmware -> ${bdir}/zephyr/zephyr.elf (copied to ${BUILD_DIR}/zephyr.elf)"
}

build_debug() {
	section "Building Zephyr firmware (DEBUG, prj.conf + prj_debug.conf overlay)"
	local bdir="${BUILD_DIR}/zephyr-debug"
	ensure_dir "${BUILD_DIR}"
	# OVERLAY_CONFIG：在 prj.conf 之上疊加 prj_debug.conf（開啟 shell）。
	run_west_build "${bdir}" -DOVERLAY_CONFIG=prj_debug.conf \
		|| die "west build (debug) failed."
	require_file "${bdir}/zephyr/zephyr.elf" "Zephyr debug build did not produce zephyr.elf."
	verify_config "${bdir}/zephyr/.config"
	grep -q '^CONFIG_SHELL=y$' "${bdir}/zephyr/.config" \
		|| die "debug Zephyr build did not enable CONFIG_SHELL."
	# debug 變體的 elf 另存，避免覆蓋 formal 的 build/zephyr.elf。
	cp -f "${bdir}/zephyr/zephyr.elf" "${BUILD_DIR}/zephyr-debug.elf"
	log "DEBUG firmware -> ${bdir}/zephyr/zephyr.elf (copied to ${BUILD_DIR}/zephyr-debug.elf)"
}

main() {
	section "Zephyr firmware build (${VARIANT})"
	check_prereqs
	case "${VARIANT}" in
		formal) build_formal ;;
		debug) build_debug ;;
		*) die "unknown variant '${VARIANT}'. Usage: $0 [formal|debug]" ;;
	esac
	section "Done"
	log "Zephyr build (${VARIANT}) complete."
}

main "$@"
