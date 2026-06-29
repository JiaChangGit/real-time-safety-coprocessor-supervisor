# eBPF CO-RE Tracing Agent — Safety Co-Processor Supervisor

本目錄提供一個以 **libbpf CO-RE (Compile-Once Run-Everywhere)** 實作的即時性能 tracing
agent，量測 `safety-supervisord` daemon 與 `safety_copro` kernel driver 在 QEMU ARM64
Linux guest 內的延遲與 fault-to-recovery 行為。

> 慣例：本 README 與程式碼註解使用台灣繁體中文，技術名詞保留英文；agent 產生的所有
> 報告 (CSV / Markdown) 與 console 輸出皆為**英文**。

---

## 檔案

| 檔案 | 角色 |
|------|------|
| `safety_trace.bpf.c` | BPF CO-RE 程式（attach 到 syscall / sched / safety_copro tracepoints） |
| `safety_trace_user.c` | libbpf userspace loader / reporter（poll ringbuf、輸出報告） |
| `Makefile` | 建置流程（vmlinux.h、BPF object、skeleton、user binary） |
| `README.md` | 本文件 |

建置產物（皆被 `.gitignore`）：`vmlinux.h`、`safety_trace.bpf.o`、`safety_trace.skel.h`、
`safety-trace`。

---

## 依賴 (Prerequisites)

執行 `scripts/00_install_deps.sh` 會安裝以下工具（**本 repo 的 host 上預設沒有
clang/llvm 與 libbpf-dev，必須先跑該 script**）：

- `clang` + `llvm`（含 `llvm-strip`）— 編譯 BPF bytecode 與產生 BTF。
- `libbpf-dev`（libbpf >= 1.0）— userspace 連結 `-lbpf`，並提供 ring buffer / skeleton API。
- `bpftool` — 由 kernel BTF dump `vmlinux.h`、由 BPF object 產生 skeleton。
- `libelf-dev`、`zlib1g-dev` — libbpf 的連結依賴 (`-lelf -lz`)。
- 一個 **啟用 `CONFIG_DEBUG_INFO_BTF=y` 的 kernel**——本專案
  `linux/configs/qemu_arm64_safety_defconfig` 已啟用，使 `/sys/kernel/btf/vmlinux` 存在。

> 此 agent **必須在 QEMU Linux guest 內執行**——也就是載入了 `safety_copro` driver、
> 具備 `safety_copro` tracepoints 的那顆 kernel。在 host 上無法 attach（host kernel
> 沒有這些自訂 tracepoint）。

---

## CO-RE 與 `vmlinux.h` 流程

CO-RE 讓「一次編譯」的 BPF object 能在不同 kernel 版本上執行，無需在目標機器安裝 kernel
headers。其核心是：

1. **`vmlinux.h`** 由目標 kernel 的 BTF（型別資訊）產生，內含所有 kernel struct/enum
   定義：

   ```sh
   bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
   ```

   Makefile 的 `vmlinux.h` target 會自動執行此命令（且**僅在檔案不存在時**生成，避免覆蓋
   為目標 guest 準備好的版本）。此步驟必須在**擁有目標 kernel BTF 的環境**執行，也就是
   QEMU guest 內。

2. BPF 程式 `#include "vmlinux.h"`，以 `clang -g` 編譯時 clang 會在 object 內嵌入 **BTF**。

3. 載入時，**libbpf 依目標 running kernel 的 BTF 做 CO-RE relocation**——自動修正 struct
   欄位 offset。因此即使 guest kernel 與編譯時略有差異，欄位讀取仍正確。

### 關於 `safety_copro` 自訂 tracepoint 的 ctx struct

`safety_copro` 是 driver 自帶的 tracepoint，其 ctx struct **不在 `vmlinux.h`** 內（vmlinux
BTF 只含 in-tree 型別）。因此 `safety_trace.bpf.c` 內**手動宣告** ctx struct：共用的 common
preamble（`common_type` / `common_flags` / `common_preempt_count` / `common_pid`），其後是各
tracepoint 依 `TP_STRUCT__entry` **宣告順序**排列的欄位。

