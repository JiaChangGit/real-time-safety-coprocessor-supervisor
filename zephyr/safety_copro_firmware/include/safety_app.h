/*
 * safety_app.h - Safety Co-Processor Firmware 內部共用標頭
 *
 * 角色：定義「整個 firmware 共用」的東西——
 *   - 共享故障狀態 (fault state) 結構與其 atomic 欄位
 *   - thread 優先權、stack 大小、CPU pin 巨集
 *   - 各 task entry point 與 frame TX / RX 輔助函式原型
 *   - UART 裝置取得方式
 *
 * 本檔不負責線格式 (那是 safety_protocol.h，LOCKED 不可改)，
 * 只負責 firmware 端的執行緒/狀態/IO 抽象。
 *
 * CPU pin 概念 (qemu_cortex_a53/smp, 2 CPUs)：
 *   - CPU0：heartbeat task (心跳必須最即時，與遙測負載隔離)
 *   - CPU1：telemetry task (高頻送資料，可能爆量，不可拖累心跳)
 *   - 其餘 (fault monitor / command handler / main) 不 pin，
 *     由 scheduler 在兩顆 CPU 間自由調度。
 */

#ifndef SAFETY_APP_H
#define SAFETY_APP_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include "safety_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 *  Thread 優先權 (數字越小越優先；皆為 preemptible 正優先權)
 * =================================================================== */
#define SAFETY_PRIO_HEARTBEAT   4   /* 最高：心跳最即時 */
#define SAFETY_PRIO_COMMAND     5   /* 命令處理 */
#define SAFETY_PRIO_FAULT       6   /* 故障監測 */
#define SAFETY_PRIO_TELEMETRY   7   /* 最低：遙測可被搶占 */

/* ===================================================================
 *  Thread stack 大小 (對應 prj.conf 內各自的 Kconfig 註解)
 * =================================================================== */
#define SAFETY_STACK_HEARTBEAT  1024
#define SAFETY_STACK_TELEMETRY  1536
#define SAFETY_STACK_FAULT      1536
#define SAFETY_STACK_COMMAND    2048

/* ===================================================================
 *  CPU pin (需要 CONFIG_SMP=y + CONFIG_SCHED_CPU_MASK=y)
 * =================================================================== */
#define SAFETY_CPU_HEARTBEAT    0
#define SAFETY_CPU_TELEMETRY    1

/* ===================================================================
 *  時序預設值
 * =================================================================== */
#define SAFETY_DEFAULT_HEARTBEAT_MS   100u   /* 心跳預設 100ms */
#define SAFETY_TELEMETRY_PERIOD_MS    250u   /* 遙測 250ms */
#define SAFETY_FAULT_SCAN_MS          100u   /* 故障掃描 100ms */

/* ===================================================================
 *  共享故障狀態
 *  ----------------------------------------------------------------
 *  注入旗標 (inject_*) 與計數使用 atomic_t，可在任意 task / shell /
 *  UART ISR-thread context 無鎖讀寫。heartbeat_interval_ms 也以 atomic
 *  存放 (單一 32-bit 寫入在 ARM64 為 atomic，但仍用 atomic API 表明意圖)。
 * =================================================================== */
struct safety_fault_state {
	atomic_t inject_task_hang;        /* 1 = heartbeat_stop，不送 HEARTBEAT */
	atomic_t inject_checksum_error;   /* 1 = 回報 checksum_error_response */
	atomic_t inject_critical_fault;   /* 1 = 回報 critical FAULT_EVENT */
	atomic_t fault_count;             /* 已偵測/回報的故障總數 */
	atomic_t in_safe_mode;            /* 1 = safe mode，週期性送資料暫停 */
	atomic_t heartbeat_interval_ms;   /* 心跳週期 (ms)，預設 100 */
};

/* 全域單例 (定義於 main.c) */
extern struct safety_fault_state g_fault_state;

/* ---- 故障狀態存取輔助 (inline) ---- */
static inline void safety_state_init(struct safety_fault_state *s)
{
	atomic_clear(&s->inject_task_hang);
	atomic_clear(&s->inject_checksum_error);
	atomic_clear(&s->inject_critical_fault);
	atomic_clear(&s->fault_count);
	atomic_clear(&s->in_safe_mode);
	atomic_set(&s->heartbeat_interval_ms, SAFETY_DEFAULT_HEARTBEAT_MS);
}

/* ===================================================================
 *  RX：解析完成的 frame 透過 message queue 交給 command handler
 *  ----------------------------------------------------------------
 *  一筆訊息 = 一個完整且已驗證 (checksum OK) 的 frame，封裝成定長結構，
 *  避免在 queue 內傳指標 / 動態配置。
 * =================================================================== */
struct safety_rx_frame {
	struct SafetyFrameHeader hdr;
	safety_u8 payload[SAFETY_MAX_PAYLOAD];
};

extern struct k_msgq g_rx_msgq;   /* 定義於 safety_protocol.c */

/* ===================================================================
 *  函式原型
 * =================================================================== */

/* --- safety_protocol.c：firmware 端 IO / 時間輔助 --- */

/* 取得單調 uptime (ms)，截成 32-bit 給線格式用。 */
uint32_t safety_now_ms(void);

/* 初始化 UART（取得裝置、設定 IRQ callback、啟用 RX 中斷）。
 * 回傳 0 成功，負值失敗。 */
int safety_uart_init(void);

/*
 * Frame TX helper：把指定 type + payload 封裝成完整 frame 並整包送出 UART。
 * sequence_id 由內部單調遞增 (thread-safe)。payload 可為 NULL（payload_len=0）。
 * 回傳送出的位元組數，失敗回傳負值。
 * TX 受一把 mutex 保護，確保多 task 不會交錯位元組。
 */
int safety_send_frame(safety_u8 type, const void *payload,
		      safety_u16 payload_len);

/* 取得目前已送出的 frame 數 (除錯用)。 */
uint32_t safety_tx_seq_peek(void);

/* --- 各 task entry point --- */
void heartbeat_task_entry(void *p1, void *p2, void *p3);
void telemetry_task_entry(void *p1, void *p2, void *p3);
void fault_monitor_task_entry(void *p1, void *p2, void *p3);
void command_handler_task_entry(void *p1, void *p2, void *p3);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_APP_H */
