/*
 * main.c - Safety Co-Processor Firmware 進入點與執行緒/CPU 配置
 *
 * 角色：
 *   1. 初始化共享故障狀態 g_fault_state。
 *   2. 初始化 UART 二進位協定通道 (safety_protocol.c)。
 *   3. 以 K_THREAD_STACK_DEFINE + k_thread_create 建立四個 task，
 *      並在「啟動前」用 k_thread_cpu_mask_* 把 heartbeat/telemetry 釘到
 *      固定 CPU，再 k_thread_start。
 *
 * SMP / CPU pin 理由 (qemu_cortex_a53/smp, 2 CPUs)：
 *   - heartbeat 釘 CPU0：心跳是最關鍵的存活訊號，必須節奏穩定。
 *   - telemetry 釘 CPU1：與心跳分屬不同 CPU，避免一般遙測工作搶占心跳時間片。
 *   - fault monitor / command handler 不釘：由 scheduler 在
 *     兩顆 CPU 間自由負載平衡。
 *
 * Zephyr API 重點：
 *   - k_thread_cpu_mask_clear / k_thread_cpu_mask_enable 只能在 thread 處於
 *     「尚未啟動 (created suspended) 或已 suspended/blocked」時呼叫。
 *     因此用 K_FOREVER 延遲建立 (= 建立但不排程)，設好 mask 後再 k_thread_start。
 *   - 需要 CONFIG_SMP=y + CONFIG_SCHED_CPU_MASK=y。
 */

#include <zephyr/kernel.h>

#include "safety_app.h"
#include "safety_protocol.h"

/* ===================================================================
 *  共享故障狀態 (全域單例；extern 宣告於 safety_app.h)
 * =================================================================== */
struct safety_fault_state g_fault_state;

/* ===================================================================
 *  Thread stacks 與 control blocks
 * =================================================================== */
K_THREAD_STACK_DEFINE(heartbeat_stack, SAFETY_STACK_HEARTBEAT);
K_THREAD_STACK_DEFINE(telemetry_stack, SAFETY_STACK_TELEMETRY);
K_THREAD_STACK_DEFINE(fault_stack, SAFETY_STACK_FAULT);
K_THREAD_STACK_DEFINE(command_stack, SAFETY_STACK_COMMAND);

static struct k_thread heartbeat_thread;
static struct k_thread telemetry_thread;
static struct k_thread fault_thread;
static struct k_thread command_thread;

/*
 * 建立一個「延遲啟動 (K_FOREVER)」的 thread。
 * 若 cpu >= 0 則先清空 CPU mask 再只 enable 該 CPU (= pin)；最後 start。
 * cpu < 0 表示不 pin，交給 scheduler。
 */
static void spawn_pinned(struct k_thread *thr, k_thread_stack_t *stack,
			 size_t stack_size, k_thread_entry_t entry,
			 int prio, int cpu, const char *name)
{
	k_tid_t tid;

	/* K_FOREVER：建立但不排程，讓我們能在啟動前安全設定 CPU mask。 */
	tid = k_thread_create(thr, stack, stack_size, entry,
			      NULL, NULL, NULL, prio, 0, K_FOREVER);

	(void)k_thread_name_set(tid, name);

#if defined(CONFIG_SCHED_CPU_MASK)
	if (cpu >= 0) {
		/* 先全清，再只開放目標 CPU = 釘死在該 CPU。 */
		(void)k_thread_cpu_mask_clear(tid);
		(void)k_thread_cpu_mask_enable(tid, cpu);
	}
#else
	ARG_UNUSED(cpu);
#endif

	k_thread_start(tid);
}

int main(void)
{
	/* 1) 初始化共享狀態 */
	safety_state_init(&g_fault_state);

	/* 2) 初始化 UART 協定通道 */
	if (safety_uart_init() != 0) {
		return -1;
	}

	/* 3) 建立並 (必要時) 釘住各 task */
	spawn_pinned(&heartbeat_thread, heartbeat_stack,
		     K_THREAD_STACK_SIZEOF(heartbeat_stack),
		     heartbeat_task_entry, SAFETY_PRIO_HEARTBEAT,
		     SAFETY_CPU_HEARTBEAT, "heartbeat");

	spawn_pinned(&telemetry_thread, telemetry_stack,
		     K_THREAD_STACK_SIZEOF(telemetry_stack),
		     telemetry_task_entry, SAFETY_PRIO_TELEMETRY,
		     SAFETY_CPU_TELEMETRY, "telemetry");

	spawn_pinned(&command_thread, command_stack,
		     K_THREAD_STACK_SIZEOF(command_stack),
		     command_handler_task_entry, SAFETY_PRIO_COMMAND,
		     -1, "command");

	spawn_pinned(&fault_thread, fault_stack,
		     K_THREAD_STACK_SIZEOF(fault_stack),
		     fault_monitor_task_entry, SAFETY_PRIO_FAULT,
		     -1, "fault_monitor");

	/* main thread 任務完成；回傳即結束 main (其它 task 繼續執行)。 */
	return 0;
}
