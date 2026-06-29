/*
 * fault_monitor_task.c - 故障監測 task
 *
 * 角色：週期掃描 (100ms) 共享狀態的 inject_* 旗標，對 checksum-error
 *       response 與 critical fault 送 FAULT_EVENT frame，並遞增 fault_count。
 * CPU pin：無 (由 scheduler 自由調度於 CPU0/CPU1)。
 *
 * 設計：以 edge-triggered (邊緣觸發) 為主——旗標由 0→1 才送 event，
 *       避免每次掃描都重複送同一個 fault。heartbeat_stop 不在此送 fault；
 *       它由 heartbeat task 停送 HEARTBEAT，讓 Linux 端 timeout 偵測。
 */

#include <zephyr/kernel.h>
#include "safety_app.h"
#include "safety_protocol.h"

/* severity 對應 safety_protocol.h 註解：0=info 1=warn 2=critical */
#define SEV_INFO      0
#define SEV_WARN      1
#define SEV_CRITICAL  2

static void emit_fault(safety_u8 fault_type, safety_u8 severity,
		       safety_u16 detail_code)
{
	struct SafetyFaultEventPayload ev = {
		.fault_type = fault_type,
		.severity = severity,
		.detail_code = detail_code,
		.fault_uptime_ms = safety_now_ms(),
	};

	atomic_inc(&g_fault_state.fault_count);
	(void)safety_send_frame(SAFETY_FRAME_FAULT_EVENT, &ev, sizeof(ev));
}

void fault_monitor_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* 上一輪各旗標狀態，用於偵測 0→1 邊緣 */
	bool prev_checksum = false;
	bool prev_critical = false;

	while (1) {
		bool checksum =
			atomic_get(&g_fault_state.inject_checksum_error) != 0;
		bool critical =
			atomic_get(&g_fault_state.inject_critical_fault) != 0;

		/* --- CHECKSUM_ERROR：邊緣觸發，warn --- */
		if (checksum && !prev_checksum) {
			emit_fault(SAFETY_FAULT_CHECKSUM_ERROR, SEV_WARN, 0x0002);
		}

		/* --- CRITICAL_FAULT：邊緣觸發，critical --- */
		if (critical && !prev_critical) {
			emit_fault(SAFETY_FAULT_CRITICAL, SEV_CRITICAL, 0x0003);
			atomic_set(&g_fault_state.in_safe_mode, 1);
		}

		prev_checksum = checksum;
		prev_critical = critical;

		k_msleep(SAFETY_FAULT_SCAN_MS);
	}
}
