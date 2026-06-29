#!/usr/bin/env bash
# scripts/01_fetch_sources.sh - fetch Zephyr v4.4.0 workspace and probe SDK strategy

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

ZEPHYR_PROJECT="${BUILD_DIR}/deps/zephyr-workspace"
ZEPHYR_BASE_DIR="${ZEPHYR_PROJECT}/zephyr"
VENV="${BUILD_DIR}/deps/zephyr-venv"
WEST="${VENV}/bin/west"
SDK_DIR="${BUILD_DIR}/tools/zephyr-sdk"
VERIFY_DIR="${BUILD_DIR}/verification"
SDK_HELP="${VERIFY_DIR}/west_sdk_help.txt"
ZEPHYR_REV="v4.4.0"
GROUP_FILTER="-optional,-experimental"
SDK_RELEASE_VERSION="1.0.1"
SDK_DOWNLOAD_DIR="${BUILD_DIR}/downloads/zephyr-sdk-${SDK_RELEASE_VERSION}"
SDK_RELEASE_URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_RELEASE_VERSION}"
SDK_BASE_ARCHIVE="zephyr-sdk-${SDK_RELEASE_VERSION}_linux-x86_64_minimal.tar.xz"
SDK_BASE_SHA256="ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb"
SDK_TARGET_ARCHIVE="toolchain_gnu_linux-x86_64_aarch64-zephyr-elf.tar.xz"
SDK_TARGET_SHA256="01c0cfe1daaab2d2a0f71165f11a066d983ef97e51c4a3753186afa6ae55c24b"

check_prereqs() {
	section "Checking fetch prerequisites"
	require_cmd git git
	require_cmd python3 python3
	if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
		die "neither curl nor wget found. Install one before fetching sources."
	fi
	ensure_dir "${BUILD_DIR}/deps"
	ensure_dir "${BUILD_DIR}/tools"
	ensure_dir "${VERIFY_DIR}"
}

download_file() {
	local url="$1"
	local out="$2"
	if [[ -s "${out}" ]]; then
		log "Download already present: ${out}"
		return 0
	fi
	if command -v wget >/dev/null 2>&1; then
		wget -O "${out}.part" "${url}" || die "download failed: ${url}"
	else
		curl -fL -o "${out}.part" "${url}" || die "download failed: ${url}"
	fi
	mv "${out}.part" "${out}"
}

verify_sha256() {
	local sha="$1"
	local path="$2"
	printf '%s  %s\n' "${sha}" "${path}" | sha256sum -c - \
		|| die "sha256 verification failed: ${path}"
}

setup_west() {
	section "Preparing Python venv and west"
	if [[ ! -x "${VENV}/bin/python3" ]]; then
		python3 -m venv "${VENV}"
	fi
	"${VENV}/bin/python3" -m pip install --upgrade pip west
	"${WEST}" --version
}

west_ws() {
	(
		cd "${ZEPHYR_PROJECT}"
		"${WEST}" "$@"
	)
}

init_zephyr() {
	section "Preparing Zephyr workspace (${ZEPHYR_REV})"
	if [[ ! -d "${ZEPHYR_PROJECT}/.west" ]]; then
		"${WEST}" init -m https://github.com/zephyrproject-rtos/zephyr \
			--mr "${ZEPHYR_REV}" "${ZEPHYR_PROJECT}"
	else
		log "Zephyr west workspace already exists: ${ZEPHYR_PROJECT}"
	fi

	west_ws config --local manifest.group-filter -- "${GROUP_FILTER}" \
		|| warn "Could not persist manifest.group-filter; continuing with explicit west update --group-filter."
	log "manifest.group-filter=$(west_ws config manifest.group-filter 2>/dev/null || true)"

	local update_args=(update "--group-filter=${GROUP_FILTER}")
	if "${WEST}" update --help 2>/dev/null | grep -q -- '--narrow'; then
		update_args+=(--narrow)
		log "west update supports --narrow; using it."
	else
		warn "west update does not support --narrow; falling back to standard west update."
	fi
	if "${WEST}" update --help 2>/dev/null | grep -q -- '--fetch-opt'; then
		update_args+=(-o=--depth=1)
		log "west update supports --fetch-opt; using git fetch --depth=1."
	else
		warn "west update does not support --fetch-opt; falling back without shallow clone."
	fi
	west_ws "${update_args[@]}"
	west_ws zephyr-export

	if [[ -d "${ZEPHYR_BASE_DIR}/.git" ]]; then
		log "Zephyr revision: $(git -C "${ZEPHYR_BASE_DIR}" describe --tags --always --dirty)"
	fi
}

install_python_requirements() {
	section "Installing Zephyr Python build requirements"
	require_file "${ZEPHYR_BASE_DIR}/scripts/requirements-base.txt" \
		"Zephyr requirements file missing; west update may have failed."
	"${VENV}/bin/python3" -m pip install -r "${ZEPHYR_BASE_DIR}/scripts/requirements-base.txt" \
		|| die "failed to install Zephyr Python build requirements."
}

