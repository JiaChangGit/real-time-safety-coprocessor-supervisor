/* SPDX-License-Identifier: GPL-2.0 */
/*
 * safety_copro_trace.h - Safety Co-Processor Supervisor 的 tracepoint 定義
 *
 * 角色：使用 TRACE_EVENT 宣告 5 個 tracepoint，TRACE_SYSTEM 為 safety_copro。
 * 這些 tracepoint 是與 userspace eBPF agent 的「契約」，名稱與欄位簽章必須維持
 * 穩定 (eBPF CO-RE 依賴它們)。本檔遵循 in-tree driver 的標準寫法：使用相對
 * TRACE_INCLUDE_PATH 指回 driver 目錄，並在「唯一」一個 .c (safety_copro_main.c)
 * 中 #define CREATE_TRACE_POINTS 後再 include 本檔，以實際生成 tracepoint 代碼。
 *
 * 注意：所有字串/名稱皆為英文；僅註解使用台灣繁體中文。
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM safety_copro

#if !defined(_SAFETY_COPRO_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _SAFETY_COPRO_TRACE_H

#include <linux/tracepoint.h>

/*
 * safety_frame_received - 一個有效 frame 成功進入 RX kfifo。
 * @seq:  frame 的 sequence_id
 * @type: frame 型別 (enum safety_frame_type)
 * @qlen: 進入後 kfifo 內目前的 frame 數量
 */
TRACE_EVENT(safety_frame_received,

	TP_PROTO(u16 seq, u8 type, u32 qlen),

	TP_ARGS(seq, type, qlen),

	TP_STRUCT__entry(
		__field(u16, seq)
		__field(u8,  type)
		__field(u32, qlen)
	),

	TP_fast_assign(
		__entry->seq  = seq;
		__entry->type = type;
		__entry->qlen = qlen;
	),

	TP_printk("seq=%u type=%u qlen=%u",
		  __entry->seq, __entry->type, __entry->qlen)
);

/*
 * safety_frame_dropped - 因 kfifo 已滿，丟棄一個有效 frame。
 * @seq:  被丟棄 frame 的 sequence_id
 * @type: frame 型別
 */
TRACE_EVENT(safety_frame_dropped,

	TP_PROTO(u16 seq, u8 type),

	TP_ARGS(seq, type),

	TP_STRUCT__entry(
		__field(u16, seq)
		__field(u8,  type)
	),

	TP_fast_assign(
		__entry->seq  = seq;
		__entry->type = type;
	),

	TP_printk("seq=%u type=%u", __entry->seq, __entry->type)
);

/*
 * safety_timeout_detected - heartbeat watchdog hrtimer 逾時觸發。
 * @timeout_ms: 設定的逾時門檻 (ms)
 * @count:      累計逾時次數
 */
TRACE_EVENT(safety_timeout_detected,

	TP_PROTO(u32 timeout_ms, u32 count),

	TP_ARGS(timeout_ms, count),

	TP_STRUCT__entry(
		__field(u32, timeout_ms)
		__field(u32, count)
	),

	TP_fast_assign(
		__entry->timeout_ms = timeout_ms;
		__entry->count      = count;
	),

	TP_printk("timeout_ms=%u count=%u",
		  __entry->timeout_ms, __entry->count)
);

/*
 * safety_recovery_queued - recovery work 被排入 workqueue。
 * @count: 累計 recovery 觸發次數
 */
TRACE_EVENT(safety_recovery_queued,

	TP_PROTO(u32 count),

	TP_ARGS(count),

	TP_STRUCT__entry(
		__field(u32, count)
	),

	TP_fast_assign(
		__entry->count = count;
	),

	TP_printk("count=%u", __entry->count)
);

/*
 * safety_poll_wakeup - poll 等待者因 RX 有資料而被喚醒。
 * @events: 回報給 poll 的事件遮罩 (POLLIN 等)
 */
TRACE_EVENT(safety_poll_wakeup,

	TP_PROTO(u32 events),

	TP_ARGS(events),

	TP_STRUCT__entry(
		__field(u32, events)
	),

	TP_fast_assign(
		__entry->events = events;
	),

	TP_printk("events=0x%x", __entry->events)
);

#endif /* _SAFETY_COPRO_TRACE_H */

/*
 * in-tree driver 目錄的 tracepoint include 規則。預設 <trace/define_trace.h>
 * 會到 include/trace/events/ 尋找本檔，因此必須覆寫搜尋路徑指回本 driver
 * 目錄。本專案在 Makefile 對 safety_copro_main.o 加了 -I$(src)，因此這裡用
 * 「目前目錄 (.)」即可被正確找到 (此為 samples/trace_events 採用的標準寫法，
 * 比硬編 ../../drivers/... 相對路徑更穩健)。
 */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE safety_copro_trace

#include <trace/define_trace.h>
