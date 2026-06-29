// SPDX-License-Identifier: GPL-2.0
/*
 * safety_copro_chrdev.c - /dev/safety_copro 的 file_operations
 *
 * 角色：實作 char device 的 read/write/poll/unlocked_ioctl/open/release。
 *
 * 資料流：
 *   write(): supervisor 寫入「一個」完整 command/ACK/NACK frame，driver 驗證後
 *            放入 TX kfifo，等待 safety-linkd 取出並送到 /dev/ttyAMA1。
 *   read():  從 kfifo 取出一個 frame 的原始位元組回 userspace；O_NONBLOCK 時
 *            空佇列回 -EAGAIN，否則 wait_event_interruptible 阻塞等待。
 *   poll():  poll_wait 在 rx_wq；kfifo 非空回報 EPOLLIN|EPOLLRDNORM，並
 *            trace_safety_poll_wakeup。
 *   ioctl(): 見 safety_copro_ioctl.h。
 *
 * 鎖規則：copy_from_user / copy_to_user 可能睡眠，「絕不」在持 dev->lock 時
 * 呼叫；一律先複製到 kernel stack 緩衝，再進臨界區。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/string.h>

#include "safety_copro.h"
#include "safety_copro_trace.h"

/* open：單一全域裝置，將其指標掛到 file private_data 方便取用。 */
static int safety_open(struct inode *inode, struct file *filp)
{
	filp->private_data = g_safety_dev;
	return 0;
}

static int safety_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * 記錄一次 protocol 驗證失敗 (呼叫端未持 lock)。
 * 依 validate 結果分類累計，方便 debugfs 顯示。
 */
static void record_proto_error(struct safety_copro_dev *dev, int vres)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	dev->protocol_error_count++;
	dev->last_proto_err = vres;
	switch (vres) {
	case SAFETY_VALIDATE_BAD_MAGIC:
		dev->proto_err_magic++;
		break;
	case SAFETY_VALIDATE_BAD_VERSION:
		dev->proto_err_version++;
		break;
	case SAFETY_VALIDATE_BAD_LENGTH:
		dev->proto_err_length++;
		break;
	case SAFETY_VALIDATE_BAD_CHECKSUM:
		dev->proto_err_checksum++;
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&dev->lock, flags);
}

static void record_drop(struct safety_copro_dev *dev, u16 seq, u8 type)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	dev->dropped_frame_count++;
	spin_unlock_irqrestore(&dev->lock, flags);
	safety_percpu_inc_dropped(dev);
	trace_safety_frame_dropped(seq, type);
}

/*
 * linkd RX path：把 UART 收到的一個完整 frame 推進 RX queue。
 * checksum 錯誤會被記錄，但仍入列交給 supervisor 產生 NACK；magic/version/length
 * 錯誤則無法安全界定 frame，直接拒絕。
 */
static int queue_rx_from_linkd(struct safety_copro_dev *dev,
			       const u8 *frame, size_t count)
{
	struct SafetyFrameHeader hdr;
	size_t expected;
	int vres, rc;
	u16 seq;
	u8 type;

	if (count < SAFETY_HEADER_SIZE)
		return -EINVAL;
	if (count > SAFETY_MAX_FRAME_SIZE)
		return -EMSGSIZE;

	memcpy(&hdr, frame, SAFETY_HEADER_SIZE);
	seq = hdr.sequence_id;
	type = hdr.type;

	if (hdr.magic != SAFETY_PROTO_MAGIC) {
		record_proto_error(dev, SAFETY_VALIDATE_BAD_MAGIC);
		return -EINVAL;
	}
	if (hdr.version != SAFETY_PROTO_VERSION) {
		record_proto_error(dev, SAFETY_VALIDATE_BAD_VERSION);
		return -EINVAL;
	}
	if (hdr.payload_length > SAFETY_MAX_PAYLOAD) {
		record_proto_error(dev, SAFETY_VALIDATE_BAD_LENGTH);
		return -EINVAL;
	}
	expected = SAFETY_HEADER_SIZE + hdr.payload_length;
	if (count != expected) {
		record_proto_error(dev, SAFETY_VALIDATE_BAD_LENGTH);
		return -EINVAL;
	}

	vres = safety_frame_validate(frame, count);
	if (vres != SAFETY_VALIDATE_OK &&
	    vres != SAFETY_VALIDATE_BAD_CHECKSUM) {
		record_proto_error(dev, vres);
		return -EINVAL;
	}
	if (vres == SAFETY_VALIDATE_BAD_CHECKSUM)
		record_proto_error(dev, vres);

	rc = safety_fifo_push_frame(dev, frame, (u16)count, seq, type,
				    (type == SAFETY_FRAME_HEARTBEAT) ?
				    SAFETY_TL_HEARTBEAT : SAFETY_TL_FRAME_RX);
	if (rc == -ENOSPC) {
		record_drop(dev, seq, type);
		return -ENOSPC;
	}
	if (rc < 0)
		return rc;

	if (vres == SAFETY_VALIDATE_OK && type == SAFETY_FRAME_HEARTBEAT) {
		unsigned long flags;

		spin_lock_irqsave(&dev->lock, flags);
		dev->heartbeat_count++;
		dev->state = SAFETY_LINK_HB_OK;
		spin_unlock_irqrestore(&dev->lock, flags);
		safety_timer_rearm(dev);
	}

	return 0;
}

