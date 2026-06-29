// SPDX-License-Identifier: GPL-2.0
/*
 * safety_copro_debugfs.c - /sys/kernel/debug/safety_copro/ 偵錯介面
 *
 * 角色：建立 debugfs 目錄與多個唯讀檔，使用 seq_file 將 driver 內部狀態以
 * 人類可讀格式輸出，方便 QEMU 內手動觀察與 CI 比對：
 *   stats           全域計數器 (state 以文字表示)
 *   percpu_stats    per-CPU rx/dropped/bytes 明細
 *   ringbuf_status  kfifo len / capacity / high-watermark
 *   last_fault      最近一次 fault 型別 / 時間 / detail
 *   protocol_error  最近一次 protocol error 與分類計數
 *   timeline        近期事件 ring (由舊到新)
 *
 * 鎖規則：seq_file show 在 process context；讀取共享狀態時持 dev->lock，但
 * seq_printf 不在持鎖時呼叫 (先複製到區域變數再輸出)，避免在可能配置記憶體
 * 的路徑上持 spinlock。timeline 因資料量較大，採「持鎖複製整個 ring 到 stack」
 * 後釋鎖再列印。
 */

#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/kfifo.h>

#include "safety_copro.h"

/* timeline 事件名稱對照 (debugfs 顯示用)。 */
static const char *tl_event_name(u32 ev)
{
	switch (ev) {
	case SAFETY_TL_FRAME_RX:     return "FRAME_RX";
	case SAFETY_TL_FRAME_DROP:   return "FRAME_DROP";
	case SAFETY_TL_HEARTBEAT:    return "HEARTBEAT";
	case SAFETY_TL_TIMEOUT:      return "TIMEOUT";
	case SAFETY_TL_RECOVERY:     return "RECOVERY";
	case SAFETY_TL_FAULT_INJECT: return "FAULT_INJECT";
	default:                     return "NONE";
	}
}

/* fault 型別名稱對照。 */
static const char *fault_type_name(u8 ft)
{
	switch (ft) {
	case SAFETY_FAULT_NONE:           return "NONE";
	case SAFETY_FAULT_TASK_HANG:      return "HEARTBEAT_STOP";
	case SAFETY_FAULT_CHECKSUM_ERROR: return "CHECKSUM_ERROR";
	case SAFETY_FAULT_CRITICAL:       return "CRITICAL";
	default:                          return "UNKNOWN";
	}
}

/* validate 結果名稱對照。 */
static const char *validate_name(int v)
{
	switch (v) {
	case SAFETY_VALIDATE_OK:           return "OK";
	case SAFETY_VALIDATE_BAD_MAGIC:    return "BAD_MAGIC";
	case SAFETY_VALIDATE_BAD_VERSION:  return "BAD_VERSION";
	case SAFETY_VALIDATE_BAD_LENGTH:   return "BAD_LENGTH";
	case SAFETY_VALIDATE_BAD_CHECKSUM: return "BAD_CHECKSUM";
	default:                           return "NONE";
	}
}

