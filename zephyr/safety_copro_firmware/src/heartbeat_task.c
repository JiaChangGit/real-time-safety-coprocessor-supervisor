/*
 * heartbeat_task.c - 心跳 task
 *
 * 角色：每 heartbeat_interval_ms (預設 100ms) 送一個 HEARTBEAT frame，
 *       讓 host 端判斷 co-processor 是否存活。
 * CPU pin：CPU0 (見 main.c)。心跳是最關鍵的存活訊號，刻意與 CPU1 上可能
 *          爆量的 telemetry 隔離，確保心跳節奏不被遙測負載拖累。
 *
 * 故障模擬：
 *   - inject_task_hang：heartbeat_stop，停止送 HEARTBEAT frame。
 *   - in_safe_mode：safe mode 下停送週期性心跳 (符合「安全停機」語意)。
 */

#include <zephyr/kernel.h>
#include "safety_app.h"
#include "safety_protocol.h"

void heartbeat_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t beat_seq = 0;

	while (1) {
		uint32_t interval =
			(uint32_t)atomic_get(&g_fault_state.heartbeat_interval_ms);

		/* 防呆：避免 0ms 造成忙迴圈 */
		if (interval == 0) {
			interval = SAFETY_DEFAULT_HEARTBEAT_MS;
		}

		bool hang = atomic_get(&g_fault_state.inject_task_hang) != 0;
		bool safe = atomic_get(&g_fault_state.in_safe_mode) != 0;

		if (!hang && !safe) {
			struct SafetyHeartbeatPayload hb = {
				.uptime_ms = safety_now_ms(),
				.beat_seq = beat_seq++,
			};
			(void)safety_send_frame(SAFETY_FRAME_HEARTBEAT,
						&hb, sizeof(hb));
		}

		k_msleep(interval);
	}
}