確切的 byte offset 與 padding 可在 guest 內檢視：

```sh
cat /sys/kernel/tracing/events/safety_copro/safety_frame_received/format
cat /sys/kernel/tracing/events/safety_copro/safety_poll_wakeup/format
# ... 其餘 3 個 tracepoint 同理
```

若 `format` 顯示的欄位順序/型別與 `safety_trace.bpf.c` 內的手寫 struct 不一致（例如 driver
改了簽章），必須同步修正手寫 struct。本 agent 的 struct 對應 driver 現行簽章：

- `safety_frame_received(u16 seq, u8 type, u32 qlen)`
- `safety_frame_dropped(u16 seq, u8 type)`
- `safety_timeout_detected(u32 timeout_ms, u32 count)`
- `safety_recovery_queued(u32 count)`
- `safety_poll_wakeup(u32 events)`

> syscall / sched tracepoint 的 ctx（`trace_event_raw_sys_enter` 等）**有**在 `vmlinux.h`
> 內，故走 CO-RE relocation，無須手寫。

---

## 建置 (Build)

在 QEMU guest 內（或交叉編譯時指定 `ARCH=arm64`）：

```sh
cd ebpf
make
```

`make` 會依序：

1. 生成 `vmlinux.h`（若不存在）。
2. `clang -g -O2 -target bpf -D__TARGET_ARCH_<arch> -c safety_trace.bpf.c -o safety_trace.bpf.o`
   接著 `llvm-strip -g safety_trace.bpf.o`（去除 DWARF、保留 BTF）。
3. `bpftool gen skeleton safety_trace.bpf.o > safety_trace.skel.h`。
4. `cc -g -o safety-trace safety_trace_user.c -lbpf -lelf -lz`。

工具與 arch 皆可覆寫：

```sh
make CLANG=clang-18 BPFTOOL=/usr/sbin/bpftool ARCH=arm64
```

`ARCH` 預設由 `uname -m` 推導（`aarch64` -> `arm64`、`x86_64` -> `x86`）。在 ARM64 guest 內
native build 時會自動為 `arm64`；若於 x86_64 host 為 guest 交叉編譯 BPF object，請明確指定
`make ARCH=arm64`。

清理：`make clean`（保留 `vmlinux.h`）或 `make distclean`（連 `vmlinux.h` 一併刪）。

---

## 執行 (Run)

需要 root（attach BPF、讀 tracefs）：

```sh
sudo ./safety-trace --duration 30
```

CLI：

| 旗標 | 說明 |
|------|------|
| `--duration N`, `-d N` | 追蹤 N 秒後自動停止並輸出報告（預設 0 = 直到 SIGINT/SIGTERM） |
| `--report-dir DIR`, `-r DIR` | 報告輸出目錄（預設 `reports`） |
| `--verbose`, `-v` | 顯示 libbpf debug 訊息 |
| `--help` | 用法 |

收到 `Ctrl-C`（SIGINT）或 SIGTERM、或到達 `--duration` 時，agent 停止追蹤、drain ringbuf，
並寫出三份報告。

---

## 產生的報告

寫入 `--report-dir`（預設 `reports/`）：

| 檔案 | 內容 |
|------|------|
| `latency_report.csv` | machine-readable：`metric,unit,count,min,p50,p90,max,avg`（每 metric 一列） |
| `latency_report.md` | human-readable 表格（含每 metric 的 log2 bucket 分佈與 tracepoint 計數） |
| `fault_timeline.md` | 依序的 fault->recovery 事件時間線，並配對計算各週期延遲 (ms) |

---

## Metric 定義（哪個 probe 產生哪個 metric）

