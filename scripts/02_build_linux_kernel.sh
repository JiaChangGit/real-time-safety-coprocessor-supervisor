#!/usr/bin/env bash
# scripts/02_build_linux_kernel.sh - fetch, patch, and build ARM64 Linux kernel (6.12 LTS)
#
# 流程：
#   1) 下載 linux-${KVER}.tar.xz 至 build/（已存在則略過）
#   2) 解壓到 build/linux（冪等：以 .extracted 標記）
#   3) 複製 driver 原始碼到 build/linux/drivers/safety_copro/
#   4) 套用 kernel-patch 把 driver 接進 Kconfig/Makefile；失敗則改用冪等 sed/grep 插入
#   5) 以 arm64 defconfig + 本專案 fragment 合併，並確保 SAFETY_COPRO / DEBUG_FS / BTF / FTRACE 開啟
#   6) make olddefconfig + make Image（平行度 JOBS，預設 2）
#   7) 驗證 build/linux/arch/arm64/boot/Image 與 CONFIG_SAFETY_COPRO=y
#
# 受限主機注意：kernel 建置吃 RAM/CPU，JOBS 預設 2。
# 冪等：可重複執行；已完成的步驟會被偵測並略過。

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

# ---- 可覆寫的 kernel 版本（預設一個具體的 6.12.x LTS 點版）----
KVER="${KVER:-6.12.30}"
KMAJOR="6"  # cdn.kernel.org 路徑用的主版本目錄 (v6.x)

TARBALL="linux-${KVER}.tar.xz"
TARBALL_PATH="${BUILD_DIR}/${TARBALL}"
KURL="https://cdn.kernel.org/pub/linux/kernel/v${KMAJOR}.x/${TARBALL}"
SRC_DIR="${BUILD_DIR}/linux"
EXTRACT_MARK="${SRC_DIR}/.extracted-${KVER}"

DRIVER_SRC="${ROOT}/linux/drivers/safety_copro"
PATCH_FILE="${ROOT}/linux/kernel-patches/0001-add-safety-copro-driver.patch"
DEFCONFIG_FRAGMENT="${ROOT}/linux/configs/qemu_arm64_safety_defconfig"

ARCH="arm64"

check_prereqs() {
	section "Checking prerequisites"
	require_cmd make build-essential
	require_cmd flex flex
	require_cmd bison bison
	require_cmd bc bc
	require_cmd "${CROSS_COMPILE}gcc" gcc-aarch64-linux-gnu
	# 下載工具：wget 或 curl 至少一個。
	if ! command -v wget >/dev/null 2>&1 && ! command -v curl >/dev/null 2>&1; then
		die "neither 'wget' nor 'curl' found. Install one with: sudo apt-get install -y wget  (or run scripts/00_install_deps.sh)"
	fi
	# libssl/libelf 為 kernel 必需的開發標頭（無對應指令，提示套件即可）。
	require_file "${DRIVER_SRC}" "driver sources expected at linux/drivers/safety_copro/ — repo layout broken?"
	require_file "${DEFCONFIG_FRAGMENT}" "config fragment expected at linux/configs/qemu_arm64_safety_defconfig"
	log "Prerequisites OK (JOBS=${JOBS}, KVER=${KVER})."
}

download_kernel() {
	section "Downloading Linux kernel ${KVER}"
	ensure_dir "${BUILD_DIR}"
	if [[ -f "${TARBALL_PATH}" ]]; then
		log "Tarball already present: ${TARBALL_PATH} (skip download)."
		return 0
	fi
	log "Fetching ${KURL}"
	if command -v wget >/dev/null 2>&1; then
		wget -O "${TARBALL_PATH}.part" "${KURL}" || die "kernel download failed (wget). Check network / KVER=${KVER} validity."
	else
		curl -fL -o "${TARBALL_PATH}.part" "${KURL}" || die "kernel download failed (curl). Check network / KVER=${KVER} validity."
	fi
	mv "${TARBALL_PATH}.part" "${TARBALL_PATH}"
	log "Downloaded ${TARBALL_PATH}."
}

