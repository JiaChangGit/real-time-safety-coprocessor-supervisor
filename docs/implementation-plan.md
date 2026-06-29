# 專案重構計畫

本文件記錄本次重構的工作順序與設計決策，讓建置過程中若主機中斷，可以從已完成項目繼續接回。終端機輸出與 script log 維持英文；文件使用台灣繁體中文。

## 已對齊的方向

| 項目 | 決策 |
| -- | -- |
| 資料流 | 嚴格收斂為 `Zephyr UART -> pty_bridge -> Linux ttyAMA1 -> safety-linkd -> /dev/safety_copro -> safety-supervisord -> reports/` |
| 相容性 | 不保留舊的 `safety-supervisord --uart` 主路徑；supervisor 只讀 driver |
| protocol | 建立 `include/safety_protocol.h` 作為 canonical source，再同步到 userspace、Linux driver、Zephyr firmware |
| safety-linkd | 新增 C++17 adapter，只做 `/dev/ttyAMA1` 與 `/dev/safety_copro` 轉接與 `reports/linkd_events.jsonl` |
| driver | 保留 miscdevice；補齊 RX queue 與 TX command queue 的分工，讓 linkd 和 supervisor 不搶同一個 read path |
| Zephyr formal | 關閉 shell、console、printk、logging；移除 formal 路徑中會污染 UART 的輸出 |
| scope | 只保留 `heartbeat_stop`、`checksum_error_response`、`critical_fault`；overflow、SMP stress、watchdog warning、sensor stuck 移到 future work |
| scripts | 補齊 `00` 到 `11`、`run_valgrind.sh`、`99_cleanup.sh`，所有 artifact 只落 `build/`，reports 只落 `reports/` |
| heavy build | Host build/tests 優先跑；Zephyr SDK、kernel、QEMU E2E、eBPF runtime 若環境不足，誠實標記 `not verified` |

## 執行順序

| 順序 | 工作 | 驗證 |
| -- | -- | -- |
| 1 | 建立 canonical `include/safety_protocol.h` 並同步三份 consumer copy | `cmp` 三份 copy 與 canonical |
| 2 | 調整 userspace include path 與 CMake 目標 | `./scripts/05_build_userspace.sh host` |
| 3 | 新增 `userspace/safety-linkd` | build 產生 `safety-linkd`，`--help` 可執行 |
| 4 | driver 補 TX queue/ioctl，linkd 用 ioctl push RX/pop TX | header/build structure check |
| 5 | supervisor live mode 收斂為 driver-only；mock/replay 保留 | Python tests、mock、replay |
| 6 | Zephyr formal 瘦身，debug-only 設定隔離 | prj.conf minimal check；Zephyr build 若工具不足標 not verified |
| 7 | initramfs 只放規格列出的檔案 | rootfs staging content check |
| 8 | 補齊 scripts 與 runtime PTY path handling | script shellcheck-style review、host runnable scripts |
| 9 | README/docs 全面同步，補 Mermaid 與 not verified 表 | Markdown path/link check、forbidden wording check |
| 10 | 執行可行驗證並修正 | host build/tests/mock/replay/Valgrind |

## 中斷恢復

1. 先看 `git status --short` 確認已修改檔案。
2. 讀本文件確認下一步。
3. 優先跑 `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v` 與 `./scripts/05_build_userspace.sh host` 找出目前破口。
4. 不使用 destructive git command；不回復使用者或其他 agent 的既有修改。

## 目前環境觀察

| 項目 | 狀態 |
| -- | -- |
| Ubuntu | 24.04.4 LTS |
| QEMU | 8.2.2 |
| AArch64 GCC | 13.3.0 |
| CMake / Ninja / Python | 3.28.3 / 1.11.1 / 3.12.3 |
| west | global PATH 未安裝；舊 `build/zephyr-venv/bin/west` 為 v1.5.0 |
| Zephyr checkout | 舊 workspace 不符合目標 v4.4.0，需由 `scripts/01_fetch_sources.sh` 準備 |
| Zephyr SDK | `build/tools/zephyr-sdk` 尚未 verified |
| BusyBox | host `/usr/bin/busybox` 是 x86-64，不可放入 ARM64 initramfs |
| pahole | 尚未安裝，eBPF/BTF baseline 標 not verified |