/* ---- stats ---- */
static int stats_show(struct seq_file *s, void *unused)
{
	struct safety_copro_dev *dev = s->private;
	unsigned long flags;
	u32 state, hb, fc, drop, to, retry, perr;
	unsigned int rx_depth, tx_depth;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->state;
	hb    = dev->heartbeat_count;
	fc    = dev->fault_count;
	drop  = dev->dropped_frame_count;
	to    = dev->timeout_count;
	retry = dev->retry_count;
	perr  = dev->protocol_error_count;
	spin_unlock_irqrestore(&dev->lock, flags);
	rx_depth = safety_fifo_len(dev);
	tx_depth = safety_tx_fifo_len(dev);

	seq_printf(s, "current_state        %s\n",
		   safety_link_state_name(state));
	seq_printf(s, "heartbeat_count      %u\n", hb);
	seq_printf(s, "fault_count          %u\n", fc);
	seq_printf(s, "dropped_frame_count  %u\n", drop);
	seq_printf(s, "timeout_count        %u\n", to);
	seq_printf(s, "retry_count          %u\n", retry);
	seq_printf(s, "protocol_error_count %u\n", perr);
	seq_printf(s, "rx_queue_depth       %u\n", rx_depth);
	seq_printf(s, "tx_queue_depth       %u\n", tx_depth);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

/* ---- percpu_stats ---- */
static int percpu_stats_show(struct seq_file *s, void *unused)
{
	struct safety_copro_dev *dev = s->private;
	struct safety_copro_pcpu_sum sum;
	int cpu;

	seq_printf(s, "%-6s %12s %12s %14s\n",
		   "cpu", "rx_frames", "dropped", "bytes");
	for_each_possible_cpu(cpu) {
		struct safety_copro_pcpu *p = per_cpu_ptr(dev->pcpu, cpu);

		seq_printf(s, "%-6d %12llu %12llu %14llu\n",
			   cpu,
			   (unsigned long long)p->rx_frames,
			   (unsigned long long)p->dropped,
			   (unsigned long long)p->bytes);
	}

	safety_percpu_sum(dev, &sum);
	seq_printf(s, "%-6s %12llu %12llu %14llu\n",
		   "total",
		   (unsigned long long)sum.rx_frames,
		   (unsigned long long)sum.dropped,
		   (unsigned long long)sum.bytes);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(percpu_stats);

/* ---- ringbuf_status ---- */
static int ringbuf_status_show(struct seq_file *s, void *unused)
{
	struct safety_copro_dev *dev = s->private;
	unsigned long flags;
	unsigned int rx_len, tx_len, cap, hwm;

	spin_lock_irqsave(&dev->lock, flags);
	rx_len = kfifo_len(&dev->rx_fifo);
	tx_len = kfifo_len(&dev->tx_fifo);
	cap = kfifo_size(&dev->rx_fifo);
	hwm = dev->fifo_high_watermark;
	spin_unlock_irqrestore(&dev->lock, flags);

	seq_printf(s, "rx_len          %u\n", rx_len);
	seq_printf(s, "tx_len          %u\n", tx_len);
	seq_printf(s, "capacity        %u\n", cap);
	seq_printf(s, "high_watermark  %u\n", hwm);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ringbuf_status);

/* ---- last_fault ---- */
static int last_fault_show(struct seq_file *s, void *unused)
{
	struct safety_copro_dev *dev = s->private;
	unsigned long flags;
	u8  ft;
	u16 detail;
	u64 ts;

	spin_lock_irqsave(&dev->lock, flags);
	ft     = dev->last_fault_type;
	detail = dev->last_fault_detail;
	ts     = dev->last_fault_ts_ns;
	spin_unlock_irqrestore(&dev->lock, flags);

	seq_printf(s, "last_fault_type   %u (%s)\n", ft, fault_type_name(ft));
	seq_printf(s, "detail_code       %u\n", detail);
	seq_printf(s, "timestamp_ns      %llu\n", (unsigned long long)ts);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(last_fault);

/* ---- protocol_error ---- */
static int protocol_error_show(struct seq_file *s, void *unused)
{
	struct safety_copro_dev *dev = s->private;
	unsigned long flags;
	int last;
	u32 total, m, v, l, c;

	spin_lock_irqsave(&dev->lock, flags);
	last  = dev->last_proto_err;
	total = dev->protocol_error_count;
	m     = dev->proto_err_magic;
	v     = dev->proto_err_version;
	l     = dev->proto_err_length;
	c     = dev->proto_err_checksum;
	spin_unlock_irqrestore(&dev->lock, flags);

	seq_printf(s, "last_error        %s\n", validate_name(last));
	seq_printf(s, "total             %u\n", total);
	seq_printf(s, "bad_magic         %u\n", m);
	seq_printf(s, "bad_version       %u\n", v);
	seq_printf(s, "bad_length        %u\n", l);
	seq_printf(s, "bad_checksum      %u\n", c);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(protocol_error);

/* ---- timeline ---- */
static int timeline_show(struct seq_file *s, void *unused)
{
	struct safety_copro_dev *dev = s->private;
	struct safety_timeline_entry snap[SAFETY_TIMELINE_DEPTH];
	unsigned long flags;
	u32 head, filled, i, idx, start;

	/* 持鎖複製整個 ring 到 stack，釋鎖後再列印 (避免持鎖 seq_printf) */
	spin_lock_irqsave(&dev->lock, flags);
	head   = dev->timeline_head;
	filled = dev->timeline_filled;
	memcpy(snap, dev->timeline, sizeof(snap));
	spin_unlock_irqrestore(&dev->lock, flags);

	seq_printf(s, "%-18s %-14s %-6s\n", "ts_ns", "event", "seq");

	if (filled == 0)
		return 0;

	/*
	 * 由舊到新輸出 (newest last)。當 filled < DEPTH 時，最舊在索引 0；
	 * 當 ring 已滿 (filled == DEPTH) 時，最舊在 head 位置。
	 */
	if (filled < SAFETY_TIMELINE_DEPTH)
		start = 0;
	else
		start = head;

	for (i = 0; i < filled; i++) {
		idx = (start + i) % SAFETY_TIMELINE_DEPTH;
		seq_printf(s, "%-18llu %-14s %-6u\n",
			   (unsigned long long)snap[idx].ts_ns,
			   tl_event_name(snap[idx].event),
			   snap[idx].seq);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(timeline);

/* safety_debugfs_init - 建立目錄與所有唯讀檔。 */
int safety_debugfs_init(struct safety_copro_dev *dev)
{
	struct dentry *d;

	d = debugfs_create_dir("safety_copro", NULL);
	/*
	 * debugfs_create_dir 失敗會回傳 ERR_PTR；若 DEBUG_FS 未編入則回傳
	 * 特殊錯誤值。我們不把 debugfs 視為致命：失敗時僅記錄並繼續，driver
	 * 主要功能不依賴 debugfs。
	 */
	if (IS_ERR(d)) {
		pr_warn("safety_copro: debugfs dir create failed (%ld)\n",
			PTR_ERR(d));
		dev->debugfs_dir = NULL;
		return 0;
	}
	dev->debugfs_dir = d;

	debugfs_create_file("stats", 0444, d, dev, &stats_fops);
	debugfs_create_file("percpu_stats", 0444, d, dev, &percpu_stats_fops);
	debugfs_create_file("ringbuf_status", 0444, d, dev,
			    &ringbuf_status_fops);
	debugfs_create_file("last_fault", 0444, d, dev, &last_fault_fops);
	debugfs_create_file("protocol_error", 0444, d, dev,
			    &protocol_error_fops);
	debugfs_create_file("timeline", 0444, d, dev, &timeline_fops);

	return 0;
}

/* safety_debugfs_destroy - 遞迴移除整個 debugfs 目錄。 */
void safety_debugfs_destroy(struct safety_copro_dev *dev)
{
	if (dev->debugfs_dir) {
		debugfs_remove_recursive(dev->debugfs_dir);
		dev->debugfs_dir = NULL;
	}
}
