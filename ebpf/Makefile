# Makefile - safety_trace eBPF CO-RE tracing agent
#
# 角色：建置 BPF object（clang -target bpf）、由 bpftool 產生 skeleton header，
# 並以 cc 連結 libbpf 產生 userspace loader/reporter `safety-trace`。
#
# 註解使用台灣繁體中文；技術名詞保留英文。所有工具路徑用 ?= 允許覆寫，例如：
#   make CLANG=clang-18 BPFTOOL=/usr/sbin/bpftool ARCH=arm64
#
# 重要：本專案目標執行環境為 QEMU ARM64 Linux guest。預設 ARCH 由 uname -m 推導，
# 但「CO-RE BPF object 必須以目標 guest 的 arch」編譯。在 ARM64 guest 內 native
# build 時 ARCH 會自動為 arm64；若於 x86_64 host 交叉編譯給 guest，請明確指定
#   make ARCH=arm64

CLANG     ?= clang
LLVM_STRIP ?= llvm-strip
BPFTOOL   ?= bpftool
CC        ?= cc

# 目標 arch 偵測：把 uname -m 正規化成 BPF 慣用名稱（x86 / arm64 ...）。
# 可用 `make ARCH=arm64` 覆寫（QEMU guest 目標）。
UNAME_M := $(shell uname -m)
ARCH ?= $(shell echo $(UNAME_M) | sed \
	-e 's/x86_64/x86/' \
	-e 's/aarch64/arm64/' \
	-e 's/armv7l/arm/' \
	-e 's/ppc64le/powerpc/' \
	-e 's/mips.*/mips/' \
	-e 's/riscv64/riscv/')

# BPF target 旗標
BPF_CFLAGS := -g -O2 -Wall -target bpf -D__TARGET_ARCH_$(ARCH)

# userspace 連結
USER_LDLIBS := -lbpf -lelf -lz

VMLINUX := vmlinux.h

BPF_SRC  := safety_trace.bpf.c
BPF_OBJ  := safety_trace.bpf.o
SKEL     := safety_trace.skel.h
USER_SRC := safety_trace_user.c
USER_BIN := safety-trace

.PHONY: all clean

all: $(USER_BIN)

# --- vmlinux.h：CO-RE 的核心。由執行中 kernel 的 BTF dump 出完整型別資訊。
#     僅在檔案不存在時生成（避免覆蓋為目標 guest 準備好的版本）。
#     必須在「擁有目標 kernel BTF」的環境執行（即 QEMU guest），其 kernel 需
#     CONFIG_DEBUG_INFO_BTF=y（本專案 defconfig 已啟用）。
$(VMLINUX):
	@echo "  GEN     $(VMLINUX) (from /sys/kernel/btf/vmlinux)"
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $(VMLINUX)

# --- BPF object：clang 編譯為 BPF bytecode，再 llvm-strip 去除 DWARF
#     （保留 BTF；BTF 由 clang -g 產生並用於 CO-RE relocation）。
$(BPF_OBJ): $(BPF_SRC) $(VMLINUX)
	@echo "  CLANG   $@ (ARCH=$(ARCH))"
	$(CLANG) $(BPF_CFLAGS) -c $(BPF_SRC) -o $@
	@echo "  STRIP   $@"
	$(LLVM_STRIP) -g $@

# --- skeleton header：bpftool 由 BPF object 產生 open/load/attach 樣板。
$(SKEL): $(BPF_OBJ)
	@echo "  GEN-SKEL $@"
	$(BPFTOOL) gen skeleton $(BPF_OBJ) > $@

# --- userspace loader：依賴 skeleton（其 #include "safety_trace.skel.h"）。
$(USER_BIN): $(USER_SRC) $(SKEL)
	@echo "  CC      $@"
	$(CC) -g -Wall -o $@ $(USER_SRC) $(USER_LDLIBS)

clean:
	@echo "  CLEAN"
	rm -f $(BPF_OBJ) $(SKEL) $(USER_BIN)

# 註：`make distclean` 會連同生成的 vmlinux.h 一併刪除（一般不需要）。
.PHONY: distclean
distclean: clean
	rm -f $(VMLINUX)