/*
 * write：supervisor 輸出一個完整 frame 到 TX queue，等待 linkd 送往 UART。
 */
static ssize_t safety_write(struct file *filp, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct safety_copro_dev *dev = filp->private_data;
	u8 frame[SAFETY_MAX_FRAME_SIZE];
	struct SafetyFrameHeader hdr;
	int vres, rc;
	u16 seq;
	u8  type;

	if (count < SAFETY_HEADER_SIZE)
		return -EINVAL;
	if (count > SAFETY_MAX_FRAME_SIZE)
		return -EMSGSIZE;

	if (copy_from_user(frame, ubuf, count))
		return -EFAULT;

	/* 驗證整段 buffer (magic/version/length/checksum) */
	vres = safety_frame_validate(frame, count);
	if (vres != SAFETY_VALIDATE_OK) {
		record_proto_error(dev, vres);
		return -EINVAL;
	}

	/* 取出 header 欄位 (frame 已驗證合法) */
	memcpy(&hdr, frame, SAFETY_HEADER_SIZE);
	seq  = hdr.sequence_id;
	type = hdr.type;

	/* 嘗試放入 TX queue，讓 linkd 送到 protocol UART。 */
	rc = safety_fifo_push_tx_frame(dev, frame, (u16)count, seq, type);
	if (rc == -ENOSPC) {
		record_drop(dev, seq, type);
		return -EAGAIN;
	} else if (rc < 0) {
		return rc;
	}

	return count;
}

/*
 * read：取出一個 frame 的原始位元組。使用者 buffer 至少要能容納整個 frame。
 * O_NONBLOCK 且空 -> -EAGAIN；否則 wait_event_interruptible 阻塞。
 */
static ssize_t safety_read(struct file *filp, char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct safety_copro_dev *dev = filp->private_data;
	struct safety_fifo_slot slot;
	u16 len;

	for (;;) {
		len = safety_fifo_pop_frame(dev, &slot);
		if (len)
			break;

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* 阻塞等待，直到 kfifo 有資料或被訊號中斷 */
		if (wait_event_interruptible(dev->rx_wq,
					     safety_fifo_len(dev) > 0))
			return -ERESTARTSYS;
		/* 被喚醒後回圈重試 pop (可能被其他 reader 搶走) */
	}

	if (count < len)
		return -EINVAL;   /* userspace buffer 太小，無法承接整個 frame */

	if (copy_to_user(ubuf, slot.data, len))
		return -EFAULT;

	return len;
}

/* poll：回報 RX 可讀與 TX command 待 linkd 取走。 */
static __poll_t safety_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct safety_copro_dev *dev = filp->private_data;
	__poll_t mask = 0;

	poll_wait(filp, &dev->rx_wq, wait);
	poll_wait(filp, &dev->tx_wq, wait);

	if (safety_fifo_len(dev) > 0) {
		mask |= EPOLLIN | EPOLLRDNORM;
		trace_safety_poll_wakeup((u32)mask);
	}
	if (safety_tx_fifo_len(dev) > 0)
		mask |= EPOLLPRI;

	return mask;
}

/* ---- ioctl 子處理 ---- */