install_minimal_sdk() {
	section "Installing minimal Zephyr SDK"
	if [[ "$(uname -m)" != "x86_64" ]]; then
		printf 'not verified: minimal SDK installer supports only linux-x86_64 in this script\n' > \
			"${VERIFY_DIR}/zephyr_sdk.not_verified"
		warn "Minimal SDK auto-install supports only linux-x86_64 hosts."
		return 0
	fi

	if [[ -f "${SDK_DIR}/cmake/Zephyr-sdkConfig.cmake" && \
	      -x "${SDK_DIR}/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-gcc" ]]; then
		log "Minimal Zephyr SDK already present: ${SDK_DIR}"
		"${SDK_DIR}/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-gcc" --version | sed -n '1p' || true
		return 0
	fi

	require_cmd tar tar
	require_cmd sha256sum coreutils
	ensure_dir "${SDK_DOWNLOAD_DIR}"
	ensure_dir "${BUILD_DIR}/tools"

	local base_path="${SDK_DOWNLOAD_DIR}/${SDK_BASE_ARCHIVE}"
	local target_path="${SDK_DOWNLOAD_DIR}/${SDK_TARGET_ARCHIVE}"
	download_file "${SDK_RELEASE_URL}/${SDK_BASE_ARCHIVE}" "${base_path}"
	download_file "${SDK_RELEASE_URL}/${SDK_TARGET_ARCHIVE}" "${target_path}"
	verify_sha256 "${SDK_BASE_SHA256}" "${base_path}"
	verify_sha256 "${SDK_TARGET_SHA256}" "${target_path}"

	rm -rf "${SDK_DIR}.part"
	ensure_dir "${SDK_DIR}.part"
	tar -xf "${base_path}" -C "${SDK_DIR}.part" --strip-components=1 \
		|| die "failed to extract Zephyr SDK minimal base."
	ensure_dir "${SDK_DIR}.part/gnu"
	tar -xf "${target_path}" -C "${SDK_DIR}.part/gnu" \
		|| die "failed to extract aarch64-zephyr-elf toolchain."
	require_file "${SDK_DIR}.part/cmake/Zephyr-sdkConfig.cmake" \
		"minimal SDK base did not provide CMake metadata."
	require_file "${SDK_DIR}.part/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-gcc" \
		"aarch64-zephyr-elf toolchain missing after extraction."
	rm -rf "${SDK_DIR}"
	mv "${SDK_DIR}.part" "${SDK_DIR}"
	log "Minimal Zephyr SDK installed: ${SDK_DIR}"
}

probe_sdk() {
	section "Probing Zephyr SDK minimal install"
	log "Preferred SDK install dir: ${SDK_DIR}"

	if [[ -d "${SDK_DIR}" ]]; then
		log "Zephyr SDK path exists: ${SDK_DIR}"
		if [[ -x "${SDK_DIR}/setup.sh" ]]; then
			"${SDK_DIR}/setup.sh" '-?' | sed -n '1,40p' || true
		fi
		return 0
	fi

	if "${WEST}" sdk install --help >"${SDK_HELP}" 2>&1; then
		log "west sdk install is available. Inspecting options."
		if grep -q -- '--install-dir' "${SDK_HELP}" && \
		   grep -Eq -- '(-t|--toolchain)' "${SDK_HELP}"; then
			warn "Minimal SDK install appears supported, but toolchain names must be verified manually for Zephyr ${ZEPHYR_REV}."
			printf 'not verified: minimal Zephyr SDK toolchain name for qemu_cortex_a53 was not proven\n' > \
				"${VERIFY_DIR}/zephyr_sdk.not_verified"
			return 0
		fi
	fi

	warn "Could not verify a precise minimal Zephyr SDK install command. Not downloading a full SDK."
	printf 'not verified: no verified minimal Zephyr SDK install command was found\n' > \
		"${VERIFY_DIR}/zephyr_sdk.not_verified"
}

print_versions() {
	section "Version summary"
	if command -v lsb_release >/dev/null 2>&1; then lsb_release -ds || true; fi
	if command -v qemu-system-aarch64 >/dev/null 2>&1; then qemu-system-aarch64 --version | sed -n '1p' || true; fi
	if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then aarch64-linux-gnu-gcc --version | sed -n '1p' || true; fi
	if command -v cmake >/dev/null 2>&1; then cmake --version | sed -n '1p' || true; fi
	if command -v ninja >/dev/null 2>&1; then printf 'Ninja %s\n' "$(ninja --version)"; fi
	python3 --version || true
	"${WEST}" --version || true
	if [[ -d "${ZEPHYR_BASE_DIR}/.git" ]]; then git -C "${ZEPHYR_BASE_DIR}" describe --tags --always --dirty || true; fi
	if [[ -d "${SDK_DIR}" ]]; then printf 'Zephyr SDK path: %s\n' "${SDK_DIR}"; else printf 'Zephyr SDK path: not verified\n'; fi
	if [[ -f "${BUILD_DIR}/linux/Makefile" ]]; then make -s -C "${BUILD_DIR}/linux" kernelversion || true; else printf 'Linux kernel source version: not present\n'; fi
}

main() {
	section "Fetch sources"
	check_prereqs
	setup_west
	init_zephyr
	install_python_requirements
	install_minimal_sdk
	probe_sdk
	print_versions
	section "Done"
	log "Source fetch step complete."
}

main "$@"
