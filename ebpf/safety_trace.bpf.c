// SPDX-License-Identifier: GPL-2.0
/*
 * safety_trace.bpf.c - Real-Time Safety Co-Processor Supervisor 的 eBPF CO-RE
 * tracing 程式。
 *
 * 角色：以 libbpf CO-RE (Compile-Once Run-Everywhere) 方式，於 QEMU ARM64 Linux
 * guest 內 attach 到核心 tracepoint，量測 supervisor 的即時性能：
 *   1) read/write/ioctl syscall latency（log2 直方圖 + min/max/sum/count）
 *   2) poll wakeup latency（frame_received -> poll_wakeup 的延遲）
 *   3) scheduler delay（sched_wakeup -> sched_switch-in 的排程等待）
 *   4) 5 個 safety_copro 自訂 tracepoint 的計數
 *   5) fault-to-recovery timeline（timeout/recovery/heartbeat_restored 事件流）
 *
 * 慣例：註解使用台灣繁體中文，技術名詞保留英文；所有對 userspace 輸出的字串為英文。
 *
 * tracepoint ctx struct 的「common 前綴」+ 各 TP 欄位「依宣告順序」排列。實際 byte
 * offset 取決於目標 kernel，CO-RE relocation 由 libbpf 依 BTF 自動修正；可用
 *   cat /sys/kernel/tracing/events/<sys>/<name>/format
 * 檢視確切 offset（見 README）。
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

/* ===================================================================
 *  共用常數與型別（與 userspace 之 safety_trace_user.c 保持一致）
 * =================================================================== */

/* supervisor 進程的 comm 前綴。daemon 二進位為 "safety-supervisord"，但 kernel
 * task->comm 長度上限為 TASK_COMM_LEN(16)，可用字元 15 個，會被截斷為
 * "safety-supervis"。我們以 14 字元前綴 "safety-supervi" 比對，屬於安全前綴。
 * 註：comm 前綴比對是「近似 scoping」——任何 comm 以此前綴開頭的 process 都會被
 * 計入。本專案不存在其他同前綴 process，故為足夠精準的近似（見 README caveat）。 */
#define COMM_PREFIX      "safety-supervi"
#define COMM_PREFIX_LEN  14

/* log2 直方圖 bucket 數：slot = log2(delta_ns)，0..63（delta=0 落在 slot 0）。 */
#define MAX_SLOTS 64

/* syscall 種類索引，作為 per-syscall 直方圖 percpu-array 的外層 key。 */
enum syscall_kind {
	SC_READ  = 0,
	SC_WRITE = 1,
	SC_IOCTL = 2,
	SC_MAX   = 3,
};

/* 單一 metric 的彙總統計（除 hist 外，min/max/sum/count 用於精確 avg 與邊界）。 */
struct hist {
	__u64 slots[MAX_SLOTS];
	__u64 count;
	__u64 sum;   /* 累計 delta_ns 總和，用於精確 average */
	__u64 min;   /* 觀測到的最小 delta_ns（初始 0 代表尚未設定，見 update 邏輯） */
	__u64 max;   /* 觀測到的最大 delta_ns */
};

/* timeline ringbuf 事件種類。 */
enum timeline_kind {
	TL_TIMEOUT            = 0,  /* safety_timeout_detected */
	TL_RECOVERY           = 1,  /* safety_recovery_queued */
	TL_HEARTBEAT_RESTORED = 2,  /* recovery 後第一個 safety_frame_received */
};

/* 推到 ringbuf 的 timeline event；ts 為 bpf_ktime_get_ns()（CLOCK_MONOTONIC）。 */
struct timeline_event {
	__u64 ts;
	__u32 kind;   /* enum timeline_kind */
	__u32 aux;    /* TIMEOUT: count；RECOVERY: count；HEARTBEAT_RESTORED: seq */
};

