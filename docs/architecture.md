# 架構與資料流

整條 pipeline 只有一條路：Zephyr 送 frame 出來，經過 pty_bridge、linkd、driver，最後到 supervisor 做決策。每個環節各司其職，不互相代勞。

## System Architecture

```mermaid
flowchart LR
    subgraph HOST["Ubuntu host"]
        BR["pty_bridge\ndrop delay corrupt"]
    end
    subgraph Z["Zephyr QEMU"]
        FW["firmware tasks"]
        ZUART["UART"]
        FW --> ZUART
    end
    subgraph L["Linux QEMU"]
        TTY["ttyAMA1"]
        LINKD["safety-linkd"]
        DEV["/dev/safety_copro"]
        SUP["safety-supervisord"]
        CTL["safetyctl"]
        DBG["debugfs / tracepoints"]
        TTY --> LINKD --> DEV --> SUP
        CTL --> DEV
        DEV --> DBG
    end
    ZUART --> BR --> TTY
    SUP --> R[("reports/")]
```

對應 source：`bridge/`、`userspace/safety-linkd/`、`linux/drivers/safety_copro/`、`userspace/safety-supervisord/`。圖上沒有 socket、database、Web 元件，因為實際就沒有。

## Data Flow

```mermaid
flowchart TB
    A["Zephyr frame"] --> B["pty_bridge ByteFramer"]
    B --> C["Linux ttyAMA1"]
    C --> D["safety-linkd LinkFramer"]
    D --> E["SAFETY_IOC_PUSH_RX_FRAME"]
    E --> F["driver RX queue"]
    F --> G["supervisor read/poll"]
    G --> H["FrameAssembler validate"]
    H --> I["HealthStateMachine"]
    I --> J[("events.jsonl")]
    H --> K["ACK/NACK/recovery command"]
    K --> L["driver TX queue"]
    L --> M["SAFETY_IOC_POP_TX_FRAME"]
    M --> D
```

supervisor 只讀 `/dev/safety_copro`，不碰 UART。linkd 只搬資料，不管 state。

## 狀態機（State Machine）

```mermaid
stateDiagram-v2
    [*] --> BOOTING
    BOOTING --> HEALTHY: first HEARTBEAT received
    HEALTHY --> DEGRADED: heartbeat timeout / fault
    DEGRADED --> RECOVERING: REQUEST_RECOVERY sent
    DEGRADED --> FAILED: repeated failure
    RECOVERING --> HEALTHY: HEARTBEAT restored
    RECOVERING --> FAILED: recovery timeout
    HEALTHY --> SAFE_MODE: critical fault
    DEGRADED --> SAFE_MODE: critical fault
    RECOVERING --> SAFE_MODE: critical fault
    FAILED --> [*]
    SAFE_MODE --> [*]
```

實作在 `userspace/safety-supervisord/health_state_machine.cpp`。bridge、linkd、driver 都不碰 state。

## 啟動流程（Boot Sequence）

Zephyr 通電後，依序拉起四條 task，heartbeat task 最先開始送 frame。

```mermaid
sequenceDiagram
    participant Z as Zephyr QEMU
    participant B as pty_bridge
    participant L as safety-linkd
    participant D as safety_copro driver
    participant S as safety-supervisord
    participant R as reports/

    Note over Z: firmware boots
    Z->>Z: heartbeat task starts (CPU0)
    Z->>Z: telemetry task starts (CPU1)
    Z->>Z: fault monitor starts
    Z->>Z: command handler starts
    loop every 100ms
        Z->>B: HEARTBEAT frame (16 + 0 bytes)
        B->>L: forward bytes
        L->>D: PUSH_RX ioctl
        D->>D: validate magic/version/checksum
        D->>D: push to RX kfifo, wake poll
        D->>S: read() returns HEARTBEAT
        S->>S: FrameAssembler.parse()
        S->>R: event: BOOTING
        S->>S: first HEARTBEAT → state = HEALTHY
        S->>R: event: HEALTHY
    end
```

第一個 HEARTBEAT 讓 supervisor 從 BOOTING 轉 HEALTHY。之後每 100ms 的 HEARTBEAT 會重設 driver 的 hrtimer，不讓 timeout 觸發。

## Heartbeat 超時流程（Heartbeat Timeout Sequence）

pty_bridge 故意丟掉或延遲 HEARTBEAT 時，driver 的 hrtimer 會觸發。

```mermaid
sequenceDiagram
    participant Z as Zephyr
    participant B as pty_bridge
    participant D as safety_copro
    participant S as safety-supervisord
    participant R as reports

    Z->>B: HEARTBEAT
    B--xD: (drop heartbeat)
    Note over D: hrtimer not rearmed
    D->>D: timeout fires
    D->>D: queue fault frame, wake poll
    D->>S: read() returns timeout event
    S->>R: HEALTHY → DEGRADED
    S->>D: write(REQUEST_RECOVERY)
    D->>Z: command forwarded via linkd
    Z->>Z: reset heartbeat state
    Z->>D: HEARTBEAT restored
    D->>S: read() returns HEARTBEAT
    S->>R: DEGRADED → RECOVERING
    S->>R: RECOVERING → HEALTHY
```

`--drop-type 1` 指定只丟 HEARTBEAT frame。整個週期完成後，reports 裡會有一組 `DEGRADED → RECOVERING → HEALTHY` 事件鏈。

## Checksum 錯誤流程（Checksum Error Sequence）

pty_bridge 用 `--corrupt` 隨機翻轉 byte 來模擬線路雜訊。

