/* SPDX-License-Identifier: GPL-2.0 */
/*
 * safety_copro.h - Safety Co-Processor Supervisor driver 的「私有」共用標頭
 *
 * 角色：定義跨所有 .c 檔共用的核心狀態結構 struct safety_copro_dev、link state
 * enum、timeline 事件、per-cpu 統計結構，以及各子模組對外的函式原型。本檔僅供
 * driver 內部使用 (非 UAPI)。
 *
 * 設計重點：
 *   - 單一 device 實例 (misc device)，所有共享欄位以 spinlock_t lock 保護。
 *   - RX 路徑為「整個 frame」進出 kfifo：每個 slot 是 struct safety_fifo_slot。
 *   - 不使用 RCU；不在 spinlock 內睡眠。
 *
 * 注意：所有 log/字串為英文，僅註解使用台灣繁體中文。
 */

#ifndef SAFETY_COPRO_DRV_H
#define SAFETY_COPRO_DRV_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/miscdevice.h>
#include <linux/atomic.h>
#include <linux/dcache.h>

#include "safety_protocol.h"
#include "safety_copro_ioctl.h"

/* ---- 預設參數 ---- */
#define SAFETY_DEFAULT_HB_TIMEOUT_MS   350u   /* heartbeat watchdog 預設逾時 */
#define SAFETY_DEFAULT_FIFO_DEPTH      64u    /* kfifo 可容納的 frame 數 */
#define SAFETY_TIMELINE_DEPTH          64u    /* timeline ring 事件數 */

/*
 * kfifo slot：儲存「完整 frame 的原始位元組」。kfifo 需要固定大小元素，因此
 * 每個 slot 預留最大 frame 空間，並另記實際長度 len。
 */
struct safety_fifo_slot {
	u16 len;                              /* 實際 frame 位元組數 */
	u8  data[SAFETY_MAX_FRAME_SIZE];      /* header + payload 原始位元組 */
};

/*
 * Kernel 端「link state」(與 userspace health FSM 不同，僅描述與 co-processor
 * 的連線/心跳狀態)。數值會透過 ioctl/debugfs 對外揭露。
 */
enum safety_link_state {
	SAFETY_LINK_BOOTING    = 0,   /* 初始化完成、尚未收到第一個 heartbeat */
	SAFETY_LINK_HB_OK      = 1,   /* heartbeat 正常 */
	SAFETY_LINK_HB_TIMEOUT = 2,   /* watchdog 逾時 */
	SAFETY_LINK_RECOVERING = 3,   /* recovery work 進行中 */
};

/* timeline ring 內單一事件的事件類型 (僅供 debugfs 顯示用)。 */
enum safety_timeline_event {
	SAFETY_TL_NONE          = 0,
	SAFETY_TL_FRAME_RX      = 1,
	SAFETY_TL_FRAME_DROP    = 2,
	SAFETY_TL_HEARTBEAT     = 3,
	SAFETY_TL_TIMEOUT       = 4,
	SAFETY_TL_RECOVERY      = 5,
	SAFETY_TL_FAULT_INJECT  = 6,
};

/* timeline ring 的單筆紀錄。 */
struct safety_timeline_entry {
	u64 ts_ns;                    /* ktime (monotonic) 時間戳 */
	u32 event;                    /* enum safety_timeline_event */
	u16 seq;                      /* 相關 frame 的 sequence_id (若適用) */
};

/* per-CPU 統計結構 (定義於 safety_copro_percpu.c)。 */
struct safety_copro_pcpu {
	u64 rx_frames;                /* 本 CPU 觀察到成功入列的 frame */
	u64 dropped;                  /* 本 CPU 觀察到丟棄的 frame */
	u64 bytes;                    /* 本 CPU 觀察到入列的位元組數 */
};

/* per-CPU 彙總結果 (給 debugfs / ioctl 用)。 */
struct safety_copro_pcpu_sum {
	u64 rx_frames;
	u64 dropped;
	u64 bytes;
};

/*
 * 主裝置結構，單一實例。所有「共享可變」欄位皆由 lock 保護；唯獨 kfifo 自身
 * 的 in/out 也一律在持有 lock 時操作 (因此使用普通 DECLARE_KFIFO 而非
 * lockless 版本)。
 */
struct safety_copro_dev {
	struct miscdevice   miscdev;

	spinlock_t          lock;             /* 保護下列共享狀態與 kfifo */

	/* RX frame kfifo：linkd push、supervisor read，元素為 struct safety_fifo_slot */
	DECLARE_KFIFO_PTR(rx_fifo, struct safety_fifo_slot);
	/* TX command kfifo：supervisor write、linkd pop，元素同樣為完整 frame */
	DECLARE_KFIFO_PTR(tx_fifo, struct safety_fifo_slot);
	unsigned int        fifo_depth;       /* 配置的深度 (frame 數) */
	unsigned int        fifo_high_watermark; /* 觀察到的最大佔用量 */

	wait_queue_head_t   rx_wq;            /* read()/poll() 等待佇列 */
	wait_queue_head_t   tx_wq;            /* linkd poll()/POP_TX 等待佇列 */

	/* link state 與計數器 (lock 保護) */
	enum safety_link_state state;
	u32                 heartbeat_count;
	u32                 fault_count;
	u32                 dropped_frame_count;
	u32                 timeout_count;
	u32                 retry_count;
	u32                 protocol_error_count;