/* 自訂 tracepoint 計數索引（counters percpu-array 的 key）。 */
enum tp_counter_idx {
	CNT_FRAME_RECEIVED   = 0,
	CNT_FRAME_DROPPED    = 1,
	CNT_TIMEOUT_DETECTED = 2,
	CNT_RECOVERY_QUEUED  = 3,
	CNT_POLL_WAKEUP      = 4,
	CNT_MAX              = 5,
};

/* ===================================================================
 *  Maps
 * =================================================================== */

/* syscall 進入時間戳，key = tid（pid_t），value = enter ts(ns)。 */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, __u32);
	__type(value, __u64);
} sys_start SEC(".maps");

/* sched_wakeup 時間戳，key = pid（被喚醒 task 的 pid），value = wakeup ts(ns)。 */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, __u32);
	__type(value, __u64);
} wakeup_ts SEC(".maps");

/* per-syscall latency 直方圖，外層 array index = enum syscall_kind。 */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, SC_MAX);
	__type(key, __u32);
	__type(value, struct hist);
} syscall_lat SEC(".maps");

/* poll wakeup latency 直方圖（單一 metric，index 0）。 */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct hist);
} poll_lat SEC(".maps");

/* scheduler delay 直方圖（單一 metric，index 0）。 */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct hist);
} sched_lat SEC(".maps");

/* 5 個 safety_copro tracepoint 的計數，index = enum tp_counter_idx。 */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, CNT_MAX);
	__type(key, __u32);
	__type(value, __u64);
} tp_counters SEC(".maps");

/* poll wakeup latency 用：最近一次 safety_frame_received 的 ts。index 0。
 * 注意：刻意使用「全域」ARRAY（非 PERCPU）——driver 中 safety_frame_received 在
 * RX/timer 路徑觸發、safety_poll_wakeup 在 supervisor 的 .poll process context
 * 觸發，兩者可能落在不同 CPU。若用 PERCPU 會在 poll CPU 上讀不到 frame ts。改用
 * 全域 slot 跨 CPU 共享；高頻並發下的寫入競態對「延遲估計」可接受。 */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} last_frame_ts SEC(".maps");

/* recovery 進行中旗標（單一 array slot）。1 = 已 queue recovery、尚未看到 restored
 * heartbeat；用於辨識「recovery 後第一個 frame_received」= HEARTBEAT_RESTORED。 */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} recovery_pending SEC(".maps");

/* timeline 事件 ringbuf（256 KiB）。 */
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} timeline_rb SEC(".maps");

/* ===================================================================
 *  Helpers
 * =================================================================== */

/* log2 floor：回傳 v 的最高有效位 index（v=0 -> 0、v=1 -> 0、v=2 -> 1 ...）。
 * 用 31..0 / 後段 unrolled 比較取代迴圈，確保 BPF verifier 友善且 O(1)。 */
static __always_inline __u32 log2_u64(__u64 v)
{
	__u32 r = 0;
	if (v >= (1ULL << 32)) { v >>= 32; r += 32; }
	if (v >= (1ULL << 16)) { v >>= 16; r += 16; }
	if (v >= (1ULL << 8))  { v >>= 8;  r += 8; }
	if (v >= (1ULL << 4))  { v >>= 4;  r += 4; }
	if (v >= (1ULL << 2))  { v >>= 2;  r += 2; }
	if (v >= (1ULL << 1))  {           r += 1; }
	return r;
}

/* 把單一 delta_ns 觀測值累加進 hist（slot + count/sum/min/max）。 */
static __always_inline void hist_record(struct hist *h, __u64 delta)
{
	__u32 slot = log2_u64(delta);
	if (slot >= MAX_SLOTS)
		slot = MAX_SLOTS - 1;
	h->slots[slot]++;
	h->count++;
	h->sum += delta;
	/* min 初值為 0 表「尚未設定」；count==1（本次為第一筆）時直接寫入。 */
	if (h->count == 1 || delta < h->min)
		h->min = delta;
	if (delta > h->max)
		h->max = delta;
}

