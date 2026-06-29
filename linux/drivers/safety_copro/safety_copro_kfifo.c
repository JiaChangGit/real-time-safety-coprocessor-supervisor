// SPDX-License-Identifier: GPL-2.0
/*
 * safety_copro_kfifo.c - RX frame kfifo、timeline ring 與 driver 合成 frame
 *
 * 角色：本檔負責「整個 frame 的原始位元組」進出 RX kfifo 的封裝、timeline
 * ring 的記錄，以及由 driver 自行合成 FAULT_EVENT / RECOVERY_REPORT frame
 * 並推入 kfifo (讓 userspace read()/poll() 能觀察到 kernel 偵測到的事件)。
 *
 * 鎖規則：所有 kfifo 操作與 timeline 寫入皆在持有 dev->lock 時進行。對外的
 * push/pop helper 會自行取得/釋放 lock；標記為 _locked 的 helper 則要求呼叫端
 * 已持有 lock。不在 lock 內睡眠 (kfifo_in/out 為非睡眠操作)。
 */

#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/string.h>

#include "safety_copro.h"
#include "safety_copro_trace.h"

/*
 * safety_timeline_record_locked - 追加一筆 timeline 事件 (呼叫端須持有 lock)。
 * timeline 為環狀緩衝，head 指向下一個寫入位置；filled 上限為 DEPTH。
 */
void safety_timeline_record_locked(struct safety_copro_dev *dev,
				   u32 event, u16 seq)
{
	struct safety_timeline_entry *e = &dev->timeline[dev->timeline_head];

	e->ts_ns = ktime_get_ns();
	e->event = event;
	e->seq   = seq;

	dev->timeline_head = (dev->timeline_head + 1) % SAFETY_TIMELINE_DEPTH;
	if (dev->timeline_filled < SAFETY_TIMELINE_DEPTH)
		dev->timeline_filled++;
}

/*
 * safety_fifo_push_frame - 將完整 frame 原始位元組推入 RX kfifo。
 * 在「未持有 lock」時呼叫；內部取得 lock。
 *
 * 回傳：
 *   0       成功入列 (已更新 high-watermark、timeline、喚醒 poll/read)
 *   -EMSGSIZE  frame 長度非法 (> 最大 frame)
 *   -ENOSPC    kfifo 已滿 (呼叫端負責累計 dropped 並 trace)
 */
int safety_fifo_push_frame(struct safety_copro_dev *dev,
			   const u8 *frame, u16 len,
			   u16 seq, u8 type, u32 timeline_event)
{
	struct safety_fifo_slot slot;
	unsigned long flags;
	unsigned int qlen;
	int ret;

	if (len == 0 || len > SAFETY_MAX_FRAME_SIZE)
		return -EMSGSIZE;

	/* 先在 stack 上組好 slot，縮短持鎖時間 */
	slot.len = len;
	memcpy(slot.data, frame, len);

	spin_lock_irqsave(&dev->lock, flags);

	if (kfifo_is_full(&dev->rx_fifo)) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return -ENOSPC;
	}

	/* kfifo_in 對「物件型 kfifo」一次放入 1 個元素 */
	ret = kfifo_in(&dev->rx_fifo, &slot, 1);
	if (ret != 1) {
		/* 理論上前面已檢查 is_full，這裡是雙保險 */
		spin_unlock_irqrestore(&dev->lock, flags);
		return -ENOSPC;
	}

	qlen = kfifo_len(&dev->rx_fifo);
	if (qlen > dev->fifo_high_watermark)
		dev->fifo_high_watermark = qlen;

	safety_timeline_record_locked(dev, timeline_event, seq);

	spin_unlock_irqrestore(&dev->lock, flags);

	/* per-CPU 統計與 tracepoint 在 lock 外進行 */
	safety_percpu_inc_rx(dev, len);
	trace_safety_frame_received(seq, type, qlen);

	/* 喚醒 read()/poll() 等待者 */
	wake_up_interruptible(&dev->rx_wq);

	return 0;
}

/*
 * safety_fifo_pop_frame - 取出一個 frame 到呼叫端提供的 slot。
 * 自行取得 lock。回傳取出的位元組長度；kfifo 空時回傳 0。
 */
u16 safety_fifo_pop_frame(struct safety_copro_dev *dev,
			  struct safety_fifo_slot *slot)
{
	unsigned long flags;
	u16 len = 0;

