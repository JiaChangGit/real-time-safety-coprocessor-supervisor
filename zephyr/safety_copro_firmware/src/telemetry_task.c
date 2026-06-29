/*
 * telemetry_task.c - 遙測 task
 *
 * 角色：週期性 (預設 250ms) 送 TELEMETRY frame，回報模擬感測值：
 *       temperature_c_x10 / voltage_mv / cpu_load_pct / fault_count。
 * CPU pin：CPU1 (見 main.c)。遙測與心跳分屬不同 CPU，避免一般 telemetry
 *          工作影響 heartbeat 節奏。safe mode 下停送週期性遙測。
 */

#include <zephyr/kernel.h>
#include "safety_app.h"
#include "safety_protocol.h"

/* 模擬感測值：以簡單可預測的方式變動，方便 host 端比對 */
#define TEMP_BASE_X10     250   /* 25.0C 起跳 */
#define TEMP_SWING_X10    150   /* 上下擺動幅度 */
#define VOLTAGE_NOMINAL   3300  /* 3.300 V */

void telemetry_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t tick = 0;

	while (1) {
		bool safe = atomic_get(&g_fault_state.in_safe_mode) != 0;

		if (!safe) {
			struct SafetyTelemetryPayload tm;

			/* 用三角波模擬溫度緩慢擺動 (確定性，host 易驗證) */
			uint32_t phase = tick % 20;
			int32_t delta = (phase < 10)
				? (int32_t)(phase * (TEMP_SWING_X10 / 10))
				: (int32_t)((20 - phase) *
					    (TEMP_SWING_X10 / 10));
			tm.temperature_c_x10 =
				(safety_u16)(TEMP_BASE_X10 + delta);

			tm.voltage_mv = VOLTAGE_NOMINAL;
			tm.cpu_load_pct = (safety_u16)(20 + (tick % 30));
			tm.fault_count =
				(safety_u16)atomic_get(&g_fault_state.fault_count);

			(void)safety_send_frame(SAFETY_FRAME_TELEMETRY,
						&tm, sizeof(tm));
		}

		tick++;

		k_msleep(SAFETY_TELEMETRY_PERIOD_MS);
	}
}
