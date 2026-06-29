# Zephyr Firmware

Zephyr target 是 `qemu_cortex_a53/qemu_cortex_a53/smp`，使用 2 個 CPU。

## Formal Config

`zephyr/safety_copro_firmware/prj.conf` 讓 formal protocol UART 保持乾淨：

```text
CONFIG_MULTITHREADING=y
CONFIG_SMP=y
CONFIG_MP_MAX_NUM_CPUS=2
CONFIG_SCHED_CPU_MASK=y
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_CONSOLE=n
CONFIG_UART_CONSOLE=n
CONFIG_LOG=n
CONFIG_PRINTK=n
CONFIG_SHELL=n
CONFIG_MAIN_STACK_SIZE=2048
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048
CONFIG_IDLE_STACK_SIZE=1024
CONFIG_HEAP_MEM_POOL_SIZE=2048
CONFIG_BOOT_BANNER=n
```

Firmware tasks：

| Task | 責任 | CPU |
| -- | -- | -- |
| heartbeat | 每 100ms 送 `HEARTBEAT` | CPU0 |
| telemetry | 每 250ms 送 `TELEMETRY` | CPU1 |
| fault monitor | 每 100ms 掃描 fault flags，送 checksum/critical fault event | scheduler |
| command handler | 處理 Linux command 與 recovery | scheduler |

Debug config 可開 shell/logging，但不是 formal demo 預設。