static long ioc_get_stats(struct safety_copro_dev *dev, void __user *arg)
{
	struct safety_copro_stats st;
	struct safety_copro_pcpu_sum sum;
	unsigned long flags;

	memset(&st, 0, sizeof(st));

	spin_lock_irqsave(&dev->lock, flags);
	st.current_state        = dev->state;
	st.heartbeat_count      = dev->heartbeat_count;
	st.fault_count          = dev->fault_count;
	st.dropped_frame_count  = dev->dropped_frame_count;
	st.timeout_count        = dev->timeout_count;
	st.retry_count          = dev->retry_count;
	st.protocol_error_count = dev->protocol_error_count;
	spin_unlock_irqrestore(&dev->lock, flags);

	/* rx_frames_total 取自 per-cpu 彙總 */
	safety_percpu_sum(dev, &sum);
	st.rx_frames_total = (u32)sum.rx_frames;
	st.rx_queue_depth = safety_fifo_len(dev);
	st.tx_queue_depth = safety_tx_fifo_len(dev);

	if (copy_to_user(arg, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

static long ioc_reset_stats(struct safety_copro_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	dev->heartbeat_count      = 0;
	dev->fault_count          = 0;
	dev->dropped_frame_count  = 0;
	dev->timeout_count        = 0;
	dev->retry_count          = 0;
	dev->protocol_error_count = 0;
	dev->fifo_high_watermark  = 0;
	dev->proto_err_magic      = 0;
	dev->proto_err_version    = 0;
	dev->proto_err_length     = 0;
	dev->proto_err_checksum   = 0;
	dev->last_proto_err       = SAFETY_VALIDATE_OK;
	dev->last_fault_type      = SAFETY_FAULT_NONE;
	dev->last_fault_detail    = 0;
	dev->last_fault_ts_ns     = 0;
	dev->timeline_head        = 0;
	dev->timeline_filled      = 0;
	spin_unlock_irqrestore(&dev->lock, flags);

	safety_percpu_reset(dev);
	return 0;
}

static long ioc_set_hb_timeout(struct safety_copro_dev *dev, void __user *arg)
{
	u32 ms;
	unsigned long flags;

	if (copy_from_user(&ms, arg, sizeof(ms)))
		return -EFAULT;
	if (ms == 0)
		return -EINVAL;

	spin_lock_irqsave(&dev->lock, flags);
	dev->hb_timeout_ms = ms;
	spin_unlock_irqrestore(&dev->lock, flags);

	/* 立刻以新門檻重新武裝 (若先前已 active) */
	safety_timer_rearm(dev);
	return 0;
}

static long ioc_get_state(struct safety_copro_dev *dev, void __user *arg)
{
	u32 state;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->state;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (copy_to_user(arg, &state, sizeof(state)))
		return -EFAULT;
	return 0;
}

static long ioc_inject_fault(struct safety_copro_dev *dev, void __user *arg)
{
	u32 fault_type;
	unsigned long flags;

	if (copy_from_user(&fault_type, arg, sizeof(fault_type)))
		return -EFAULT;

	spin_lock_irqsave(&dev->lock, flags);
	dev->fault_count++;
	dev->last_fault_type   = (u8)fault_type;
	dev->last_fault_detail = 0;
	dev->last_fault_ts_ns  = ktime_get_ns();
	spin_unlock_irqrestore(&dev->lock, flags);

	/* 合成 FAULT_EVENT 推入 kfifo，severity=critical(2) */
	safety_synth_fault_event(dev, (u8)fault_type, 2 /* critical */, 0);
	return 0;
}

static long ioc_push_rx_frame(struct safety_copro_dev *dev, void __user *arg)
{
	struct safety_copro_frame_io io;

	if (copy_from_user(&io, arg, sizeof(io)))
		return -EFAULT;
	if (io.len > SAFETY_MAX_FRAME_SIZE)
		return -EMSGSIZE;
	return queue_rx_from_linkd(dev, io.data, io.len);
}

static long ioc_pop_tx_frame(struct safety_copro_dev *dev, void __user *arg)
{
	struct safety_copro_frame_io io;
	struct safety_fifo_slot slot;
	u16 len;

	memset(&io, 0, sizeof(io));
	len = safety_fifo_pop_tx_frame(dev, &slot);
	if (!len)
		return -EAGAIN;

	io.len = len;
	memcpy(io.data, slot.data, len);
	if (copy_to_user(arg, &io, sizeof(io)))
		return -EFAULT;
	return 0;
}

static long safety_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct safety_copro_dev *dev = filp->private_data;
	void __user *uarg = (void __user *)arg;

	/* 基本 magic / 命令範圍檢查 */
	if (_IOC_TYPE(cmd) != SAFETY_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > SAFETY_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case SAFETY_IOC_GET_STATS:
		return ioc_get_stats(dev, uarg);
	case SAFETY_IOC_RESET_STATS:
		return ioc_reset_stats(dev);
	case SAFETY_IOC_SET_HB_TIMEOUT_MS:
		return ioc_set_hb_timeout(dev, uarg);
	case SAFETY_IOC_GET_STATE:
		return ioc_get_state(dev, uarg);
	case SAFETY_IOC_FORCE_RECOVERY:
		safety_recovery_schedule(dev);
		return 0;
	case SAFETY_IOC_INJECT_FAULT:
		return ioc_inject_fault(dev, uarg);
	case SAFETY_IOC_PUSH_RX_FRAME:
		return ioc_push_rx_frame(dev, uarg);
	case SAFETY_IOC_POP_TX_FRAME:
		return ioc_pop_tx_frame(dev, uarg);
	default:
		return -ENOTTY;
	}
}

const struct file_operations safety_copro_fops = {
	.owner          = THIS_MODULE,
	.open           = safety_open,
	.release        = safety_release,
	.read           = safety_read,
	.write          = safety_write,
	.poll           = safety_poll,
	.unlocked_ioctl = safety_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	/*
	 * 6.12 已移除 no_llseek。本裝置為串流式 (不支援 seek)，依新慣例將
	 * .llseek 留 NULL，lseek(2) 會自動失敗，毋須再指定 no_llseek。
	 */
};
