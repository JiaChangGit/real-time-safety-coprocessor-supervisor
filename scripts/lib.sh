#!/usr/bin/env bash
# scripts/lib.sh - 全專案共用的 shell 輔助函式庫（被 source，不直接執行）
#
# 本檔提供統一的 log/warn/die/section/require_cmd 等輔助，所有 build/release
# 腳本以下列樣板載入：
#
#   #!/usr/bin/env bash
#   set -euo pipefail
#   ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
#   source "${ROOT}/scripts/lib.sh"
#
# 設計原則：
#   - 註解用台灣繁體中文；所有 echo/log/error 輸出一律英文（避免 mojibake）。
#   - 不在此檔執行任何有副作用的動作（純函式 + 常數定義）。
#   - 受限主機假設：10GB RAM / ~60GB disk / 8 cores。建置平行度預設 2。

# ---- 防止重複載入（多腳本互相 source 時）----
if [[ -n "${__SAFETY_LIB_SH_LOADED:-}" ]]; then
	return 0 2>/dev/null || true
fi
__SAFETY_LIB_SH_LOADED=1

# ---- 顏色：僅在輸出為 TTY 時啟用，避免污染 log 檔 ----
if [[ -t 1 ]]; then
	__C_RED="$(printf '\033[31m')"
	__C_YEL="$(printf '\033[33m')"
	__C_GRN="$(printf '\033[32m')"
	__C_BLU="$(printf '\033[36m')"
	__C_BLD="$(printf '\033[1m')"
	__C_RST="$(printf '\033[0m')"
else
	__C_RED="" __C_YEL="" __C_GRN="" __C_BLU="" __C_BLD="" __C_RST=""
fi

# ---- 專案根目錄：若呼叫端未設，依本檔位置推導 ----
# 注意：呼叫端通常已自行設定 ROOT；此處僅作為後備，且絕不寫死 /home/louis。
if [[ -z "${ROOT:-}" ]]; then
	ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
export ROOT

# ---- 標準目錄：所有產物落 build/、所有報告落 reports/，不污染原始碼樹 ----
BUILD_DIR="${ROOT}/build"
REPORTS_DIR="${ROOT}/reports"
export BUILD_DIR REPORTS_DIR

# ---- 建置平行度：受限主機，預設 2 ----
# 呼叫端可用環境變數覆寫，例如 `JOBS=2 ./scripts/02_build_linux_kernel.sh`。
JOBS="${JOBS:-2}"
export JOBS

# ---- log()：一般資訊（綠色標籤）----
log() {
	printf '%s[ OK ]%s %s\n' "${__C_GRN}" "${__C_RST}" "$*"
}

# ---- warn()：非致命警告（黃色，輸出至 stderr）----
warn() {
	printf '%s[WARN]%s %s\n' "${__C_YEL}" "${__C_RST}" "$*" >&2
}

# ---- die()：致命錯誤，印英文訊息至 stderr 後以非零碼結束 ----
# 用法：die "human readable English message"
die() {
	printf '%s[FAIL]%s %s\n' "${__C_RED}" "${__C_RST}" "$*" >&2
	exit 1
}

# ---- section()：印一段醒目的區塊標題，便於閱讀長 log ----
section() {
	printf '\n%s==== %s ====%s\n' "${__C_BLU}${__C_BLD}" "$*" "${__C_RST}"
}

# ---- require_cmd()：檢查指令是否存在；缺少時給出可操作的安裝提示 ----
# 用法：require_cmd <command> [apt-package]
#   第 2 參數為對應的 Debian/Ubuntu 套件名（預設等於指令名）。
require_cmd() {
	local cmd="$1"
	local pkg="${2:-$1}"
	if ! command -v "${cmd}" >/dev/null 2>&1; then
		die "required tool '${cmd}' not found. Install it on Debian/Ubuntu with: sudo apt-get install -y ${pkg}  (or run scripts/00_install_deps.sh)"
	fi
}

# ---- require_file()：檢查檔案存在；缺少時提示先跑哪個腳本 ----
# 用法：require_file <path> "<English hint, e.g. 'run scripts/02_build_linux_kernel.sh first'>"
require_file() {
	local path="$1"
	local hint="${2:-}"
	if [[ ! -e "${path}" ]]; then
		if [[ -n "${hint}" ]]; then
			die "required file '${path}' is missing. ${hint}"
		fi
		die "required file '${path}' is missing."
	fi
}

# ---- ensure_dir()：確保目錄存在（mkdir -p，冪等）----
ensure_dir() {
	mkdir -p "$1"
}

# ---- need_root_or_sudo()：回傳要在前面加的 sudo 前綴（root 則為空）----
# 非 root 時若沒有 sudo，回傳錯誤訊息。輸出寫到 stdout 供 $(...) 捕捉。
sudo_prefix() {
	if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
		printf ''
	elif command -v sudo >/dev/null 2>&1; then
		printf 'sudo'
	else
		die "this operation needs root privileges but neither root nor 'sudo' is available. Re-run as root."
	fi
}

# ---- is_debian()：偵測是否為 Debian/Ubuntu 系（有 apt-get）----
is_debian() {
	command -v apt-get >/dev/null 2>&1
}

# ---- 標準交叉工具鏈前綴（ARM64）----
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
export CROSS_COMPILE