	/* TX 端序號：driver 合成 frame 時自行遞增 */
	u16                 synth_seq;

	/* heartbeat watchdog */
	struct hrtimer      hb_timer;
	u32                 hb_timeout_ms;    /* 目前逾時門檻 (ms) */

	/* recovery 延遲處理 */
	struct work_struct  recovery_work;

	/* 最近一次 fault 紀錄 (lock 保護) */
	u8                  last_fault_type;
	u16                 last_fault_detail;
	u64                 last_fault_ts_ns;

	/* 最近一次 protocol error 紀錄 (lock 保護) */
	int                 last_proto_err;   /* enum safety_validate_result */
	u32                 proto_err_magic;
	u32                 proto_err_version;
	u32                 proto_err_length;
	u32                 proto_err_checksum;

	/* timeline ring (lock 保護)，head 指向下一個寫入位置 */
	struct safety_timeline_entry timeline[SAFETY_TIMELINE_DEPTH];
	u32                 timeline_head;    /* 下一個寫入索引 */
	u32                 timeline_filled;  /* 已填入筆數 (上限 DEPTH) */

	/* per-CPU 統計 */
	struct safety_copro_pcpu __percpu *pcpu;

	/* debugfs 根目錄 */
	struct dentry      *debugfs_dir;
};

/* 全域唯一的裝置實例 (定義於 main.c)。 */
extern struct safety_copro_dev *g_safety_dev;

/* ---- 跨檔案函式原型 ---- */

/* chrdev (file_operations) — safety_copro_chrdev.c */
extern const struct file_operations safety_copro_fops;

/*
 * 將一個「完整 frame 原始位元組」推入 RX kfifo。需在「未持有 lock」時呼叫，
 * 函式內部自行取得 lock。成功 (入列) 回傳 0；kfifo 滿回傳 -ENOSPC (呼叫端據此
 * 累計 dropped 並 trace)。timeline_event 指定要記錄的事件類型。
 *
 * 定義於 safety_copro_kfifo.c。
 */
int safety_fifo_push_frame(struct safety_copro_dev *dev,
			   const u8 *frame, u16 len,
			   u16 seq, u8 type, u32 timeline_event);

/* 取出一個 RX frame 到 slot (lock 保護)。回傳取出的位元組長度，空則回傳 0。 */
u16 safety_fifo_pop_frame(struct safety_copro_dev *dev,
			  struct safety_fifo_slot *slot);

/* 目前 RX kfifo 內 frame 數量 (lock 保護)。 */
unsigned int safety_fifo_len(struct safety_copro_dev *dev);

/* TX command queue：supervisor write() -> linkd ioctl POP_TX。 */
int safety_fifo_push_tx_frame(struct safety_copro_dev *dev,
			      const u8 *frame, u16 len,
			      u16 seq, u8 type);
u16 safety_fifo_pop_tx_frame(struct safety_copro_dev *dev,
			     struct safety_fifo_slot *slot);
unsigned int safety_tx_fifo_len(struct safety_copro_dev *dev);

/*
 * 在「已持有 dev->lock」的情況下，往 timeline ring 追加一筆事件。
 * 由各路徑 (rx/timeout/recovery) 呼叫。定義於 safety_copro_kfifo.c。
 */
void safety_timeline_record_locked(struct safety_copro_dev *dev,
				   u32 event, u16 seq);

/*
 * 合成一個 driver 端 frame (FAULT_EVENT / RECOVERY_REPORT) 並推入 kfifo，
 * 讓 userspace read()/poll() 觀察到。內部自行取得/釋放 lock。
 * 定義於 safety_copro_kfifo.c。
 */
int safety_synth_fault_event(struct safety_copro_dev *dev,
			     u8 fault_type, u8 severity, u16 detail_code);
int safety_synth_recovery_report(struct safety_copro_dev *dev,
				 u8 recovered, u8 new_state);

/* hrtimer watchdog — safety_copro_timer.c */
void safety_timer_init(struct safety_copro_dev *dev);
void safety_timer_destroy(struct safety_copro_dev *dev);
void safety_timer_rearm(struct safety_copro_dev *dev);   /* 重新武裝逾時 */

/* workqueue recovery — safety_copro_workqueue.c */
void safety_recovery_init(struct safety_copro_dev *dev);
void safety_recovery_destroy(struct safety_copro_dev *dev);
void safety_recovery_schedule(struct safety_copro_dev *dev);

/* debugfs — safety_copro_debugfs.c */
int  safety_debugfs_init(struct safety_copro_dev *dev);
void safety_debugfs_destroy(struct safety_copro_dev *dev);

/* per-cpu — safety_copro_percpu.c */
int  safety_percpu_init(struct safety_copro_dev *dev);
void safety_percpu_destroy(struct safety_copro_dev *dev);
void safety_percpu_inc_rx(struct safety_copro_dev *dev, u32 bytes);
void safety_percpu_inc_dropped(struct safety_copro_dev *dev);
void safety_percpu_reset(struct safety_copro_dev *dev);
void safety_percpu_sum(struct safety_copro_dev *dev,
		       struct safety_copro_pcpu_sum *out);

/* 共用工具 — main.c */
const char *safety_link_state_name(enum safety_link_state st);
u32 safety_now_ms(void);   /* monotonic uptime in ms (給 frame timestamp 用) */

#endif /* SAFETY_COPRO_DRV_H */
