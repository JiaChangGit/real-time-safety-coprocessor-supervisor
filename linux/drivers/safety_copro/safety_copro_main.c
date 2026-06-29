// SPDX-License-Identifier: GPL-2.0
/*
 * safety_copro_main.c - Safety Co-Processor Supervisor driver 進入點
 *
 * 角色：driver 的初始化/卸載骨幹。負責配置全域 device 實例、初始化 kfifo /
 * hrtimer / workqueue / per-cpu / debugfs，並以 misc device (MISC_DYNAMIC_MINOR,
 * name "safety_copro") 註冊，使 /dev/safety_copro 由 udev/devtmpfs 自動建立。
 * 同時在此「唯一」一處 #define CREATE_TRACE_POINTS 以生成 tracepoint 代碼。
 *
 * 初始化採用 goto unwind 標籤確保任何一步失敗都不會洩漏資源 (kfifo / percpu /
 * debugfs / misc)。built-in driver 仍以 module_init/module_exit 宣告 (built-in
 * 時會被展開為對應的 initcall)。
 *
 * 模組參數：
 *   hb_timeout_ms  heartbeat watchdog 逾時 (ms)，預設 350
 *   fifo_depth     RX kfifo 深度 (frame 數)，預設 64
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/miscdevice.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/moduleparam.h>

#include "safety_copro.h"

/* 在「唯一」一個編譯單元中生成 tracepoint 代碼。必須在 include trace.h 之前。 */
#define CREATE_TRACE_POINTS
#include "safety_copro_trace.h"

/* ---- 模組參數 ---- */
static unsigned int hb_timeout_ms = SAFETY_DEFAULT_HB_TIMEOUT_MS;
module_param(hb_timeout_ms, uint, 0444);
MODULE_PARM_DESC(hb_timeout_ms,
		 "Heartbeat watchdog timeout in milliseconds (default 350)");

static unsigned int fifo_depth = SAFETY_DEFAULT_FIFO_DEPTH;
module_param(fifo_depth, uint, 0444);
MODULE_PARM_DESC(fifo_depth,
		 "RX kfifo depth in frames (rounded up to power of two, default 64)");

/* 全域唯一 device 實例。 */
struct safety_copro_dev *g_safety_dev;

/* link state 文字對照 (debugfs / log 用)。 */
const char *safety_link_state_name(enum safety_link_state st)
{
	switch (st) {
	case SAFETY_LINK_BOOTING:    return "BOOTING";
	case SAFETY_LINK_HB_OK:      return "HB_OK";
	case SAFETY_LINK_HB_TIMEOUT: return "HB_TIMEOUT";
	case SAFETY_LINK_RECOVERING: return "RECOVERING";
	default:                     return "UNKNOWN";
	}
}

/* monotonic uptime (ms)，給 driver 合成 frame 的 timestamp_ms 欄位使用。 */
u32 safety_now_ms(void)
{
	return (u32)ktime_to_ms(ktime_get());
}

static int __init safety_copro_init(void)
{
	struct safety_copro_dev *dev;
	unsigned int depth;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	spin_lock_init(&dev->lock);
	init_waitqueue_head(&dev->rx_wq);
	init_waitqueue_head(&dev->tx_wq);
	dev->state = SAFETY_LINK_BOOTING;

	/* 套用模組參數 (做基本健全性檢查) */
	dev->hb_timeout_ms = hb_timeout_ms ? hb_timeout_ms :
					     SAFETY_DEFAULT_HB_TIMEOUT_MS;

	/* kfifo 深度需為 2 的次方；不足者向上取整 */
	depth = fifo_depth ? fifo_depth : SAFETY_DEFAULT_FIFO_DEPTH;
	depth = roundup_pow_of_two(depth);
	dev->fifo_depth = depth;

	/* 動態配置物件型 kfifo (元素為 struct safety_fifo_slot) */
	ret = kfifo_alloc(&dev->rx_fifo, depth, GFP_KERNEL);
	if (ret) {
		pr_err("safety_copro: kfifo_alloc failed (%d)\n", ret);
		goto err_free_dev;
	}
	ret = kfifo_alloc(&dev->tx_fifo, depth, GFP_KERNEL);
	if (ret) {
		pr_err("safety_copro: tx kfifo_alloc failed (%d)\n", ret);
		goto err_free_rx_fifo;
	}

	ret = safety_percpu_init(dev);
	if (ret) {
		pr_err("safety_copro: percpu init failed (%d)\n", ret);
		goto err_free_tx_fifo;
	}

	safety_timer_init(dev);
	safety_recovery_init(dev);

	/* 設定 misc device，註冊後 /dev/safety_copro 自動建立 */
	dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	dev->miscdev.name  = "safety_copro";
	dev->miscdev.fops  = &safety_copro_fops;

	/* g_safety_dev 必須在 misc_register 之前可見：open() 會立即用到 */
	g_safety_dev = dev;

	ret = misc_register(&dev->miscdev);
	if (ret) {
		pr_err("safety_copro: misc_register failed (%d)\n", ret);
		goto err_destroy_wq;
	}

	/* debugfs 非致命，失敗只記錄不中止 */
	safety_debugfs_init(dev);

	pr_info("safety_copro: ready (hb_timeout_ms=%u fifo_depth=%u, /dev/safety_copro minor=%d)\n",
		dev->hb_timeout_ms, dev->fifo_depth, dev->miscdev.minor);
	return 0;

err_destroy_wq:
	g_safety_dev = NULL;
	safety_recovery_destroy(dev);
	safety_timer_destroy(dev);
	safety_percpu_destroy(dev);
err_free_tx_fifo:
	kfifo_free(&dev->tx_fifo);
err_free_rx_fifo:
	kfifo_free(&dev->rx_fifo);
err_free_dev:
	kfree(dev);
	return ret;
}

static void __exit safety_copro_exit(void)
{
	struct safety_copro_dev *dev = g_safety_dev;

	if (!dev)
		return;

	/*
	 * 先移 misc device，阻止新的 open()/read()/write() 進入；之後再拆卸
	 * 計時器與 work，確保沒有 callback 會再觸碰已釋放的結構。
	 */
	safety_debugfs_destroy(dev);
	misc_deregister(&dev->miscdev);

	safety_timer_destroy(dev);      /* hrtimer_cancel：等待在途 callback */
	safety_recovery_destroy(dev);   /* cancel_work_sync：等待在途 work */

	safety_percpu_destroy(dev);
	kfifo_free(&dev->tx_fifo);
	kfifo_free(&dev->rx_fifo);

	g_safety_dev = NULL;
	kfree(dev);

	pr_info("safety_copro: removed\n");
}

module_init(safety_copro_init);
module_exit(safety_copro_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Real-Time Safety Co-Processor Supervisor Project");
MODULE_DESCRIPTION("Built-in character driver: kernel-side safety co-processor monitor");