extract_kernel() {
	section "Extracting kernel source"
	if [[ -f "${EXTRACT_MARK}" ]]; then
		log "Source already extracted at ${SRC_DIR} (skip)."
		return 0
	fi
	# 若有半成品舊目錄，清掉重來以確保乾淨。
	if [[ -d "${SRC_DIR}" ]]; then
		warn "Removing stale ${SRC_DIR} before re-extracting."
		rm -rf "${SRC_DIR}"
	fi
	ensure_dir "${SRC_DIR}"
	# --strip-components=1：把 linux-${KVER}/ 內容直接放進 build/linux/。
	tar -xf "${TARBALL_PATH}" -C "${SRC_DIR}" --strip-components=1 \
		|| die "failed to extract ${TARBALL_PATH}."
	touch "${EXTRACT_MARK}"
	log "Extracted to ${SRC_DIR}."
}

copy_driver() {
	section "Copying safety_copro driver into kernel tree"
	local dst="${SRC_DIR}/drivers/safety_copro"
	ensure_dir "${dst}"
	# -a 保留屬性；以 rsync 風格的 cp 覆寫（冪等）。
	cp -af "${DRIVER_SRC}/." "${dst}/" \
		|| die "failed to copy driver sources into ${dst}."
	log "Driver sources in place at ${dst}."
}

# ---- 把 driver 接進 drivers/Kconfig 與 drivers/Makefile ----
# 優先用 git apply / patch；若 patch 因 context 不符失敗，改用冪等 sed/grep 插入。
wire_driver() {
	section "Wiring driver into drivers/Kconfig and drivers/Makefile"
	local kconfig="${SRC_DIR}/drivers/Kconfig"
	local makefile="${SRC_DIR}/drivers/Makefile"
	local kconfig_line='source "drivers/safety_copro/Kconfig"'
	local makefile_line='obj-$(CONFIG_SAFETY_COPRO)	+= safety_copro/'

	# 若兩處都已含目標行，視為已接好（冪等）。
	if grep -qF 'drivers/safety_copro/Kconfig' "${kconfig}" 2>/dev/null \
		&& grep -qF 'CONFIG_SAFETY_COPRO' "${makefile}" 2>/dev/null; then
		log "Driver already wired into Kconfig and Makefile (skip)."
		return 0
	fi

	# 嘗試 1：git apply（若是 git 樹）。多數情況 build/linux 非 git，故會失敗→fallback。
	if [[ -f "${PATCH_FILE}" ]] && command -v git >/dev/null 2>&1 \
		&& git -C "${SRC_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
		&& git -C "${SRC_DIR}" apply --check "${PATCH_FILE}" >/dev/null 2>&1; then
		git -C "${SRC_DIR}" apply "${PATCH_FILE}" && {
			log "Driver wired via git apply."
			return 0
		}
	fi

	# 嘗試 2：patch -p1（不依賴 git）。
	if [[ -f "${PATCH_FILE}" ]] && command -v patch >/dev/null 2>&1 \
		&& patch -p1 -N --dry-run -d "${SRC_DIR}" <"${PATCH_FILE}" >/dev/null 2>&1; then
		patch -p1 -N -d "${SRC_DIR}" <"${PATCH_FILE}" && {
			log "Driver wired via patch -p1."
			return 0
		}
	fi

	# Fallback：冪等 sed/grep 插入。patch 常因上游 context 行不符而失敗，
	# 此 fallback 對任意 6.12.x 都穩定可用。
	warn "Patch did not apply cleanly; falling back to idempotent text insertion."

	# Kconfig：在 endmenu 之前插入 source 行（若尚未存在）。
	if ! grep -qF 'drivers/safety_copro/Kconfig' "${kconfig}"; then
		# 在「最後一個 endmenu」前插入；用 awk 較 sed 的 in-place 安全。
		awk -v line="${kconfig_line}" '
			# 逐行緩存，遇到 endmenu 時先輸出我們的 source 再輸出 endmenu。
			/^endmenu/ && !done { print line; print ""; done=1 }
			{ print }
		' "${kconfig}" >"${kconfig}.tmp" && mv "${kconfig}.tmp" "${kconfig}"
		grep -qF 'drivers/safety_copro/Kconfig' "${kconfig}" \
			|| die "failed to insert source line into drivers/Kconfig."
		log "Inserted Kconfig source line."
	fi

	# Makefile：在檔尾 append obj 行（若尚未存在）。
	if ! grep -qF 'CONFIG_SAFETY_COPRO' "${makefile}"; then
		printf '\n%s\n' "${makefile_line}" >>"${makefile}"
		grep -qF 'CONFIG_SAFETY_COPRO' "${makefile}" \
			|| die "failed to append obj line into drivers/Makefile."
		log "Appended Makefile obj line."
	fi
	log "Driver wired via text insertion."
}

