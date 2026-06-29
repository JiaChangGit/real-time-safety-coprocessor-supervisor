/* SPDX-License-Identifier: GPL-2.0 */
/*
 * safety_copro_ioctl.h - /dev/safety_copro 的 ioctl UAPI 介面
 *
 * 角色：本檔同時被 kernel driver 與 userspace 工具 (safetyctl) include，
 * 因此必須維持「self-contained」且不依賴任何 driver 內部結構。整數型別一律
 * 使用 __u32 等 kernel UAPI 型別，並以 __KERNEL__ 切換要 include 的 ioctl 標頭，
 * 確保同一份檔案在 kernel space 與 userspace 都能編譯。
 *
 * 注意：所有 log/字串使用英文，僅註解使用台灣繁體中文。
 */

#ifndef SAFETY_COPRO_IOCTL_H
#define SAFETY_COPRO_IOCTL_H

#if defined(__KERNEL__)
#  include <linux/types.h>
#  include <linux/ioctl.h>
#else
#  include <linux/types.h>   /* __u32 等型別 (userspace 仍由 kernel headers 提供) */
#  include <sys/ioctl.h>
#endif

#include "safety_protocol.h"

/* ioctl magic：使用 'S' (Safety)。所有命令編號集中於此避免衝突。 */
#define SAFETY_IOC_MAGIC   'S'

/*
 * SAFETY_IOC_GET_STATS 回傳的彙總統計。
 * 全部使用 __u32 以保證 kernel/userspace ABI 一致 (no padding 議題)。
 * current_state 對應 driver 內部 enum safety_link_state。
 */
struct safety_copro_stats {
	__u32 current_state;          /* enum safety_link_state 的數值 */
	__u32 heartbeat_count;        /* 收到的有效 HEARTBEAT frame 數 */
	__u32 fault_count;            /* INJECT_FAULT + 偵測到的 fault 數 */
	__u32 dropped_frame_count;    /* 因 kfifo 滿而丟棄的 frame 數 */
	__u32 timeout_count;          /* heartbeat watchdog 逾時次數 */
	__u32 retry_count;            /* recovery workqueue 觸發 (重試) 次數 */
	__u32 protocol_error_count;   /* validate 失敗 (magic/version/checksum) 次數 */
	__u32 rx_frames_total;        /* 成功進入 kfifo 的 frame 總數 (per-cpu 彙總) */
	__u32 rx_queue_depth;         /* RX queue 目前 frame 數 */
	__u32 tx_queue_depth;         /* TX command queue 目前 frame 數 */
};

/*
 * linkd transport frame buffer。len 為 data 內有效位元組數，data 存一個完整
 * SafetyFrameHeader + payload。此 ABI 讓 safety-linkd 不需使用 read() 取 TX
 * queue，避免與 safety-supervisord 競爭 RX read path。
 */
struct safety_copro_frame_io {
	__u32 len;
	__u8  data[SAFETY_MAX_FRAME_SIZE];
};

/* ---- ioctl 命令定義 ---- */
#define SAFETY_IOC_GET_STATS \
	_IOR(SAFETY_IOC_MAGIC, 0x01, struct safety_copro_stats)
#define SAFETY_IOC_RESET_STATS \
	_IO(SAFETY_IOC_MAGIC, 0x02)
#define SAFETY_IOC_SET_HB_TIMEOUT_MS \
	_IOW(SAFETY_IOC_MAGIC, 0x03, __u32)
#define SAFETY_IOC_GET_STATE \
	_IOR(SAFETY_IOC_MAGIC, 0x04, __u32)
#define SAFETY_IOC_FORCE_RECOVERY \
	_IO(SAFETY_IOC_MAGIC, 0x05)
#define SAFETY_IOC_INJECT_FAULT \
	_IOW(SAFETY_IOC_MAGIC, 0x06, __u32)   /* arg = enum safety_fault_type */
#define SAFETY_IOC_PUSH_RX_FRAME \
	_IOW(SAFETY_IOC_MAGIC, 0x07, struct safety_copro_frame_io)
#define SAFETY_IOC_POP_TX_FRAME \
	_IOR(SAFETY_IOC_MAGIC, 0x08, struct safety_copro_frame_io)

#define SAFETY_IOC_MAXNR  0x08

#endif /* SAFETY_COPRO_IOCTL_H */