/* comm 前綴比對：回傳 1 表示目前 task 的 comm 以 COMM_PREFIX 開頭。 */
static __always_inline int comm_is_supervisor(void)
{
	char comm[16];
	if (bpf_get_current_comm(&comm, sizeof(comm)))
		return 0;
#pragma unroll
	for (int i = 0; i < COMM_PREFIX_LEN; i++) {
		if (comm[i] != COMM_PREFIX[i])
			return 0;
	}
	return 1;
}

/* ===================================================================
 *  1) read/write/ioctl syscall latency（raw syscall tracepoints）
 *
 *  使用 SEC("tp/syscalls/sys_enter_*") / sys_exit_*。我們在 enter 記錄
 *  start ts（以 tid 為 key），exit 算 delta；scoping 以 comm 前綴近似。
 *  ctx 採 BTF tracepoint 形式（trace_event_raw_sys_enter / _sys_exit），
 *  CO-RE 自動修正欄位 offset。
 * =================================================================== */

static __always_inline void sys_enter_record(void)
{
	if (!comm_is_supervisor())
		return;
	__u64 tid = (__u32)bpf_get_current_pid_tgid();  /* 低 32 bits = tid */
	__u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&sys_start, &tid, &ts, BPF_ANY);
}

static __always_inline void sys_exit_record(enum syscall_kind kind)
{
	__u32 tid = (__u32)bpf_get_current_pid_tgid();
	__u64 *tsp = bpf_map_lookup_elem(&sys_start, &tid);
	if (!tsp)
		return;
	__u64 delta = bpf_ktime_get_ns() - *tsp;
	bpf_map_delete_elem(&sys_start, &tid);

	__u32 idx = kind;
	struct hist *h = bpf_map_lookup_elem(&syscall_lat, &idx);
	if (h)
		hist_record(h, delta);
}

SEC("tp/syscalls/sys_enter_read")
int handle_sys_enter_read(struct trace_event_raw_sys_enter *ctx)
{
	sys_enter_record();
	return 0;
}

SEC("tp/syscalls/sys_exit_read")
int handle_sys_exit_read(struct trace_event_raw_sys_exit *ctx)
{
	sys_exit_record(SC_READ);
	return 0;
}

SEC("tp/syscalls/sys_enter_write")
int handle_sys_enter_write(struct trace_event_raw_sys_enter *ctx)
{
	sys_enter_record();
	return 0;
}

SEC("tp/syscalls/sys_exit_write")
int handle_sys_exit_write(struct trace_event_raw_sys_exit *ctx)
{
	sys_exit_record(SC_WRITE);
	return 0;
}

SEC("tp/syscalls/sys_enter_ioctl")
int handle_sys_enter_ioctl(struct trace_event_raw_sys_enter *ctx)
{
	sys_enter_record();
	return 0;
}

SEC("tp/syscalls/sys_exit_ioctl")
int handle_sys_exit_ioctl(struct trace_event_raw_sys_exit *ctx)
{
	sys_exit_record(SC_IOCTL);
	return 0;
}

/* ===================================================================
 *  3) scheduler delay（sched_wakeup -> sched_switch-in）
 *
 *  sched_wakeup：記錄被喚醒 task 的 pid -> wakeup ts。
 *  sched_switch：當 next（被切入）task 之前曾被 wakeup，且其 comm 為 supervisor，
 *  計算 delta = now - wakeup_ts，即「可執行到實際上 CPU」的排程等待。
 *
 *  ctx 用 BTF 形式 struct trace_event_raw_sched_wakeup / _sched_switch。
 *  注意：sched_switch 的 next_comm 已在 tracepoint 內，故直接比對 next_comm，
 *  不用 bpf_get_current_comm（此刻 current 仍是 prev）。
 * =================================================================== */

SEC("tp/sched/sched_wakeup")
int handle_sched_wakeup(struct trace_event_raw_sched_wakeup *ctx)
{
	__u32 pid = ctx->pid;
	__u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&wakeup_ts, &pid, &ts, BPF_ANY);
	return 0;
}

