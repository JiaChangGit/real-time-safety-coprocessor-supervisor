# Kernel Driver

`linux/drivers/safety_copro/` 實作 built-in miscdevice `/dev/safety_copro`。

## file_operations

| Operation | User | Purpose |
| -- | -- | -- |
| `open()` | `safety-linkd`, `safety-supervisord`, `safetyctl` | attach private_data |
| `release()` | any | 無特殊清理 |
| `read()` | `safety-supervisord` | RX queue → supervisor |
| `write()` | `safety-supervisord`, `safetyctl` | supervisor command → TX queue |
| `poll()` | `safety-supervisord` | RX queue 非空時回報 EPOLLIN |

## ioctl

全部命令定義於 `linux/drivers/safety_copro/safety_copro_ioctl.h`。

| ioctl | Number | User | Purpose |
| -- | -- | -- | -- |
| `SAFETY_IOC_GET_STATS` | `0x01` | `safetyctl` | 讀取彙總統計（state, counts, queue depths） |
| `SAFETY_IOC_RESET_STATS` | `0x02` | `safetyctl` | 重置 driver 內部計數器 |
| `SAFETY_IOC_SET_HB_TIMEOUT_MS` | `0x03` | `safetyctl` | 設定 heartbeat timeout 毫秒數 |
| `SAFETY_IOC_GET_STATE` | `0x04` | `safetyctl` | 讀取目前 driver link state |
| `SAFETY_IOC_FORCE_RECOVERY` | `0x05` | `safetyctl` | 觸發 recovery workqueue |
| `SAFETY_IOC_INJECT_FAULT` | `0x06` | `safetyctl` | 注入 fault（arg = enum safety_fault_type） |
| `SAFETY_IOC_PUSH_RX_FRAME` | `0x07` | `safety-linkd` | UART frame → RX queue |
| `SAFETY_IOC_POP_TX_FRAME` | `0x08` | `safety-linkd` | TX queue → UART |

## debugfs

預期 `/sys/kernel/debug/safety_copro/` 下有：

```text
stats
percpu_stats
ringbuf_status
last_fault
protocol_error
timeline
```

`percpu_stats` 顯示 driver-local per-CPU counters。
