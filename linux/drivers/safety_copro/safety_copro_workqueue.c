// SPDX-License-Identifier: GPL-2.0
/*
 * safety_copro_workqueue.c - 延遲 recovery 處理 (workqueue)
 *
 * 角色：watchdog 逾時或 FORCE_RECOVERY ioctl 會 schedule 一個 work；該 work
 * 在 process context (system workqueue) 執行，負責：
 *   - retry_count++ (recovery 嘗試計數)、link state 轉為 RECOVERING
 *   - 寫入 timeline、trace_safety_recovery_queued
 *   - 合成一個 RECOVERY_REPORT frame 推入 kfifo (recovered=1)
 *
 * 設計：work handler 不阻塞 (no msleep / no I/O)，僅記錄狀態並合成 frame。
 * 使用核心 system_wq (schedule_work)，毋須自建 workqueue。
 *
 * 競態說明：trace_safety_recovery_queued 命名上是「queued」，但事件本身代表
 * 「一次 recovery 被處理」，故在 handler 內計數並 trace，語意為「已實際排程並
 * 進入處理」。
 */

#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>

#include "safety_copro.h"
#include "safety_copro_trace.h"

/* recovery_work_fn - process context，可睡眠 (此處刻意不睡眠)。 */
static void recovery_work_fn(struct work_struct *w)
{
	struct safety_copro_dev *dev =
		container_of(w, struct safety_copro_dev, recovery_work);
	unsigned long flags;
	u32 count;

	spin_lock_irqsave(&dev->lock, flags);
	dev->retry_count++;
	count = dev->retry_count;
	dev->state = SAFETY_LINK_RECOVERING;
	safety_timeline_record_locked(dev, SAFETY_TL_RECOVERY, 0);
	spin_unlock_irqrestore(&dev->lock, flags);

	trace_safety_recovery_queued(count);

	/* 合成 RECOVERY_REPORT，讓 userspace 知道 kernel 已嘗試恢復 */
	safety_synth_recovery_report(dev, 1 /* recovered */,
				     SAFETY_LINK_RECOVERING);
}

/* safety_recovery_init - 初始化 work_struct。 */
void safety_recovery_init(struct safety_copro_dev *dev)
{
	INIT_WORK(&dev->recovery_work, recovery_work_fn);
}

/* safety_recovery_destroy - teardown：等待任何在途 work 完成。 */
void safety_recovery_destroy(struct safety_copro_dev *dev)
{
	cancel_work_sync(&dev->recovery_work);
}

/*
 * safety_recovery_schedule - 排入 recovery work。
 * schedule_work 為非睡眠操作，可在 hrtimer atomic context 與 ioctl process
 * context 安全呼叫；若同一 work 已在 queue 中，重複呼叫會被忽略 (回傳 false)，
 * 屬預期行為。
 */
void safety_recovery_schedule(struct safety_copro_dev *dev)
{
	schedule_work(&dev->recovery_work);
}