/* sched_wakeup_new（fork 後第一次喚醒）共用同一處理。 */
SEC("tp/sched/sched_wakeup_new")
int handle_sched_wakeup_new(struct trace_event_raw_sched_wakeup *ctx)
{
	__u32 pid = ctx->pid;
	__u64 ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&wakeup_ts, &pid, &ts, BPF_ANY);
	return 0;
}

static __always_inline int next_comm_is_supervisor(struct trace_event_raw_sched_switch *ctx)
{
#pragma unroll
	for (int i = 0; i < COMM_PREFIX_LEN; i++) {
		if (ctx->next_comm[i] != COMM_PREFIX[i])
			return 0;
	}
	return 1;
}

SEC("tp/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
	__u32 next_pid = ctx->next_pid;
	__u64 *tsp = bpf_map_lookup_elem(&wakeup_ts, &next_pid);
	if (!tsp)
		return 0;

	/* 僅統計 supervisor；其它 task 的 wakeup 記錄在被切入時一併清掉以免外洩。 */
	if (next_comm_is_supervisor(ctx)) {
		__u64 delta = bpf_ktime_get_ns() - *tsp;
		__u32 idx = 0;
		struct hist *h = bpf_map_lookup_elem(&sched_lat, &idx);
		if (h)
			hist_record(h, delta);
	}
	bpf_map_delete_elem(&wakeup_ts, &next_pid);
	return 0;
}

/* ===================================================================
 *  safety_copro 自訂 tracepoint 的 ctx struct 宣告
 *
 *  共用 common preamble（common_type/flags/preempt_count/pid，自然對齊共 8 bytes），
 *  其後是各 TP 依 TP_STRUCT__entry 宣告順序排列的欄位。
 *
 *  重要：kernel 的 trace event struct 採「自然對齊 (natural alignment)」佈局，會為
 *  後續較寬欄位插入 padding（例如 u16 seq + u8 type 之後，u32 qlen 會對齊到 4-byte
 *  邊界，中間補 1 byte）。因此這些 struct「不可」加 __attribute__((packed))——否則
 *  offset 會與真實 format 不符。確切 offset 請以
 *    cat /sys/kernel/tracing/events/safety_copro/<name>/format
 *  為準（見 README）。這些 struct「不」在 vmlinux.h 內（自訂 module tracepoint），
 *  故在此手動宣告，欄位順序/型別/對齊必須與 format 完全一致。
 * =================================================================== */

struct safety_tp_common {
	__u16 common_type;
	__u8  common_flags;
	__u8  common_preempt_count;
	__s32 common_pid;
};

struct trace_event_safety_frame_received {
	struct safety_tp_common common;  /* offset 0, size 8 */
	__u16 seq;                       /* offset 8 */
	__u8  type;                      /* offset 10 (+1 byte pad) */
	__u32 qlen;                      /* offset 12 (4-byte aligned) */
};

struct trace_event_safety_frame_dropped {
	struct safety_tp_common common;  /* offset 0 */
	__u16 seq;                       /* offset 8 */
	__u8  type;                      /* offset 10 */
};

struct trace_event_safety_timeout_detected {
	struct safety_tp_common common;  /* offset 0 */
	__u32 timeout_ms;                /* offset 8 */
	__u32 count;                     /* offset 12 */
};

struct trace_event_safety_recovery_queued {
	struct safety_tp_common common;  /* offset 0 */
	__u32 count;                     /* offset 8 */
};

struct trace_event_safety_poll_wakeup {
	struct safety_tp_common common;  /* offset 0 */
	__u32 events;                    /* offset 8 */
};

/* ===================================================================
 *  4) 自訂 tracepoint 計數 helper
 * =================================================================== */

static __always_inline void counter_inc(enum tp_counter_idx idx)
{
	__u32 k = idx;
	__u64 *c = bpf_map_lookup_elem(&tp_counters, &k);
	if (c)
		(*c)++;
}

