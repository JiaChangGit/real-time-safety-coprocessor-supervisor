// SPDX-License-Identifier: GPL-2.0
/*
 * safety_trace_user.c - safety_trace BPF programs 的 libbpf userspace
 * loader / reporter。
 *
 * 角色：
 *   - 透過 bpftool 產生的 skeleton (safety_trace.skel.h) open/load/attach BPF 程式。
 *   - 以 ring_buffer 接收 fault-to-recovery timeline 事件。
 *   - 執行 --duration 秒（或直到 SIGINT/SIGTERM），停止後讀出各直方圖/計數 map，
 *     彙總並寫出三份報告：
 *       reports/latency_report.csv  （machine-readable）
 *       reports/latency_report.md   （human-readable，英文表格）
 *       reports/fault_timeline.md    （fault->recovery 時間線，英文）
 *
 * 慣例：註解使用台灣繁體中文，技術名詞保留英文；所有對外輸出/報告字串為英文。
 *
 * 直方圖為 log2 buckets：slot i 涵蓋 [2^i, 2^(i+1)) ns（slot 0 含 0）。percentile 由
 * bucket 累積分佈近似（取 bucket 上界 2^(i+1) ns，slot 0 取 1 ns），故為「近似值」。
 * min/max/avg 由 BPF 端精確累加之 min/max/sum/count 得到。
 */

#include <argp.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <bpf/libbpf.h>

#include "safety_trace.skel.h"

/* ===================================================================
 *  與 BPF 端 (safety_trace.bpf.c) 完全一致的常數 / 型別
 * =================================================================== */

#define MAX_SLOTS 64

/* enum syscall_kind 對應索引（syscall_lat percpu-array 的 key）。 */
enum { SC_READ = 0, SC_WRITE = 1, SC_IOCTL = 2, SC_MAX = 3 };

/* enum tp_counter_idx（tp_counters percpu-array 的 key）。 */
enum {
	CNT_FRAME_RECEIVED   = 0,
	CNT_FRAME_DROPPED    = 1,
	CNT_TIMEOUT_DETECTED = 2,
	CNT_RECOVERY_QUEUED  = 3,
	CNT_POLL_WAKEUP      = 4,
	CNT_MAX              = 5,
};

/* enum timeline_kind。 */
enum {
	TL_TIMEOUT            = 0,
	TL_RECOVERY           = 1,
	TL_HEARTBEAT_RESTORED = 2,
};

/* 與 BPF 端 struct hist 二進位一致（percpu map 每 CPU 一份）。 */
struct hist {
	__u64 slots[MAX_SLOTS];
	__u64 count;
	__u64 sum;
	__u64 min;
	__u64 max;
};

/* 與 BPF 端 struct timeline_event 一致。 */
struct timeline_event {
	__u64 ts;
	__u32 kind;
	__u32 aux;
};

/* ===================================================================
 *  CLI 參數
 * =================================================================== */

static struct env {
	long duration_sec;     /* 0 = run until signal */
	const char *report_dir;
	int verbose;
} env = {
	.duration_sec = 0,
	.report_dir = "reports",
	.verbose = 0,
};

const char *argp_program_version = "safety_trace_user 1.0";
static const char doc[] =
	"safety_trace_user - eBPF CO-RE tracing agent for the Real-Time Safety "
	"Co-Processor Supervisor.\n\n"
	"Attaches to syscall, sched and safety_copro tracepoints, then writes "
	"latency and fault-timeline reports.";

