#!/usr/bin/env bash
# scripts/00_install_deps.sh - install Ubuntu 24.04 host dependencies

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/lib.sh"

APT_PACKAGES=(
	build-essential git ca-certificates curl wget xz-utils file rsync cpio
	bc flex bison libssl-dev libelf-dev device-tree-compiler
	qemu-system-arm gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
	cmake ninja-build gperf ccache
	python3 python3-venv python3-pip python3-dev
	busybox-static valgrind
	clang llvm lld linux-tools-common libbpf-dev
)

optional_pahole_package() {
	if apt-cache show pahole >/dev/null 2>&1; then
		printf 'pahole'
	else
		printf 'dwarves'
	fi
}

print_versions() {
	section "Version summary"
	if command -v lsb_release >/dev/null 2>&1; then printf 'Ubuntu: %s\n' "$(lsb_release -ds)"; fi
	if command -v qemu-system-aarch64 >/dev/null 2>&1; then qemu-system-aarch64 --version | sed -n '1p'; else printf 'QEMU: not installed\n'; fi
	if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then printf 'aarch64-linux-gnu-gcc: %s\n' "$(aarch64-linux-gnu-gcc -dumpfullversion -dumpversion)"; else printf 'aarch64-linux-gnu-gcc: not installed\n'; fi
	if command -v cmake >/dev/null 2>&1; then cmake --version | sed -n '1p'; else printf 'CMake: not installed\n'; fi
	if command -v ninja >/dev/null 2>&1; then printf 'Ninja: %s\n' "$(ninja --version)"; else printf 'Ninja: not installed\n'; fi
	if command -v python3 >/dev/null 2>&1; then python3 --version; else printf 'Python: not installed\n'; fi
	if command -v west >/dev/null 2>&1; then west --version; elif [[ -x "${BUILD_DIR}/deps/zephyr-venv/bin/west" ]]; then "${BUILD_DIR}/deps/zephyr-venv/bin/west" --version; else printf 'west: not installed\n'; fi
	if [[ -d "${BUILD_DIR}/deps/zephyr-workspace/zephyr/.git" ]]; then printf 'Zephyr revision: %s\n' "$(git -C "${BUILD_DIR}/deps/zephyr-workspace/zephyr" describe --tags --always --dirty)"; else printf 'Zephyr revision: not present\n'; fi
	if [[ -d "${BUILD_DIR}/tools/zephyr-sdk" ]]; then printf 'Zephyr SDK path: %s\n' "${BUILD_DIR}/tools/zephyr-sdk"; else printf 'Zephyr SDK path/version: not verified\n'; fi
	if [[ -f "${BUILD_DIR}/linux/Makefile" ]]; then printf 'Linux kernel source version: %s\n' "$(make -s -C "${BUILD_DIR}/linux" kernelversion)"; else printf 'Linux kernel source version: not present\n'; fi
}

main() {
	section "Dependency installer"
	if ! is_debian; then
		die "apt-get not found. This installer supports Ubuntu/Debian hosts only."
	fi

	local pahole_pkg
	pahole_pkg="$(optional_pahole_package)"
	APT_PACKAGES+=("${pahole_pkg}")

	local SUDO
	SUDO="$(sudo_prefix)"
	log "The following apt packages will be installed with --no-install-recommends:"
	printf '       %s\n' "${APT_PACKAGES[*]}"

	section "apt-get update"
	${SUDO} apt-get update

	section "apt-get install"
	${SUDO} env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${APT_PACKAGES[@]}"

	section "Next steps"
	log "Run scripts/01_fetch_sources.sh to prepare Zephyr v4.4.0 workspace and probe minimal SDK installation."
	log "Run scripts/05_build_userspace.sh host for the host baseline build."
	print_versions
	section "Done"
}

main "$@"
