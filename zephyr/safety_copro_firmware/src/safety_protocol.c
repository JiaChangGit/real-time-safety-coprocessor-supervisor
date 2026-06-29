/*
 * safety_protocol.c - firmware 端通訊協定 IO 輔助
 *
 * 角色 (非 task，無 CPU pin；在呼叫者的 context 與 UART ISR context 執行)：
 *   1. 時間源：safety_now_ms() 以 k_uptime_get() 提供單調毫秒。
 *   2. UART：取得裝置、安裝 interrupt-driven callback、RX 中斷收位元組。
 *   3. RX frame 組裝狀態機：在 ISR 內把原始位元組 hunt magic → 讀 header →
 *      讀 payload_length bytes → safety_frame_validate() → 丟進 g_rx_msgq。
 *   4. TX：safety_send_frame() 用 safety_frame_pack() 封包，整包以 polling
 *      uart_poll_out 送出 (受 mutex 保護，避免多 task 交錯)。
 *
 * UART 選擇：使用 board 的 console UART = DT_CHOSEN(zephyr_console)。
 *   在 qemu_cortex_a53 上即 PL011。FORMAL build 關閉 shell，故 console UART
 *   專供二進位協定使用，不會被 shell 文字污染。DEBUG build 開 shell 時兩者
 *   共用同一條 UART（僅供人工注入故障，不跑正式 host 協定）。
 *
 * 設計取捨：
 *   - RX 走 interrupt-driven (CONFIG_UART_INTERRUPT_DRIVEN)：低延遲收命令。
 *   - TX 走 polling uart_poll_out：frame 很短 (<= 32 bytes)，實作簡單且
 *     不需 TX FIFO 中斷狀態機；以 mutex 保證整包原子送出。
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include "safety_app.h"
#include "safety_protocol.h"

/* console UART = 二進位協定通道 (見檔頭說明) */
static const struct device *const uart_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* ===================================================================
 *  時間源
 * =================================================================== */
uint32_t safety_now_ms(void)
{
	/* k_uptime_get() 回傳 int64_t ms，截成 32-bit 給線格式 (約 49.7 天回繞)。 */
	return (uint32_t)k_uptime_get();
}

/* ===================================================================
 *  TX
 * =================================================================== */
static K_MUTEX_DEFINE(tx_mutex);          /* 保護整包送出不交錯 */
static atomic_t tx_sequence = ATOMIC_INIT(0);

uint32_t safety_tx_seq_peek(void)
{
	return (uint32_t)atomic_get(&tx_sequence);
}

int safety_send_frame(safety_u8 type, const void *payload,
		      safety_u16 payload_len)
{
	static safety_u8 frame_buf[SAFETY_MAX_FRAME_SIZE]; /* 受 tx_mutex 保護 */
	struct SafetyFrameHeader hdr;
	size_t total;
	safety_u16 seq;

	if (payload_len > SAFETY_MAX_PAYLOAD) {
		return -EINVAL;
	}
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}

	/* sequence_id 單調遞增 (atomic_inc 回傳舊值)。 */
	seq = (safety_u16)atomic_inc(&tx_sequence);

	safety_frame_init(&hdr, type, seq, payload_len, safety_now_ms());

	k_mutex_lock(&tx_mutex, K_FOREVER);

	total = safety_frame_pack(frame_buf, &hdr, payload);
	if (total == 0) {
		k_mutex_unlock(&tx_mutex);
		return -EINVAL;
	}

	/* 整包逐 byte polling 送出；frame 很短，無阻塞顧慮。 */
	for (size_t i = 0; i < total; i++) {
		uart_poll_out(uart_dev, frame_buf[i]);
	}

	k_mutex_unlock(&tx_mutex);
	return (int)total;
}

/* ===================================================================
 *  RX：interrupt-driven + frame 組裝狀態機
 * =================================================================== */

/* 解析完成的 frame 投遞佇列：最多 8 筆待處理命令。 */
K_MSGQ_DEFINE(g_rx_msgq, sizeof(struct safety_rx_frame), 8, 4);

/* RX 狀態機階段 */
enum rx_phase {
	RX_HUNT_MAGIC_LO,   /* 找 magic 低位元組 (0x46 = 'F') */
	RX_HUNT_MAGIC_HI,   /* 找 magic 高位元組 (0x53 = 'S') */
	RX_HEADER,          /* 收剩餘 header bytes */
	RX_PAYLOAD,         /* 收 payload bytes */
};

struct rx_state {
	enum rx_phase phase;
	safety_u8 buf[SAFETY_MAX_FRAME_SIZE]; /* header + payload 暫存 */
	size_t need;        /* 當前階段還需幾個 byte */
	size_t got;         /* buf 內已累積 byte 數 */
	safety_u16 payload_len;
};