static const struct argp_option opts[] = {
	{ "duration", 'd', "SECONDS", 0,
	  "Run for N seconds then stop and report (default: until SIGINT/SIGTERM)" },
	{ "report-dir", 'r', "DIR", 0,
	  "Directory for output reports (default: reports)" },
	{ "verbose", 'v', NULL, 0, "Verbose libbpf debug output" },
	{ 0 },
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'd':
		errno = 0;
		env.duration_sec = strtol(arg, NULL, 10);
		if (errno || env.duration_sec < 0) {
			fprintf(stderr, "Invalid --duration: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'r':
		env.report_dir = arg;
		break;
	case 'v':
		env.verbose = 1;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const struct argp argp = { opts, parse_arg, NULL, doc };

/* ===================================================================
 *  全域狀態
 * =================================================================== */

static volatile sig_atomic_t exiting = 0;

static void sig_handler(int sig)
{
	(void)sig;
	exiting = 1;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *fmt,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, fmt, args);
}

/* timeline 事件最多保留筆數（足夠涵蓋一次測試多輪 fault/recovery）。 */
#define MAX_TIMELINE 4096
static struct timeline_event g_timeline[MAX_TIMELINE];
static size_t g_timeline_n = 0;

/* 程式啟動時的 wall-clock 與 monotonic 基準，用於把 ktime(ns) 轉成相對秒數。 */
static __u64 g_start_mono_ns = 0;

static __u64 now_mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

/* ring_buffer callback：複製 timeline 事件到 g_timeline。 */
static int handle_rb_event(void *ctx, void *data, size_t size)
{
	(void)ctx;
	if (size < sizeof(struct timeline_event))
		return 0;
	if (g_timeline_n < MAX_TIMELINE) {
		memcpy(&g_timeline[g_timeline_n], data,
		       sizeof(struct timeline_event));
		g_timeline_n++;
	}
	return 0;
}

/* ===================================================================
 *  直方圖彙總（percpu map -> 單一 struct hist）
 * =================================================================== */

/* 跨 CPU 加總一個 percpu-array slot（index）的 struct hist。 */
static int sum_percpu_hist(int map_fd, __u32 index, int ncpu, struct hist *out)
{
	struct hist *per = calloc(ncpu, sizeof(struct hist));
	if (!per)
		return -1;

	memset(out, 0, sizeof(*out));
	if (bpf_map_lookup_elem(map_fd, &index, per) != 0) {
		/* 該 index 尚無資料時 lookup 仍應成功（array map 預設全 0）；
		 * 失敗則視為空。 */
		free(per);
		return 0;
	}

	int have_min = 0;
	for (int c = 0; c < ncpu; c++) {
		out->count += per[c].count;
		out->sum   += per[c].sum;
		for (int s = 0; s < MAX_SLOTS; s++)
			out->slots[s] += per[c].slots[s];
		if (per[c].count == 0)
			continue;
		if (!have_min || per[c].min < out->min) {
			out->min = per[c].min;
			have_min = 1;
		}
		if (per[c].max > out->max)
			out->max = per[c].max;
	}
	free(per);
	return 0;
}

/* 跨 CPU 加總一個 percpu-array slot 的 u64 counter。 */
static __u64 sum_percpu_u64(int map_fd, __u32 index, int ncpu)
{
	__u64 *per = calloc(ncpu, sizeof(__u64));
	if (!per)
		return 0;
	__u64 total = 0;
	if (bpf_map_lookup_elem(map_fd, &index, per) == 0) {
		for (int c = 0; c < ncpu; c++)
			total += per[c];
	}
	free(per);
	return total;
}

/*
 * 由 log2 直方圖近似 percentile（回傳 ns）。
 * 取目標累積 rank 落入的 bucket，回傳該 bucket 的「上界」2^(slot+1) ns
 * （slot 0 特例回傳 1 ns）。這是保守的近似上界。
 */
static __u64 hist_percentile_ns(const struct hist *h, double pct)
{
	if (h->count == 0)
		return 0;
	__u64 target = (__u64)(pct / 100.0 * (double)h->count + 0.5);
	if (target == 0)
		target = 1;
	__u64 cum = 0;
	for (int s = 0; s < MAX_SLOTS; s++) {
		cum += h->slots[s];
		if (cum >= target) {
			if (s == 0)
				return 1;
			return 1ULL << (s + 1);
		}
	}
	return h->max;
}

static double hist_avg_ns(const struct hist *h)
{
	if (h->count == 0)
		return 0.0;
	return (double)h->sum / (double)h->count;
}

/* ===================================================================
 *  報告輸出
 * =================================================================== */

/* 一個 metric 的彙總列（給 CSV / MD 共用）。 */
struct metric_row {
	const char *name;
	const char *unit;
	struct hist h;
};

static void collect_metric(struct metric_row *row, const char *name,
			   int map_fd, __u32 index, int ncpu)
{
	row->name = name;
	row->unit = "ns";
	sum_percpu_hist(map_fd, index, ncpu, &row->h);
}

static int ensure_dir(const char *dir)
{
	if (mkdir(dir, 0755) == 0)
		return 0;
	if (errno == EEXIST)
		return 0;
	fprintf(stderr, "Failed to create report dir '%s': %s\n", dir,
		strerror(errno));
	return -1;
}

static void write_csv(FILE *f, struct metric_row *rows, int nrows)
{
	fprintf(f, "metric,unit,count,min,p50,p90,max,avg\n");
	for (int i = 0; i < nrows; i++) {
		struct hist *h = &rows[i].h;
		fprintf(f, "%s,%s,%llu,%llu,%llu,%llu,%llu,%.0f\n",
			rows[i].name, rows[i].unit,
			(unsigned long long)h->count,
			(unsigned long long)h->min,
			(unsigned long long)hist_percentile_ns(h, 50.0),
			(unsigned long long)hist_percentile_ns(h, 90.0),
			(unsigned long long)h->max,
			hist_avg_ns(h));
	}
}

static void write_md_latency(FILE *f, struct metric_row *rows, int nrows,
			     __u64 counters[CNT_MAX])
{
	fprintf(f, "# Safety Co-Processor Supervisor - Latency Report\n\n");
	fprintf(f, "All latency values are in nanoseconds (ns). Percentiles "
		   "(p50/p90) are approximated from log2 histogram buckets and "
		   "represent the upper bound of the bucket containing the "
		   "target rank. min/max/avg are exact.\n\n");

	fprintf(f, "## Latency metrics\n\n");
	fprintf(f, "| Metric | Count | Min (ns) | p50 (ns) | p90 (ns) | Max (ns) | Avg (ns) |\n");
	fprintf(f, "|--------|------:|---------:|---------:|---------:|---------:|---------:|\n");
	for (int i = 0; i < nrows; i++) {
		struct hist *h = &rows[i].h;
		fprintf(f, "| %s | %llu | %llu | %llu | %llu | %llu | %.0f |\n",
			rows[i].name,
			(unsigned long long)h->count,
			(unsigned long long)h->min,
			(unsigned long long)hist_percentile_ns(h, 50.0),
			(unsigned long long)hist_percentile_ns(h, 90.0),
			(unsigned long long)h->max,
			hist_avg_ns(h));
	}

	/* 每個 metric 的 log2 bucket 分佈（僅列出非空 bucket）。 */
	fprintf(f, "\n## Histogram distributions (log2 buckets)\n\n");
	for (int i = 0; i < nrows; i++) {
		struct hist *h = &rows[i].h;
		fprintf(f, "### %s\n\n", rows[i].name);
		if (h->count == 0) {
			fprintf(f, "No samples observed.\n\n");
			continue;
		}
		fprintf(f, "| range (ns) | count |\n");
		fprintf(f, "|------------|------:|\n");
		for (int s = 0; s < MAX_SLOTS; s++) {
			if (h->slots[s] == 0)
				continue;
			unsigned long long lo = (s == 0) ? 0ULL : (1ULL << s);
			unsigned long long hi = (1ULL << (s + 1)) - 1;
			fprintf(f, "| [%llu, %llu] | %llu |\n", lo, hi,
				(unsigned long long)h->slots[s]);
		}
		fprintf(f, "\n");
	}

	/* 自訂 tracepoint 計數表。 */
	fprintf(f, "## safety_copro tracepoint counts\n\n");
	fprintf(f, "| Tracepoint | Count |\n");
	fprintf(f, "|------------|------:|\n");
	fprintf(f, "| safety_frame_received | %llu |\n",
		(unsigned long long)counters[CNT_FRAME_RECEIVED]);
	fprintf(f, "| safety_frame_dropped | %llu |\n",
		(unsigned long long)counters[CNT_FRAME_DROPPED]);
	fprintf(f, "| safety_timeout_detected | %llu |\n",
		(unsigned long long)counters[CNT_TIMEOUT_DETECTED]);
	fprintf(f, "| safety_recovery_queued | %llu |\n",
		(unsigned long long)counters[CNT_RECOVERY_QUEUED]);
	fprintf(f, "| safety_poll_wakeup | %llu |\n",
		(unsigned long long)counters[CNT_POLL_WAKEUP]);
	fprintf(f, "\n");
}

static const char *tl_kind_str(__u32 kind)
{
	switch (kind) {
	case TL_TIMEOUT:            return "TIMEOUT";
	case TL_RECOVERY:           return "RECOVERY";
	case TL_HEARTBEAT_RESTORED: return "HEARTBEAT_RESTORED";
	default:                    return "UNKNOWN";
	}
}

/* 把 ktime ns 轉成相對啟動時間的秒數（double）。 */
static double rel_seconds(__u64 ts_ns)
{
	if (ts_ns <= g_start_mono_ns)
		return 0.0;
	return (double)(ts_ns - g_start_mono_ns) / 1e9;
}

/*
 * 寫出 fault timeline 報告。事件已依到達順序保存（ringbuf 為 FIFO）。
 * 配對規則：每遇到 TIMEOUT 記為一段 fault 的開始；其後第一個 RECOVERY 計算
 * timeout->recovery 延遲；其後第一個 HEARTBEAT_RESTORED 計算
 * recovery->restored 與 timeout->restored（fault-to-recovery）總延遲。
 */
static void write_fault_timeline(FILE *f)
{
	fprintf(f, "# Safety Co-Processor Supervisor - Fault-to-Recovery Timeline\n\n");
	fprintf(f, "Timestamps are relative to tracing start (seconds). "
		   "Durations are in milliseconds (ms).\n\n");

	if (g_timeline_n == 0) {
		fprintf(f, "No fault/recovery events were observed during the "
			   "tracing window.\n");
		return;
	}

	fprintf(f, "## Ordered events\n\n");
	fprintf(f, "| # | t (s) | event | aux |\n");
	fprintf(f, "|--:|------:|-------|----:|\n");
	for (size_t i = 0; i < g_timeline_n; i++) {
		fprintf(f, "| %zu | %.6f | %s | %u |\n", i + 1,
			rel_seconds(g_timeline[i].ts),
			tl_kind_str(g_timeline[i].kind),
			g_timeline[i].aux);
	}
	fprintf(f, "\n");

	/* 配對成 fault 週期。 */
	fprintf(f, "## Fault-to-recovery cycles\n\n");
	fprintf(f, "| Cycle | timeout t (s) | timeout->recovery (ms) | "
		   "recovery->restored (ms) | total fault->recovery (ms) |\n");
	fprintf(f, "|------:|--------------:|-----------------------:|"
		   "------------------------:|---------------------------:|\n");

	int cycle = 0;
	size_t i = 0;
	while (i < g_timeline_n) {
		if (g_timeline[i].kind != TL_TIMEOUT) {
			i++;
			continue;
		}
		__u64 t_timeout = g_timeline[i].ts;
		__u64 t_recovery = 0, t_restored = 0;

		/* 在後續事件中找第一個 RECOVERY 與其後第一個 HEARTBEAT_RESTORED，
		 * 且不跨越下一個 TIMEOUT。 */
		size_t j = i + 1;
		for (; j < g_timeline_n; j++) {
			if (g_timeline[j].kind == TL_TIMEOUT)
				break;
			if (!t_recovery && g_timeline[j].kind == TL_RECOVERY)
				t_recovery = g_timeline[j].ts;
			else if (t_recovery && !t_restored &&
				 g_timeline[j].kind == TL_HEARTBEAT_RESTORED) {
				t_restored = g_timeline[j].ts;
				break;
			}
		}

		cycle++;
		double to_rec_ms = t_recovery ?
			(double)(t_recovery - t_timeout) / 1e6 : -1.0;
		double rec_res_ms = (t_recovery && t_restored) ?
			(double)(t_restored - t_recovery) / 1e6 : -1.0;
		double total_ms = t_restored ?
			(double)(t_restored - t_timeout) / 1e6 : -1.0;

		fprintf(f, "| %d | %.6f | ", cycle, rel_seconds(t_timeout));
		if (to_rec_ms >= 0) fprintf(f, "%.3f", to_rec_ms);
		else fprintf(f, "n/a");
		fprintf(f, " | ");
		if (rec_res_ms >= 0) fprintf(f, "%.3f", rec_res_ms);
		else fprintf(f, "n/a");
		fprintf(f, " | ");
		if (total_ms >= 0) fprintf(f, "%.3f", total_ms);
		else fprintf(f, "n/a (not yet recovered)");
		fprintf(f, " |\n");

		/* 從本週期結束位置（j）繼續；若 j 因下一 TIMEOUT 中斷，則 i=j。 */
		i = (j > i) ? j : i + 1;
	}
	fprintf(f, "\n");
}

/* ===================================================================
 *  彙總並輸出全部報告
 * =================================================================== */
static int generate_reports(struct safety_trace_bpf *skel)
{
	int ncpu = libbpf_num_possible_cpus();
	if (ncpu <= 0) {
		fprintf(stderr, "libbpf_num_possible_cpus failed: %d\n", ncpu);
		return -1;
	}

	int syscall_fd = bpf_map__fd(skel->maps.syscall_lat);
	int poll_fd    = bpf_map__fd(skel->maps.poll_lat);
	int sched_fd   = bpf_map__fd(skel->maps.sched_lat);
	int cnt_fd     = bpf_map__fd(skel->maps.tp_counters);

	struct metric_row rows[5];
	int nrows = 0;
	collect_metric(&rows[nrows++], "read_latency_ns",  syscall_fd, SC_READ,  ncpu);
	collect_metric(&rows[nrows++], "write_latency_ns", syscall_fd, SC_WRITE, ncpu);
	collect_metric(&rows[nrows++], "ioctl_latency_ns", syscall_fd, SC_IOCTL, ncpu);
	collect_metric(&rows[nrows++], "poll_wakeup_ns",   poll_fd,    0,        ncpu);
	collect_metric(&rows[nrows++], "sched_delay_ns",   sched_fd,   0,        ncpu);

	__u64 counters[CNT_MAX];
	for (int i = 0; i < CNT_MAX; i++)
		counters[i] = sum_percpu_u64(cnt_fd, i, ncpu);

	if (ensure_dir(env.report_dir) != 0)
		return -1;

	char path[1024];
	FILE *f;

	/* latency_report.csv */
	snprintf(path, sizeof(path), "%s/latency_report.csv", env.report_dir);
	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	write_csv(f, rows, nrows);
	fclose(f);
	printf("Wrote %s\n", path);

	/* latency_report.md */
	snprintf(path, sizeof(path), "%s/latency_report.md", env.report_dir);
	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	write_md_latency(f, rows, nrows, counters);
	fclose(f);
	printf("Wrote %s\n", path);

	/* fault_timeline.md */
	snprintf(path, sizeof(path), "%s/fault_timeline.md", env.report_dir);
	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	write_fault_timeline(f);
	fclose(f);
	printf("Wrote %s\n", path);

	return 0;
}

/* ===================================================================
 *  main
 * =================================================================== */
int main(int argc, char **argv)
{
	struct safety_trace_bpf *skel = NULL;
	struct ring_buffer *rb = NULL;
	int err;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return 1;

	libbpf_set_print(libbpf_print_fn);

	/* open -> load -> attach */
	skel = safety_trace_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open BPF skeleton\n");
		return 1;
	}

	err = safety_trace_bpf__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load BPF skeleton: %d (%s)\n",
			err, strerror(-err));
		fprintf(stderr, "Hint: ensure the safety_copro module is loaded "
			"and you are running inside the QEMU guest kernel that "
			"exposes the safety_copro tracepoints.\n");
		goto cleanup;
	}

	err = safety_trace_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs: %d (%s)\n",
			err, strerror(-err));
		goto cleanup;
	}

	/* timeline ringbuf */
	rb = ring_buffer__new(bpf_map__fd(skel->maps.timeline_rb),
			      handle_rb_event, NULL, NULL);
	if (!rb) {
		err = -1;
		fprintf(stderr, "Failed to create ring buffer\n");
		goto cleanup;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	g_start_mono_ns = now_mono_ns();

	if (env.duration_sec > 0)
		printf("Tracing for %ld seconds... (Ctrl-C to stop early)\n",
		       env.duration_sec);
	else
		printf("Tracing... (Ctrl-C to stop)\n");

	__u64 deadline_ns = env.duration_sec > 0 ?
		g_start_mono_ns + (__u64)env.duration_sec * 1000000000ULL : 0;

	/* main loop：poll ringbuf，並檢查 duration / signal。 */
	while (!exiting) {
		err = ring_buffer__poll(rb, 200 /* ms */);
		if (err == -EINTR) {
			err = 0;
			break;
		}
		if (err < 0) {
			fprintf(stderr, "ring_buffer__poll error: %d\n", err);
			break;
		}
		if (deadline_ns && now_mono_ns() >= deadline_ns)
			break;
	}

	printf("Stopping. Collected %zu timeline event(s).\n", g_timeline_n);

	/* 收尾前再 drain 一次 ringbuf 以撈出殘留事件。 */
	ring_buffer__consume(rb);

	err = generate_reports(skel);

cleanup:
	ring_buffer__free(rb);
	safety_trace_bpf__destroy(skel);
	return err ? 1 : 0;
}