| Metric | 單位 | 由哪個 probe 量測 | 意義 |
|--------|------|-------------------|------|
| `read_latency_ns` | ns | `tp/syscalls/sys_enter_read` + `sys_exit_read` | supervisor 一次 `read()` 的耗時（enter->exit delta） |
| `write_latency_ns` | ns | `sys_enter_write` + `sys_exit_write` | supervisor 一次 `write()` 的耗時 |
| `ioctl_latency_ns` | ns | `sys_enter_ioctl` + `sys_exit_ioctl` | supervisor 一次 `ioctl()` 的耗時 |
| `poll_wakeup_ns` | ns | `tp/safety_copro/safety_frame_received` -> `safety_poll_wakeup` | 從一個 frame 進入 RX kfifo 到 poll waiter 被喚醒的延遲 |
| `sched_delay_ns` | ns | `tp/sched/sched_wakeup(_new)` -> `tp/sched/sched_switch` | supervisor task 從「可執行 (woken)」到「實際被排上 CPU」的等待 |
| safety_copro tracepoint counts | 次數 | 5 個 `tp/safety_copro/*` | 每個自訂 tracepoint 的觸發次數 |
| fault-to-recovery timeline | 事件 + ms | `safety_timeout_detected` / `safety_recovery_queued` / recovery 後首個 `safety_frame_received` | 一次 fault 從 timeout 偵測 -> recovery 排入 -> heartbeat 恢復的時間線與各段延遲 |

### 直方圖與 percentile 的近似

延遲 metric 以 **log2 buckets** 累積：slot `i` 涵蓋 `[2^i, 2^(i+1)) ns`（slot 0 含 0）。

- `min` / `max` / `avg`：由 BPF 端**精確**累加的 min/max/sum/count 計算，為精確值。
- `p50` / `p90`：由 bucket 累積分佈**近似**——回傳目標 rank 落入之 bucket 的**上界**
  `2^(slot+1) ns`（slot 0 取 1 ns），屬保守近似上界。

### fault-to-recovery 配對規則

timeline 事件以 ringbuf FIFO 順序保存。報告把事件配對成「fault 週期」：每個 `TIMEOUT` 起
一段；其後第一個 `RECOVERY` 給出 `timeout->recovery` 延遲；其後第一個
`HEARTBEAT_RESTORED`（recovery 之後 driver 收到的第一個 frame）給出 `recovery->restored`
與 `timeout->restored`（fault->recovery 總延遲）。未在追蹤窗內恢復的週期標記為
`n/a (not yet recovered)`。

---

## comm-filter 近似的 caveat

syscall 與 sched 的 tracepoint 是**全系統**的；為了把量測 scope 限縮到 supervisor，本 agent
以 **comm 前綴比對**近似：

- daemon 二進位為 `safety-supervisord`，但 kernel `task->comm` 上限為 `TASK_COMM_LEN`（16，
  可用 15 字元），會被截斷為 `safety-supervis`。
- agent 以 **14 字元前綴 `safety-supervi`** 比對（安全前綴）。

近似的限制：

1. **任何** comm 以此前綴開頭的 process 都會被計入；本專案不存在其他同前綴 process，故為
   足夠精準的近似。若日後新增同前綴的工具，需收緊比對。
2. comm 比對發生在 syscall enter（`bpf_get_current_comm`）與 sched_switch 的 `next_comm`；
   若 supervisor 在 `read()` 期間改名（罕見），可能漏計。
3. syscall latency 以 thread id (tid) 為 key 配對 enter/exit；多執行緒 supervisor 的每條
   thread 各自配對，正確。

如需精確 scoping，可改以 PID 過濾（啟動時把 supervisor PID 寫入一個 BPF map），但會增加
loader 與 supervisor 的耦合；本 agent 選擇低耦合的 comm 前綴近似。

---

## 疑難排解

- **load 失敗 / verifier 錯誤**：用 `-v` 看 libbpf 詳細輸出。
- **attach `safety_copro/*` 失敗**：確認 driver 已載入且
  `/sys/kernel/tracing/events/safety_copro/` 存在。
- **`vmlinux.h` 為空或缺**：確認 guest kernel `CONFIG_DEBUG_INFO_BTF=y` 且
  `/sys/kernel/btf/vmlinux` 存在。
- **欄位讀取數值異常**：以 `.../format` 核對自訂 tracepoint 的欄位 offset 是否與手寫 ctx
  struct 一致。
