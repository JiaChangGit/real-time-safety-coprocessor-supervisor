#!/usr/bin/env bash
# scripts/03_build_initramfs.sh - build minimal ARM64 BusyBox initramfs

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

ROOTFS="${BUILD_DIR}/initramfs/rootfs"
OUT="${BUILD_DIR}/initramfs/rootfs.cpio.gz"
ARM64_BIN="${BUILD_DIR}/userspace-arm64/bin"
SYMBOLS_DIR="${BUILD_DIR}/symbols"
VERIFY_DIR="${BUILD_DIR}/verification"

not_verified() {
	ensure_dir "${VERIFY_DIR}"
	printf 'not verified: %s\n' "$*" >"${VERIFY_DIR}/initramfs.not_verified"
}

check_prereqs() {
	section "Checking initramfs prerequisites"
	require_cmd cpio cpio
	require_cmd gzip gzip
	require_cmd file file
	require_cmd "${CROSS_COMPILE}g++" g++-aarch64-linux-gnu
	require_cmd "${CROSS_COMPILE}strip" binutils-aarch64-linux-gnu
	require_cmd "${CROSS_COMPILE}objcopy" binutils-aarch64-linux-gnu
	require_cmd "${CROSS_COMPILE}readelf" binutils-aarch64-linux-gnu
}

ensure_arm64_userspace() {
	section "Ensuring ARM64 userspace"
	if [[ ! -x "${ARM64_BIN}/safety-linkd" || \
	      ! -x "${ARM64_BIN}/safety-supervisord" || \
	      ! -x "${ARM64_BIN}/safetyctl" ]]; then
		"${ROOT}/scripts/05_build_userspace.sh" arm64
	fi
	require_file "${ARM64_BIN}/safety-linkd" "run scripts/05_build_userspace.sh arm64 first."
	require_file "${ARM64_BIN}/safety-supervisord" "run scripts/05_build_userspace.sh arm64 first."
	require_file "${ARM64_BIN}/safetyctl" "run scripts/05_build_userspace.sh arm64 first."
}

find_busybox() {
	local bb="${BUSYBOX:-}"
	if [[ -z "${bb}" ]]; then
		for candidate in /usr/bin/busybox /bin/busybox /usr/lib/busybox/busybox; do
			if [[ -x "${candidate}" ]]; then
				bb="${candidate}"
				break
			fi
		done
	fi
	if [[ -z "${bb}" || ! -x "${bb}" ]]; then
		not_verified "AArch64 busybox is missing"
		die "ARM64 busybox not found. Set BUSYBOX=/path/to/aarch64/busybox."
	fi
	if ! file -L "${bb}" | grep -qi 'aarch64\|ARM aarch64'; then
		not_verified "busybox '${bb}' is not AArch64"
		die "busybox '${bb}' is not AArch64. Set BUSYBOX=/path/to/aarch64/busybox."
	fi
	printf '%s' "${bb}"
}

install_binary() {
	local src="$1"
	local dst="$2"
	local name
	name="$(basename "${dst}")"
	cp -f "${src}" "${dst}"
	chmod +x "${dst}"
	if "${CROSS_COMPILE}readelf" -l "${dst}" | grep -q 'INTERP'; then
		die "${dst} is dynamically linked; initramfs only accepts static ARM64 binaries."
	fi
	if "${CROSS_COMPILE}objcopy" --only-keep-debug "${dst}" "${SYMBOLS_DIR}/${name}.debug" 2>/dev/null; then
		"${CROSS_COMPILE}strip" --strip-all "${dst}" 2>/dev/null || true
		"${CROSS_COMPILE}objcopy" --add-gnu-debuglink="${SYMBOLS_DIR}/${name}.debug" "${dst}" 2>/dev/null || true
	else
		warn "Could not extract debug symbols for ${name}; continuing."
		"${CROSS_COMPILE}strip" --strip-all "${dst}" 2>/dev/null || true
	fi
}

