# SPDX-License-Identifier: GPL-2.0
#
# Safety Co-Processor Supervisor driver 的 Makefile
#
# 以多個 .o 組成單一內建模組 safety_copro.o。tracepoint header
# (safety_copro_trace.h) 透過 main.c 內的 CREATE_TRACE_POINTS 生成代碼，
# 並需要編譯器能找到本 driver 目錄作為 trace include 路徑。
#

obj-$(CONFIG_SAFETY_COPRO) += safety_copro.o

safety_copro-y := safety_copro_main.o \
		  safety_copro_chrdev.o \
		  safety_copro_kfifo.o \
		  safety_copro_timer.o \
		  safety_copro_workqueue.o \
		  safety_copro_debugfs.o \
		  safety_copro_percpu.o

# 讓 <trace/define_trace.h> 能以 TRACE_INCLUDE_PATH 找到本目錄的 trace header。
CFLAGS_safety_copro_main.o := -I$(src)