/* ===================================================================
 *  safety_frame_received：
 *    - counter++
 *    - 記錄 last_frame_ts（供 poll wakeup latency 計算）
 *    - 若 recovery_pending，視為 HEARTBEAT_RESTORED，推 timeline 事件並清 flag
 *      （5) fault-to-recovery timeline 的收尾）
 * =================================================================== */
SEC("tp/safety_copro/safety_frame_received")
int handle_safety_frame_received(struct trace_event_safety_frame_received *ctx)
{
	counter_inc(CNT_FRAME_RECEIVED);

	__u64 now = bpf_ktime_get_ns();
	__u32 zero = 0;

	__u64 *lf = bpf_map_lookup_elem(&last_frame_ts, &zero);
	if (lf)
		*lf = now;

	__u32 *pending = bpf_map_lookup_elem(&recovery_pending, &zero);
	if (pending && *pending) {
		*pending = 0;  /* 收尾：只回報 recovery 後的「第一個」frame */
		struct timeline_event *e =
			bpf_ringbuf_reserve(&timeline_rb, sizeof(*e), 0);
		if (e) {
			e->ts = now;
			e->kind = TL_HEARTBEAT_RESTORED;
			e->aux = ctx->seq;
			bpf_ringbuf_submit(e, 0);
		}
	}
	return 0;
}

/* safety_frame_dropped：僅計數。 */
SEC("tp/safety_copro/safety_frame_dropped")
int handle_safety_frame_dropped(struct trace_event_safety_frame_dropped *ctx)
{
	counter_inc(CNT_FRAME_DROPPED);
	return 0;
}

/* ===================================================================
 *  2) poll wakeup latency：safety_poll_wakeup 觸發時，以 last_frame_ts 算 delta。
 *  同時 counter++。
 * =================================================================== */
SEC("tp/safety_copro/safety_poll_wakeup")
int handle_safety_poll_wakeup(struct trace_event_safety_poll_wakeup *ctx)
{
	counter_inc(CNT_POLL_WAKEUP);

	__u32 zero = 0;
	__u64 *lf = bpf_map_lookup_elem(&last_frame_ts, &zero);
	if (!lf || *lf == 0)
		return 0;

	__u64 delta = bpf_ktime_get_ns() - *lf;
	struct hist *h = bpf_map_lookup_elem(&poll_lat, &zero);
	if (h)
		hist_record(h, delta);
	return 0;
}

/* ===================================================================
 *  5) fault-to-recovery timeline：
 *    safety_timeout_detected  -> 推 TL_TIMEOUT
 *    safety_recovery_queued   -> 推 TL_RECOVERY，並設 recovery_pending=1
 *  （HEARTBEAT_RESTORED 在 safety_frame_received 收尾，見上方）
 * =================================================================== */
SEC("tp/safety_copro/safety_timeout_detected")
int handle_safety_timeout_detected(struct trace_event_safety_timeout_detected *ctx)
{
	counter_inc(CNT_TIMEOUT_DETECTED);

	struct timeline_event *e =
		bpf_ringbuf_reserve(&timeline_rb, sizeof(*e), 0);
	if (e) {
		e->ts = bpf_ktime_get_ns();
		e->kind = TL_TIMEOUT;
		e->aux = ctx->count;
		bpf_ringbuf_submit(e, 0);
	}
	return 0;
}

SEC("tp/safety_copro/safety_recovery_queued")
int handle_safety_recovery_queued(struct trace_event_safety_recovery_queued *ctx)
{
	counter_inc(CNT_RECOVERY_QUEUED);

	__u32 zero = 0;
	__u32 one = 1;
	bpf_map_update_elem(&recovery_pending, &zero, &one, BPF_ANY);

	struct timeline_event *e =
		bpf_ringbuf_reserve(&timeline_rb, sizeof(*e), 0);
	if (e) {
		e->ts = bpf_ktime_get_ns();
		e->kind = TL_RECOVERY;
		e->aux = ctx->count;
		bpf_ringbuf_submit(e, 0);
	}
	return 0;
}