	spin_lock_irqsave(&dev->lock, flags);
	if (!kfifo_is_empty(&dev->rx_fifo)) {
		if (kfifo_out(&dev->rx_fifo, slot, 1) == 1)
			len = slot->len;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	return len;
}

/* safety_fifo_len - 目前 kfifo 內 frame 數量。 */
unsigned int safety_fifo_len(struct safety_copro_dev *dev)
{
	unsigned long flags;
	unsigned int n;

	spin_lock_irqsave(&dev->lock, flags);
	n = kfifo_len(&dev->rx_fifo);
	spin_unlock_irqrestore(&dev->lock, flags);

	return n;
}

/* safety_fifo_push_tx_frame - 將 supervisor 寫入的 command frame 放入 TX queue。 */
int safety_fifo_push_tx_frame(struct safety_copro_dev *dev,
			      const u8 *frame, u16 len,
			      u16 seq, u8 type)
{
	struct safety_fifo_slot slot;
	unsigned long flags;
	int ret;

	if (len == 0 || len > SAFETY_MAX_FRAME_SIZE)
		return -EMSGSIZE;

	slot.len = len;
	memcpy(slot.data, frame, len);

	spin_lock_irqsave(&dev->lock, flags);
	if (kfifo_is_full(&dev->tx_fifo)) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return -ENOSPC;
	}
	ret = kfifo_in(&dev->tx_fifo, &slot, 1);
	if (ret != 1) {
		spin_unlock_irqrestore(&dev->lock, flags);
		return -ENOSPC;
	}
	safety_timeline_record_locked(dev, SAFETY_TL_RECOVERY, seq);
	spin_unlock_irqrestore(&dev->lock, flags);

	trace_safety_frame_received(seq, type, safety_tx_fifo_len(dev));
	wake_up_interruptible(&dev->tx_wq);
	return 0;
}

/* safety_fifo_pop_tx_frame - linkd 取出一個待送 UART 的 TX frame。 */
u16 safety_fifo_pop_tx_frame(struct safety_copro_dev *dev,
			     struct safety_fifo_slot *slot)
{
	unsigned long flags;
	u16 len = 0;

	spin_lock_irqsave(&dev->lock, flags);
	if (!kfifo_is_empty(&dev->tx_fifo)) {
		if (kfifo_out(&dev->tx_fifo, slot, 1) == 1)
			len = slot->len;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	return len;
}

/* safety_tx_fifo_len - 目前 TX command queue 內 frame 數量。 */
unsigned int safety_tx_fifo_len(struct safety_copro_dev *dev)
{
	unsigned long flags;
	unsigned int n;

	spin_lock_irqsave(&dev->lock, flags);
	n = kfifo_len(&dev->tx_fifo);
	spin_unlock_irqrestore(&dev->lock, flags);

	return n;
}

/*
 * 共用：將 header + payload 打包成完整 frame 並推入 kfifo。
 * seq 由 dev->synth_seq 提供 (需在 lock 外讀取，但合成 frame 不要求嚴格序號，
 * 此處以 spinlock 短暫保護遞增即可)。timeline_event 指定記錄類型。
 */
static int safety_synth_and_push(struct safety_copro_dev *dev,
				 u8 type, const void *payload, u16 payload_len,
				 u32 timeline_event)
{
	struct SafetyFrameHeader hdr;
	u8 frame[SAFETY_MAX_FRAME_SIZE];
	unsigned long flags;
	size_t total;
	u16 seq;
	int rc;

	if (payload_len > SAFETY_MAX_PAYLOAD)
		return -EMSGSIZE;

	/* 取一個合成序號 (短暫持鎖) */
	spin_lock_irqsave(&dev->lock, flags);
	seq = dev->synth_seq++;
	spin_unlock_irqrestore(&dev->lock, flags);

	safety_frame_init(&hdr, type, seq, payload_len, safety_now_ms());
	total = safety_frame_pack(frame, &hdr, payload);
	if (total == 0)
		return -EMSGSIZE;

	rc = safety_fifo_push_frame(dev, frame, (u16)total, seq, type,
				    timeline_event);
	if (rc == -ENOSPC) {
		/* 合成 frame 也可能因滿而丟棄；計入 dropped 並 trace */
		unsigned long f;

		spin_lock_irqsave(&dev->lock, f);
		dev->dropped_frame_count++;
		spin_unlock_irqrestore(&dev->lock, f);
		safety_percpu_inc_dropped(dev);
		trace_safety_frame_dropped(seq, type);
	}
	return rc;
}

/*
 * safety_synth_fault_event - 合成 FAULT_EVENT frame 並推入 kfifo。
 * 由 timeout watchdog 與 INJECT_FAULT ioctl 共用。
 */
int safety_synth_fault_event(struct safety_copro_dev *dev,
			     u8 fault_type, u8 severity, u16 detail_code)
{
	struct SafetyFaultEventPayload pl;

	memset(&pl, 0, sizeof(pl));
	pl.fault_type      = fault_type;
	pl.severity        = severity;
	pl.detail_code     = detail_code;
	pl.fault_uptime_ms = safety_now_ms();

	return safety_synth_and_push(dev, SAFETY_FRAME_FAULT_EVENT,
				     &pl, sizeof(pl), SAFETY_TL_FAULT_INJECT);
}

/*
 * safety_synth_recovery_report - 合成 RECOVERY_REPORT frame 並推入 kfifo。
 * 由 recovery workqueue 呼叫。
 */
int safety_synth_recovery_report(struct safety_copro_dev *dev,
				 u8 recovered, u8 new_state)
{
	struct SafetyRecoveryReportPayload pl;

	memset(&pl, 0, sizeof(pl));
	pl.recovered          = recovered;
	pl.new_state          = new_state;
	pl.recovery_uptime_ms = safety_now_ms();

	return safety_synth_and_push(dev, SAFETY_FRAME_RECOVERY_REPORT,
				     &pl, sizeof(pl), SAFETY_TL_RECOVERY);
}
