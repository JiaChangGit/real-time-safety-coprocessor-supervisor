# 未來工作

以下項目刻意不放在目前 implemented scope：

| Item | 原因 |
| -- | -- |
| queue overflow scenario | 超出三個必要 demo |
| SMP stress benchmark | 超出 baseline safety flow |
| watchdog warning fault | heartbeat timeout 由 Linux driver hrtimer 處理 |
| sensor stuck simulation | 目前沒有 sensor subsystem |
| Web UI / REST / gRPC | 不屬於 embedded serial supervision flow |
| Docker / Yocto / disk rootfs | 不符合資源與 image 限制 |

這些項目不應在 README 或 verification report 中描述為已完成。