static struct rx_state rxs = {
	.phase = RX_HUNT_MAGIC_LO,
};

/* SAFETY_PROTO_MAGIC = 0x5346，little-endian 線上順序：先 0x46 再 0x53。 */
#define MAGIC_LO ((safety_u8)(SAFETY_PROTO_MAGIC & 0xFFu))        /* 0x46 'F' */
#define MAGIC_HI ((safety_u8)((SAFETY_PROTO_MAGIC >> 8) & 0xFFu)) /* 0x53 'S' */

static void rx_reset(void)
{
	rxs.phase = RX_HUNT_MAGIC_LO;
	rxs.need = 0;
	rxs.got = 0;
	rxs.payload_len = 0;
}

/* 在 ISR context 餵入單一 byte 推進狀態機。 */
static void rx_feed_byte(safety_u8 b)
{
	switch (rxs.phase) {
	case RX_HUNT_MAGIC_LO:
		if (b == MAGIC_LO) {
			rxs.buf[0] = b;
			rxs.got = 1;
			rxs.phase = RX_HUNT_MAGIC_HI;
		}
		/* 否則持續 hunt，丟棄雜訊 byte */
		break;

	case RX_HUNT_MAGIC_HI:
		if (b == MAGIC_HI) {
			rxs.buf[1] = b;
			rxs.got = 2;
			/* 已收到 magic 兩 byte，接著收剩餘 header */
			rxs.need = SAFETY_HEADER_SIZE - 2u;
			rxs.phase = RX_HEADER;
		} else if (b == MAGIC_LO) {
			/* 連續兩個 lo：把這個當新的 lo，停在 HI 階段 */
			rxs.buf[0] = b;
			rxs.got = 1;
		} else {
			rx_reset();
		}
		break;

	case RX_HEADER:
		rxs.buf[rxs.got++] = b;
		rxs.need--;
		if (rxs.need == 0) {
			/* header 收齊：取出 payload_length (offset 6, little-endian) */
			struct SafetyFrameHeader hdr;

			memcpy(&hdr, rxs.buf, SAFETY_HEADER_SIZE);
			if (hdr.payload_length > SAFETY_MAX_PAYLOAD) {
				/* 長度非法：丟棄並重新 hunt */
				rx_reset();
				break;
			}
			rxs.payload_len = hdr.payload_length;
			if (rxs.payload_len == 0) {
				goto frame_complete;
			}
			rxs.need = rxs.payload_len;
			rxs.phase = RX_PAYLOAD;
		}
		break;

	case RX_PAYLOAD:
		rxs.buf[rxs.got++] = b;
		rxs.need--;
		if (rxs.need == 0) {
			goto frame_complete;
		}
		break;
	}
	return;

frame_complete:
	/* 完整 frame 已在 rxs.buf；驗證 checksum 後投遞。 */
	if (safety_frame_validate(rxs.buf, rxs.got) == SAFETY_VALIDATE_OK) {
		struct safety_rx_frame msg;

		memcpy(&msg.hdr, rxs.buf, SAFETY_HEADER_SIZE);
		if (rxs.payload_len) {
			memcpy(msg.payload, rxs.buf + SAFETY_HEADER_SIZE,
			       rxs.payload_len);
		}
		/* ISR context：用 K_NO_WAIT；佇列滿則丟棄 (host 會重送/逾時)。 */
		(void)k_msgq_put(&g_rx_msgq, &msg, K_NO_WAIT);
	}
	/* checksum 失敗：靜默丟棄，host 端會自行偵測逾時/重送。 */
	rx_reset();
}

/* UART interrupt callback：把 RX FIFO 內的 byte 全部餵進狀態機。
 * 採 Zephyr 標準範式：update -> is_pending -> rx_ready -> fifo_read。
 * 只啟用 RX 中斷 (未啟用 TX IRQ)，故只處理 RX；無 RX 待處理即離開，
 * 避免因其它 pending 旗標造成忙迴圈。 */
static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) > 0) {
		if (uart_irq_is_pending(dev) <= 0) {
			break;
		}
		if (uart_irq_rx_ready(dev)) {
			safety_u8 byte;

			while (uart_fifo_read(dev, &byte, 1) == 1) {
				rx_feed_byte(byte);
			}
		} else {
			/* pending 但非 RX (例如 TX，本專案未用)：消耗後離開。 */
			break;
		}
	}
}

int safety_uart_init(void)
{
	int ret;

	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}

	rx_reset();

	ret = uart_irq_callback_set(uart_dev, uart_isr);
	if (ret < 0 && ret != -ENOSYS && ret != -ENOTSUP) {
		return ret;
	}

	uart_irq_rx_enable(uart_dev);
	return 0;
}
