# 驗證

## 必要 Host 檢查

```sh
./scripts/05_build_userspace.sh host
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v
mkdir -p reports
./build/userspace/bin/safety-supervisord --mock-device tests/fixtures/fault_log.bin --report-dir reports
./build/userspace/bin/safety-supervisord --replay reports/events.jsonl --report-dir reports
./scripts/run_valgrind.sh
```

## Optional 檢查

| Check | Status rule |
| -- | -- |
| Linux kernel build | 只有 `scripts/02_build_linux_kernel.sh` 完成後才是 pass |
| Initramfs content | 只有 `scripts/03_build_initramfs.sh` 搭配 AArch64 BusyBox 完成後才是 pass |
| Zephyr build | 只有 SDK 與 `scripts/04_build_zephyr.sh formal` 完成後才是 pass |
| Dual QEMU | 只有兩個 PTY files 與 bridge 成功執行後才是 pass |
| eBPF runtime | 只有在含 BTF 與 custom tracepoint 的目標 kernel 內執行成功才是 pass |

未執行的 optional checks 必須報告為 `not verified`。
