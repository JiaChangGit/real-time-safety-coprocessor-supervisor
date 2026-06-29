# QEMU Linux

Linux guest 使用 ARM64 `virt` machine、BusyBox static initramfs、built-in `safety_copro` driver。

## 預設命令

`scripts/07_run_qemu_linux.sh` 使用下列設定：

```text
machine: virt
cpu: cortex-a53
smp: 4
memory: 1024M
console: ttyAMA0
link UART: ttyAMA1
network: none
disk: none
graphics: nographic
```

第二個 serial PTY 會寫到 `build/run/linux_uart_pty.txt`。

## Artifacts

| Artifact | 產生方式 |
| -- | -- |
| `build/linux/arch/arm64/boot/Image` | `scripts/02_build_linux_kernel.sh` |
| `build/initramfs/rootfs.cpio.gz` | `scripts/03_build_initramfs.sh` |

Kernel config 從 ARM64 defconfig 出發，再 merge `linux/configs/qemu_arm64_safety_defconfig`。Driver 以 `CONFIG_SAFETY_COPRO=y` built-in。