configure_kernel() {
	section "Configuring kernel (defconfig + project fragment)"
	# 先把 fragment 放進 arch/arm64/configs/，便於人工 inspect（非必要但符合慣例）。
	cp -f "${DEFCONFIG_FRAGMENT}" \
		"${SRC_DIR}/arch/arm64/configs/qemu_arm64_safety_defconfig"

	# 基底 arm64 defconfig。
	make -C "${SRC_DIR}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" defconfig \
		|| die "make defconfig failed."

	# 合併本專案 fragment（merge_config.sh 會保留基底並覆寫我們指定的項目）。
	"${SRC_DIR}/scripts/kconfig/merge_config.sh" -m \
		-O "${SRC_DIR}" \
		"${SRC_DIR}/.config" "${DEFCONFIG_FRAGMENT}" \
		|| die "merge_config.sh failed to merge the project fragment."

	# 明確啟用關鍵選項（避免 fragment 被 olddefconfig 依賴關係吃掉）。
	pushd "${SRC_DIR}" >/dev/null
	./scripts/config --enable SAFETY_COPRO
	./scripts/config --enable DEBUG_FS
	./scripts/config --enable DEBUG_INFO_BTF
	./scripts/config --enable FTRACE
	./scripts/config --enable BPF_SYSCALL
	popd >/dev/null

	# olddefconfig：依相依關係解析出最終 .config。
	make -C "${SRC_DIR}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" olddefconfig \
		|| die "make olddefconfig failed."
	log "Kernel configured."
}

build_kernel() {
	section "Building kernel Image (ARCH=${ARCH}, JOBS=${JOBS})"
	make -C "${SRC_DIR}" -j"${JOBS}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" Image \
		|| die "kernel Image build failed. On a constrained host try a lower JOBS (e.g. JOBS=2)."
	log "Kernel Image built."
}

verify_and_install() {
	section "Verifying config and installing Image"
	local cfg="${SRC_DIR}/.config"
	# 驗證 CONFIG_SAFETY_COPRO=y 真的進了最終 .config。
	if ! grep -q '^CONFIG_SAFETY_COPRO=y$' "${cfg}"; then
		die "CONFIG_SAFETY_COPRO=y is NOT present in the final .config (${cfg}). Driver would not be built in."
	fi
	log "Verified: CONFIG_SAFETY_COPRO=y."

	local img="${SRC_DIR}/arch/${ARCH}/boot/Image"
	require_file "${img}" "kernel build did not produce arch/${ARCH}/boot/Image."
	log "Kernel image ready -> ${img}"
}

main() {
	section "Linux kernel build (${KVER}, ARM64)"
	check_prereqs
	download_kernel
	extract_kernel
	copy_driver
	wire_driver
	configure_kernel
	build_kernel
	verify_and_install
	section "Done"
	log "Kernel ready: ${SRC_DIR}/arch/${ARCH}/boot/Image"
}

main "$@"
