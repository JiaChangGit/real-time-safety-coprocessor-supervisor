// SPDX-License-Identifier: GPL-2.0
/*
 * safety_copro_percpu.c - per-CPU RX 統計
 *
 * 角色：以 alloc_percpu 配置 per-CPU 計數器 (rx_frames / dropped / bytes)，
 * 在 RX 熱路徑使用 this_cpu_inc / this_cpu_add 避免跨 CPU cache line 競爭與
 * 鎖開銷；debugfs 與 ioctl 需要總和時，再以 for_each_possible_cpu 逐一累加。
 *
 * 注意：per-CPU 計數採「最終一致」語意 — 讀取彙總時不持任何鎖，可能讀到瞬間
 * 不一致的快照，但對統計顯示足夠 (避免 RX 熱路徑被讀者拖慢)。不使用 RCU。
 */

#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/gfp.h>

#include "safety_copro.h"

/* safety_percpu_init - 配置 per-CPU 結構並歸零。 */
int safety_percpu_init(struct safety_copro_dev *dev)
{
	int cpu;

	dev->pcpu = alloc_percpu(struct safety_copro_pcpu);
	if (!dev->pcpu)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct safety_copro_pcpu *p = per_cpu_ptr(dev->pcpu, cpu);

		p->rx_frames = 0;
		p->dropped   = 0;
		p->bytes     = 0;
	}
	return 0;
}

/* safety_percpu_destroy - 釋放 per-CPU 結構。 */
void safety_percpu_destroy(struct safety_copro_dev *dev)
{
	if (dev->pcpu) {
		free_percpu(dev->pcpu);
		dev->pcpu = NULL;
	}
}

/*
 * safety_percpu_inc_rx - RX 熱路徑：成功入列一個 frame。
 * 使用 this_cpu_*：本身已停用搶佔 (preempt) 對該 CPU 變數的更新為原子，
 * 可安全在 lock 外、process/IRQ context 呼叫。
 */
void safety_percpu_inc_rx(struct safety_copro_dev *dev, u32 bytes)
{
	this_cpu_inc(dev->pcpu->rx_frames);
	this_cpu_add(dev->pcpu->bytes, bytes);
}

/* safety_percpu_inc_dropped - RX 熱路徑：丟棄一個 frame。 */
void safety_percpu_inc_dropped(struct safety_copro_dev *dev)
{
	this_cpu_inc(dev->pcpu->dropped);
}

/*
 * safety_percpu_reset - RESET_STATS 時將所有 per-CPU 計數歸零。
 * 逐 CPU 直接寫入；可能與熱路徑的 this_cpu_inc 競爭，但 reset 為罕見的
 * 管理操作，最壞情況僅遺失極少數計數，可接受。
 */
void safety_percpu_reset(struct safety_copro_dev *dev)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct safety_copro_pcpu *p = per_cpu_ptr(dev->pcpu, cpu);

		p->rx_frames = 0;
		p->dropped   = 0;
		p->bytes     = 0;
	}
}

/* safety_percpu_sum - 跨 CPU 彙總 (給 debugfs / ioctl)。 */
void safety_percpu_sum(struct safety_copro_dev *dev,
		       struct safety_copro_pcpu_sum *out)
{
	int cpu;

	out->rx_frames = 0;
	out->dropped   = 0;
	out->bytes     = 0;

	for_each_possible_cpu(cpu) {
		struct safety_copro_pcpu *p = per_cpu_ptr(dev->pcpu, cpu);

		out->rx_frames += p->rx_frames;
		out->dropped   += p->dropped;
		out->bytes     += p->bytes;
	}
}