write_init() {
	cat >"${ROOTFS}/init" <<'INIT_EOF'
#!/bin/sh
# Minimal init for the safety co-processor Linux guest. Output is English.

/bin/busybox --install -s 2>/dev/null
mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null
mkdir -p /tmp /reports

DEMO=""
DURATION="8000"
for tok in $(cat /proc/cmdline); do
    case "$tok" in
        safety.demo=*) DEMO="${tok#safety.demo=}" ;;
        safety.duration_ms=*) DURATION="${tok#safety.duration_ms=}" ;;
    esac
done

case "$DEMO" in
    ""|shell)
        echo "[init] shell mode"
        exec /bin/sh
        ;;
    baseline|heartbeat-timeout|checksum-error)
        echo "[init] starting safety-linkd and safety-supervisord (demo=${DEMO})"
        if [ -e /dev/ttyAMA1 ] && [ -e /dev/safety_copro ]; then
            /usr/bin/safety-linkd --uart /dev/ttyAMA1 --device /dev/safety_copro --report-dir /reports &
        else
            echo "[init] required UART or safety device is missing"
        fi
        /usr/bin/safety-supervisord --device /dev/safety_copro --report-dir /reports --duration-ms "$DURATION"
        echo "[init] demo complete"
        exec /bin/sh
        ;;
    *)
        echo "[init] unknown safety.demo=${DEMO}"
        exec /bin/sh
        ;;
esac
INIT_EOF
	chmod +x "${ROOTFS}/init"
}

build_rootfs() {
	section "Building rootfs"
	rm -rf "${ROOTFS}"
	ensure_dir "${ROOTFS}/bin"
	ensure_dir "${ROOTFS}/dev"
	ensure_dir "${ROOTFS}/proc"
	ensure_dir "${ROOTFS}/sys"
	ensure_dir "${ROOTFS}/tmp"
	ensure_dir "${ROOTFS}/run"
	ensure_dir "${ROOTFS}/usr/bin"
	ensure_dir "${SYMBOLS_DIR}"

	local bb
	bb="$(find_busybox)"
	cp -f "${bb}" "${ROOTFS}/bin/busybox"
	chmod +x "${ROOTFS}/bin/busybox"
	ln -sf busybox "${ROOTFS}/bin/sh"

	install_binary "${ARM64_BIN}/safety-linkd" "${ROOTFS}/usr/bin/safety-linkd"
	install_binary "${ARM64_BIN}/safety-supervisord" "${ROOTFS}/usr/bin/safety-supervisord"
	install_binary "${ARM64_BIN}/safetyctl" "${ROOTFS}/usr/bin/safetyctl"
	write_init
}

verify_rootfs() {
	section "Verifying initramfs content"
	local required=(init bin/busybox bin/sh dev proc sys tmp run usr/bin/safety-linkd usr/bin/safety-supervisord usr/bin/safetyctl)
	for path in "${required[@]}"; do
		[[ -e "${ROOTFS}/${path}" ]] || die "rootfs missing /${path}"
	done
	if [[ -d "${ROOTFS}/build" || -d "${ROOTFS}/usr/src" || -d "${ROOTFS}/opt" ]]; then
		die "rootfs contains forbidden source/tool directories"
	fi
	log "Initramfs content check passed."
}

pack_rootfs() {
	section "Packing initramfs"
	ensure_dir "$(dirname "${OUT}")"
	(
		cd "${ROOTFS}"
		find . -print0 | cpio --null -o -H newc 2>/dev/null
	) | gzip -9 >"${OUT}.part"
	mv "${OUT}.part" "${OUT}"
	log "Initramfs ready: ${OUT}"
}

main() {
	section "Initramfs build"
	check_prereqs
	ensure_arm64_userspace
	build_rootfs
	verify_rootfs
	pack_rootfs
	section "Done"
}

main "$@"
