// SPDX-License-Identifier: GPL-2.0
/*
 * safety_copro_timer.c - heartbeat watchdog (hrtimer)
 *
 * 角色：維護一個 monotonic hrtimer 作為 co-processor 心跳看門狗。每收到一個
 * HEARTBEAT frame 就 rearm；若在 hb_timeout_ms 內未再收到心跳，hrtimer 觸發：
 *   - timeout_count++、link state 轉為 HB_TIMEOUT
 *   - 記錄 last_fault、寫入 timeline
 *   - 觸發 trace_safety_timeout_detected
 *   - 合成一個 FAULT_EVENT(HEARTBEAT_STOP, detail=1) frame 推入 kfifo
 *   - 排入 recovery workqueue
 *
 * 鎖規則：hrtimer callback 在「軟中斷/atomic context」執行，期間只用
 * spin_lock 更新共享計數，且不睡眠。合成 frame 與 schedule_work 皆為非睡眠
 * 操作，可在 callback 中安全呼叫 (kfifo_in/wake_up_interruptible/schedule_work
 * 均不睡眠)。callback 回傳 HRTIMER_NORESTART — 由「下一次心跳」負責重新 arm，
 * 避免在已逾時狀態下持續刷洗。
 *
 * 6.12 API 註記：使用 hrtimer_init() + 指定 .function (6.12 LTS 的標準寫法)。
 */

#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>

#include "safety_copro.h"
#include "safety_copro_trace.h"

/*
 * hb_timer_fn - watchdog hrtimer 回呼。
 * context：HRTIMER_MODE_REL soft/hard IRQ context，不可睡眠。
 */
static enum hrtimer_restart hb_timer_fn(struct hrtimer *t)
{
	struct safety_copro_dev *dev =
		container_of(t, struct safety_copro_dev, hb_timer);
	unsigned long flags;
	u32 timeout_ms, count;

	spin_lock_irqsave(&dev->lock, flags);
	dev->timeout_count++;
	count = dev->timeout_count;
	timeout_ms = dev->hb_timeout_ms;
	dev->state = SAFETY_LINK_HB_TIMEOUT;

	/* 記錄 last_fault：heartbeat stop */
	dev->last_fault_type   = SAFETY_FAULT_TASK_HANG;
	dev->last_fault_detail = 1;
	dev->last_fault_ts_ns  = ktime_get_ns();
	dev->fault_count++;

	safety_timeline_record_locked(dev, SAFETY_TL_TIMEOUT, 0);
	spin_unlock_irqrestore(&dev->lock, flags);

	trace_safety_timeout_detected(timeout_ms, count);

	/*
	 * 合成 FAULT_EVENT frame 讓 userspace 觀察到 kernel 偵測到的逾時。
	 * safety_synth_fault_event 內部使用 spinlock + kfifo + wake_up，皆非
	 * 睡眠操作，可於 hrtimer atomic context 安全呼叫。
	 */
	safety_synth_fault_event(dev, SAFETY_FAULT_TASK_HANG,
				 1 /* warn */, 1 /* detail_code */);

	/* 觸發延遲 recovery */
	safety_recovery_schedule(dev);

	/* 不自動重啟；待下一個 HEARTBEAT 再 rearm */
	return HRTIMER_NORESTART;
}

/* safety_timer_init - 初始化 hrtimer (monotonic, relative mode)。 */
void safety_timer_init(struct safety_copro_dev *dev)
{
	hrtimer_init(&dev->hb_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev->hb_timer.function = hb_timer_fn;
}

/* safety_timer_destroy - 取消並等待 hrtimer 結束 (teardown 用)。 */
void safety_timer_destroy(struct safety_copro_dev *dev)
{
	/*
	 * hrtimer_cancel 會等待正在執行的 callback 完成；不可在 callback 自身
	 * 或持有 callback 會搶的 lock 時呼叫。teardown 在 process context、未持
	 * dev->lock，故安全。
	 */
	hrtimer_cancel(&dev->hb_timer);
}

/*
 * safety_timer_rearm - 收到 HEARTBEAT 後重新武裝看門狗。
 * hrtimer_start 在 timer 已 active 時等同重新設定到期時間，可安全重複呼叫。
 */
void safety_timer_rearm(struct safety_copro_dev *dev)
{
	u32 ms;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	ms = dev->hb_timeout_ms;
	spin_unlock_irqrestore(&dev->lock, flags);

	hrtimer_start(&dev->hb_timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
}
