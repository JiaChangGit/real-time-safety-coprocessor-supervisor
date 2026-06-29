# Tracing

eBPF tracing 是 optional，不阻塞 baseline build。

Files：

```text
ebpf/safety_trace.bpf.c
ebpf/safety_trace_user.c
ebpf/Makefile
ebpf/README.md
```

`scripts/06_build_ebpf.sh` 會檢查 clang、bpftool、libbpf headers 與 BTF。缺少需求時會在 `build/verification/` 寫入 `not verified`。

Host kernel 通常沒有自訂 `safety_copro` tracepoints。Runtime attach 應在目標 Linux guest 內驗證。