```mermaid
sequenceDiagram
    participant Z as Zephyr
    participant B as pty_bridge
    participant D as safety_copro
    participant S as safety-supervisord
    participant R as reports

    Z->>B: HEARTBEAT frame (valid CRC)
    B->>B: corrupt bit(s)
    B->>D: corrupted frame
    D->>D: validate → BAD_CHECKSUM
    D->>D: drop frame
    D->>D: increment protocol_error_count
    D->>R: (tracepoint safety_frame_dropped)
    Note over S: supervisor waiting for frame
    S->>S: poll timeout or next read
    S->>S: missing expected heartbeat
    S->>R: event: checksum error detected
    D->>Z: NACK (via linkd TX path)
    Z->>Z: retry logic
    Z->>D: retransmit HEARTBEAT
    D->>S: read() valid HEARTBEAT
    S->>R: HEALTHY (after x missed)
```

這種 case 在 `--corrupt 7 --corrupt-type 1` 下測試：每 7 個 HEARTBEAT 就翻轉一次。

## Heartbeat 時序（Timing Diagram）

```mermaid
timeline
    title Heartbeat & Timeout Timing
    0 ms : Zephyr boots
         : heartbeat task starts
  100 ms : HEARTBEAT #1
         : supervisor → HEALTHY
  200 ms : HEARTBEAT #2
  300 ms : HEARTBEAT #3
  350 ms : pty_bridge drops #4
  450 ms : pty_bridge drops #5
  500 ms : hrtimer fires (~200ms gap)
         : driver generates timeout fault
         : supervisor → DEGRADED
  550 ms : REQUEST_RECOVERY sent
  650 ms : HEARTBEAT restored
         : supervisor → HEALTHY
```

driver 的 hrtimer timeout 預設值決定了多少次 missed HEARTBEAT 才算超時。實際間隔可透過 `SAFETY_IOC_SET_HB_TIMEOUT_MS` 調整。

## Kernel Driver 內部（Kernel Driver Call Flow）

```mermaid
flowchart TD
    W["write() from supervisor"] --> TX["TX command queue"]
    POP["POP_TX ioctl from linkd"] --> TX
    PUSH["PUSH_RX ioctl from linkd"] --> RX["RX queue"]
    RX --> READ["read() from supervisor"]
    RX --> POLL["poll wakeup"]
    HB["HEARTBEAT"] --> TIMER["hrtimer rearm"]
    TIMER --> FAULT["timeout fault frame"]
    FAULT --> RX
```

driver source 在 `linux/drivers/safety_copro/`。裡面用到 miscdevice、kfifo、wait queue、hrtimer、workqueue、debugfs、tracepoint。

## Zephyr Task 互動（Zephyr Task Interaction）

```mermaid
flowchart LR
    HB["heartbeat task\nCPU0 100ms"] --> UART["safety_send_frame"]
    TM["telemetry task\nCPU1 periodic"] --> UART
    FM["fault monitor"] --> UART
    UART --> LINE["binary UART"]
    CMD["command handler"] --> STATE["fault flags / safe mode"]
    STATE --> HB
    STATE --> TM
    STATE --> FM
```

Formal `prj.conf` 關掉 console、UART console、printk、log、shell，確保 UART 只走 protocol frame。Debug config 可以開 shell/logging，但那不是 formal demo 的預設。

## 建置依賴（Build Dependency）

執行 script 前先確認前置產物在不在，沒有的話會跳過或標 not verified。

```mermaid
flowchart TD
    A["00_install_deps.sh"] --> B["01_fetch_sources.sh"]
    A --> C["02_build_linux_kernel.sh"]
    A --> D["05_build_userspace.sh"]
    B --> E["04_build_zephyr.sh"]
    C --> F["03_build_initramfs.sh"]
    C --> G["07_run_qemu_linux.sh"]
    D --> H["06_build_ebpf.sh"]
    E --> I["08_run_qemu_zephyr.sh"]
    F --> G
    G --> J["09_run_pty_bridge.sh"]
    I --> J
    D --> K["10_run_demo.sh"]
    J --> K
    K --> L["11_collect_report.sh"]
    K --> M["run_valgrind.sh"]
```

`05_build_userspace.sh` 不依賴 kernel 或 Zephyr build，是 host 上最快的驗證路徑。

## eBPF Tracing

```mermaid
flowchart LR
    TP["safety_copro tracepoints"] --> BPF["ebpf/safety_trace.bpf.c"]
    SYS["read/write/ioctl timing"] --> BPF
    BPF --> USER["safety_trace_user.c"]
    USER --> REP[("reports/")]
```

eBPF 是 optional，不影響 baseline build。它必須跑在含 BTF 和 `safety_copro` tracepoints 的目標 kernel 上；host kernel attach 會標 `not verified`。

## 整條 pipeline 的相依元件

這張圖把 runtime 的 process 和它們開啟的檔案列出來，方便除錯時釐清哪個 process 讀哪個 device。

```mermaid
flowchart LR
    ZQ["Zephyr QEMU"] -- /dev/pts/X --> PB["pty_bridge"]
    LQ["Linux QEMU"] -- /dev/pts/Y --> PB
    PB -- /dev/pts/Y --> LQ
    LQ -- /dev/ttyAMA1 --> LD["safety-linkd"]
    LD -- ioctl --> DV["/dev/safety_copro"]
    DV -- read/write/poll --> SV["safety-supervisord"]
    DV -- ioctl --> CT["safetyctl"]
    SV -- write --> RP[("reports/")]
```

QEMU 的 PTY path 每次啟動不一定相同，所以 script 執行時會把 PTY 編號寫到 `build/run/` 下的文字檔。
