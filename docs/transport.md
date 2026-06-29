# 傳輸層

Transport 拆成兩個程式，避免混淆責任：

| Program | Role |
| -- | -- |
| `pty_bridge` | host 端在兩個 QEMU PTY 之間轉送 bytes/frame |
| `safety-linkd` | Linux guest 內連接 `/dev/ttyAMA1` 與 `/dev/safety_copro` |

`pty_bridge` 支援 `--drop`、`--delay-ms`、`--corrupt`、`--log FILE`（CSV 輸出，無預設路徑）。它不維護 health state。

`safety-linkd` 使用：

```text
SAFETY_IOC_PUSH_RX_FRAME: UART RX -> driver RX queue
SAFETY_IOC_POP_TX_FRAME: driver TX queue -> UART TX
```

`safety-linkd` logs 寫到 `reports/linkd_events.jsonl`。`pty_bridge` 需以 `--log` 指定 CSV 輸出檔。
