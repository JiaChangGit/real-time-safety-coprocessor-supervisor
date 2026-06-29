/*
 * command_handler_task.c - 命令處理 task
 *
 * 角色：從 g_rx_msgq (由 safety_protocol.c 的 UART RX 狀態機投遞、已驗證
 *       checksum 的 frame) 取出 COMMAND frame，解析 SafetyCommandPayload，
 *       執行對應動作並回 ACK / NACK。
 * CPU pin：無 (scheduler 自由調度)。
 *
 * 支援命令 (enum safety_command_type)：
 *   GET_STATUS             -> 回 TELEMETRY 快照 + ACK
 *   INJECT_FAULT           -> arg8 對應 heartbeat_stop/checksum/critical 旗標 + ACK
 *   REQUEST_RECOVERY       -> 清故障旗標 + 退出 safe mode + RECOVERY_REPORT(1) + ACK
 *   ENTER_SAFE_MODE        -> in_safe_mode=1 + ACK
 *   RESET_FAULT_STATE      -> 清故障旗標 / fault_count / safe mode + ACK
 *
 * 規則：每個被接受的命令都回 ACK (SafetyAckPayload)；
 *       payload 長度不符或未知命令回 NACK (SafetyNackPayload + reason)。
 */

#include <zephyr/kernel.h>

#include "safety_app.h"
#include "safety_protocol.h"

static void send_ack(safety_u16 acked_seq)
{
	struct SafetyAckPayload ack = {
		.acked_sequence_id = acked_seq,
		.reserved = 0,
	};
	(void)safety_send_frame(SAFETY_FRAME_ACK, &ack, sizeof(ack));
}

static void send_nack(safety_u16 nacked_seq, safety_u8 reason)
{
	struct SafetyNackPayload nack = {
		.nacked_sequence_id = nacked_seq,
		.reason = reason,
		.reserved = 0,
	};
	(void)safety_send_frame(SAFETY_FRAME_NACK, &nack, sizeof(nack));
}

/* GET_STATUS：回一個即時 telemetry 快照 (與 telemetry task 同格式)。 */
static void reply_status(void)
{
	struct SafetyTelemetryPayload tm = {
		.temperature_c_x10 = 250,
		.voltage_mv = 3300,
		.cpu_load_pct = 25,
		.fault_count =
			(safety_u16)atomic_get(&g_fault_state.fault_count),
	};
	(void)safety_send_frame(SAFETY_FRAME_TELEMETRY, &tm, sizeof(tm));
}

/* 依 INJECT_FAULT 的 arg8 (= enum safety_fault_type) 設對應旗標。
 * 回傳 true 表示是已知 fault type。 */
static bool apply_inject(safety_u8 fault_type)
{
	switch (fault_type) {
	case SAFETY_FAULT_TASK_HANG:
		atomic_set(&g_fault_state.inject_task_hang, 1);
		return true;
	case SAFETY_FAULT_CHECKSUM_ERROR:
		atomic_set(&g_fault_state.inject_checksum_error, 1);
		return true;
	case SAFETY_FAULT_CRITICAL:
		atomic_set(&g_fault_state.inject_critical_fault, 1);
		return true;
	default:
		return false;
	}
}

/* 清除所有注入旗標 (供 RESET / RECOVERY 共用)。 */
static void clear_all_injects(void)
{
	atomic_clear(&g_fault_state.inject_task_hang);
	atomic_clear(&g_fault_state.inject_checksum_error);
	atomic_clear(&g_fault_state.inject_critical_fault);
}

static void handle_command(const struct safety_rx_frame *frame)
{
	safety_u16 seq = frame->hdr.sequence_id;
	struct SafetyCommandPayload cmd;

	/* payload 長度必須剛好等於 command payload */
	if (frame->hdr.payload_length != sizeof(struct SafetyCommandPayload)) {
		send_nack(seq, SAFETY_NACK_PAYLOAD);
		return;
	}
	memcpy(&cmd, frame->payload, sizeof(cmd));

	switch (cmd.command) {
	case SAFETY_CMD_GET_STATUS:
		reply_status();
		send_ack(seq);
		break;

	case SAFETY_CMD_INJECT_FAULT:
		if (!apply_inject(cmd.arg8)) {
			send_nack(seq, SAFETY_NACK_PAYLOAD);
			break;
		}
		send_ack(seq);
		break;

	case SAFETY_CMD_RESET_FAULT_STATE:
		clear_all_injects();
		atomic_clear(&g_fault_state.fault_count);
		atomic_clear(&g_fault_state.in_safe_mode);
		send_ack(seq);
		break;

	case SAFETY_CMD_ENTER_SAFE_MODE:
		atomic_set(&g_fault_state.in_safe_mode, 1);
		send_ack(seq);
		break;

	case SAFETY_CMD_REQUEST_RECOVERY: {
		/* 清 hang + 退出 safe mode → heartbeat 恢復 kick/送 frame。 */
		clear_all_injects();
		atomic_clear(&g_fault_state.in_safe_mode);

		struct SafetyRecoveryReportPayload rr = {
			.recovered = 1,
			.new_state = 0, /* 0 = normal */
			.reserved = 0,
			.recovery_uptime_ms = safety_now_ms(),
		};
		(void)safety_send_frame(SAFETY_FRAME_RECOVERY_REPORT,
					&rr, sizeof(rr));
		send_ack(seq);
		break;
	}

	default:
		send_nack(seq, SAFETY_NACK_UNKNOWN_TYPE);
		break;
	}
}

void command_handler_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct safety_rx_frame frame;

	while (1) {
		/* 阻塞等待 RX 狀態機投遞的已驗證 frame。 */
		if (k_msgq_get(&g_rx_msgq, &frame, K_FOREVER) != 0) {
			continue;
		}

		/* 此 task 只處理 COMMAND；其它型別非預期 (host->fw 應只送命令)。 */
		if (frame.hdr.type == SAFETY_FRAME_COMMAND) {
			handle_command(&frame);
		} else {
			send_nack(frame.hdr.sequence_id,
				  SAFETY_NACK_UNKNOWN_TYPE);
		}
	}
}
