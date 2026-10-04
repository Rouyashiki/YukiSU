#include <linux/anon_inodes.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "internal.h"

struct yz_exit_history_reader {
	struct mutex lock;
	u64 epoch;
	u64 sequence;
	u64 coverage_generation;
	bool initial;
};

struct yz_exit_history_batch {
	struct yz_exit_history_header header;
	struct yz_exit_history_record records[];
};

static DEFINE_SPINLOCK(yz_history_lock);
static DECLARE_WAIT_QUEUE_HEAD(yz_history_wait);
static struct yz_exit_history_record yz_history[YZ_EXIT_HISTORY_MAX];
static u64 yz_history_epoch;
static u64 yz_history_sequence;
static u64 yz_history_coverage;
static u32 yz_history_count;
static bool yz_history_active;
static bool yz_history_closed = true;

int ksu_yukizygisk_get_health(void __user *arg)
{
	struct yz_health_query_cmd cmd = {};
	unsigned long flags;

	if (copy_from_user(
		&cmd, arg,
		offsetof(struct yz_health_query_cmd, sample_begin_boottime)))
		return -EFAULT;
	if (cmd.version != YZ_HEALTH_VERSION || cmd.size != sizeof(cmd) ||
	    cmd.flags || cmd.reserved)
		return -EINVAL;
	cmd.sample_begin_boottime = ktime_get_boottime_ns();
	spin_lock_irqsave(&yz_history_lock, flags);
	cmd.history.epoch = yz_history_epoch;
	cmd.history.oldest_sequence =
	    yz_history_count ? yz_history_sequence - yz_history_count + 1 : 0;
	cmd.history.newest_sequence = yz_history_sequence;
	cmd.history.coverage_generation = yz_history_coverage;
	cmd.history.count = yz_history_count;
	cmd.history.observer_active = yz_history_active;
	spin_unlock_irqrestore(&yz_history_lock, flags);
	yz_load_policy_fill_health(&cmd.policy, &cmd.cleanup);
	cmd.sample_end_boottime = ktime_get_boottime_ns();
	return copy_to_user(arg, &cmd, sizeof(cmd)) ? -EFAULT : 0;
}

void yz_exit_history_wake_readers(void)
{
	wake_up_interruptible_poll(&yz_history_wait,
				   EPOLLIN | EPOLLRDNORM | EPOLLHUP);
}

static void yz_history_wake_fn(struct work_struct *work)
{
	(void)work;
	yz_exit_history_wake_readers();
}

static DECLARE_WORK(yz_history_wake_work, yz_history_wake_fn);

void yz_exit_history_flush(void)
{
	flush_work(&yz_history_wake_work);
}

void yz_exit_history_init(void)
{
	u64 epoch = get_random_u64();
	unsigned long flags;

	spin_lock_irqsave(&yz_history_lock, flags);
	yz_history_epoch = epoch ? epoch : 1;
	yz_history_sequence = 0;
	yz_history_count = 0;
	yz_history_coverage = 1;
	yz_history_active = false;
	yz_history_closed = false;
	spin_unlock_irqrestore(&yz_history_lock, flags);
}

void yz_exit_history_exit(void)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_history_lock, flags);
	yz_history_closed = true;
	yz_history_active = false;
	yz_history_coverage++;
	spin_unlock_irqrestore(&yz_history_lock, flags);
	cancel_work_sync(&yz_history_wake_work);
	yz_exit_history_wake_readers();
}

void yz_exit_history_set_active(bool active)
{
	unsigned long flags;
	bool changed;

	spin_lock_irqsave(&yz_history_lock, flags);
	changed = !yz_history_closed && yz_history_active != active;
	if (changed) {
		yz_history_active = active;
		yz_history_coverage++;
	}
	spin_unlock_irqrestore(&yz_history_lock, flags);
	if (changed)
		yz_exit_history_wake_readers();
}

void yz_exit_history_append(const struct yz_runtime_record *record,
			    u64 start_boottime, u64 observed_boottime,
			    u32 status)
{
	struct yz_exit_history_record *entry;
	unsigned long flags;

	spin_lock_irqsave(&yz_history_lock, flags);
	if (yz_history_closed)
		goto out;
	if (++yz_history_sequence == 0) {
		if (++yz_history_epoch == 0)
			yz_history_epoch = 1;
		yz_history_sequence = 1;
		yz_history_count = 0;
	}
	entry = &yz_history[(yz_history_sequence - 1) % YZ_EXIT_HISTORY_MAX];
	memset(entry, 0, sizeof(*entry));
	entry->sequence = yz_history_sequence;
	entry->event.event.type = YZ_EV_TARGET_EXIT;
	entry->event.event.pid = record->pid;
	entry->event.event.appid = status;
	entry->event.generation = record->generation;
	entry->event.start_boottime = start_boottime;
	entry->event.observed_boottime = observed_boottime;
	entry->event.abi = record->abi;
	entry->event.kind = record->kind;
	entry->runtime_flags = record->flags;
	entry->injection_state =
	    (record->flags & YZ_RUNTIME_F_INJECTION_STATE_MASK) >>
	    YZ_RUNTIME_F_INJECTION_STATE_SHIFT;
	entry->target_type = record->target_type;
	memcpy(entry->process, record->process, sizeof(entry->process));
	memcpy(entry->target, record->target, sizeof(entry->target));
	if (yz_history_count < YZ_EXIT_HISTORY_MAX)
		yz_history_count++;
	/* Reconciliation may append while the exit observer is disabled. */
	schedule_work(&yz_history_wake_work);
out:
	spin_unlock_irqrestore(&yz_history_lock, flags);
}

static bool
yz_history_readable_locked(const struct yz_exit_history_reader *reader)
{
	return reader->initial || reader->epoch != yz_history_epoch ||
	       reader->coverage_generation != yz_history_coverage ||
	       reader->sequence < yz_history_sequence;
}

static ssize_t yz_history_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct yz_exit_history_reader *reader = file->private_data;
	struct yz_exit_history_batch *batch = NULL;
	struct yz_exit_history_header *header;
	unsigned long flags;
	u64 sequence, oldest;
	u32 capacity;
	size_t size;
	ssize_t ret;

	(void)ppos;
	if (!count)
		return 0;
	if (count < sizeof(*header))
		return -EINVAL;
	capacity = min_t(size_t, YZ_EXIT_HISTORY_BATCH_MAX,
			 (count - sizeof(*header)) /
			     sizeof(struct yz_exit_history_record));
	size =
	    sizeof(*header) + capacity * sizeof(struct yz_exit_history_record);
	if (mutex_lock_interruptible(&reader->lock))
		return -ERESTARTSYS;
	spin_lock_irqsave(&yz_history_lock, flags);
	if (!yz_history_readable_locked(reader)) {
		ret = yz_history_closed ? 0 : -EAGAIN;
		goto unlock_history;
	}
	spin_unlock_irqrestore(&yz_history_lock, flags);
	batch = kvzalloc(size, GFP_KERNEL);
	if (!batch) {
		ret = -ENOMEM;
		goto unlock_reader;
	}
	header = &batch->header;
	spin_lock_irqsave(&yz_history_lock, flags);
	if (!yz_history_readable_locked(reader)) {
		ret = yz_history_closed ? 0 : -EAGAIN;
		goto unlock_history;
	}
	header->version = YZ_EXIT_HISTORY_VERSION;
	header->record_size = sizeof(struct yz_exit_history_record);
	header->epoch = yz_history_epoch;
	header->newest_sequence = yz_history_sequence;
	oldest =
	    yz_history_count ? yz_history_sequence - yz_history_count + 1 : 0;
	header->oldest_sequence = oldest;
	header->coverage_generation = yz_history_coverage;
	header->observer_active = yz_history_active;
	sequence = reader->sequence;
	if (reader->epoch != yz_history_epoch) {
		if (reader->epoch)
			header->flags |= YZ_EXIT_HISTORY_F_RESET;
		sequence = 0;
	}
	if (oldest && sequence < oldest - 1) {
		header->flags |= YZ_EXIT_HISTORY_F_OVERFLOW;
		header->lost_first = sequence + 1;
		header->lost_last = oldest - 1;
		sequence = oldest - 1;
	}
	if (sequence < yz_history_sequence && !capacity) {
		ret = -EINVAL;
		goto unlock_history;
	}
	while (sequence < yz_history_sequence && header->count < capacity) {
		batch->records[header->count++] =
		    yz_history[sequence % YZ_EXIT_HISTORY_MAX];
		sequence++;
	}
	header->next_sequence = sequence;
	spin_unlock_irqrestore(&yz_history_lock, flags);
	size = sizeof(*header) + header->count * sizeof(batch->records[0]);
	if (copy_to_user(buf, batch, size)) {
		ret = -EFAULT;
		goto unlock_reader;
	}
	spin_lock_irqsave(&yz_history_lock, flags);
	reader->epoch = header->epoch;
	reader->sequence = header->next_sequence;
	reader->coverage_generation = header->coverage_generation;
	reader->initial = false;
	ret = size;
unlock_history:
	spin_unlock_irqrestore(&yz_history_lock, flags);
unlock_reader:
	mutex_unlock(&reader->lock);
	kvfree(batch);
	return ret;
}

static __poll_t yz_history_poll(struct file *file, poll_table *wait)
{
	struct yz_exit_history_reader *reader = file->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(file, &yz_history_wait, wait);
	spin_lock_irqsave(&yz_history_lock, flags);
	if (yz_history_readable_locked(reader))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (yz_history_closed)
		mask |= EPOLLHUP;
	spin_unlock_irqrestore(&yz_history_lock, flags);
	return mask;
}

static int yz_history_release(struct inode *inode, struct file *file)
{
	(void)inode;
	kfree(file->private_data);
	return 0;
}

static const struct file_operations yz_history_fops = {
    .owner = THIS_MODULE,
    .read = yz_history_read,
    .poll = yz_history_poll,
    .release = yz_history_release,
};

int ksu_yukizygisk_get_exit_history_fd(void __user *arg)
{
	struct yz_exit_history_reader *reader;
	struct yz_exit_history_fd_cmd cmd;
	struct file *file;
	unsigned long flags;
	int fd, ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags || (!cmd.epoch && cmd.after_sequence))
		return -EINVAL;
	spin_lock_irqsave(&yz_history_lock, flags);
	ret = yz_history_closed ? -EPIPE : 0;
	if (!ret && cmd.epoch == yz_history_epoch &&
	    cmd.after_sequence > yz_history_sequence)
		ret = -EINVAL;
	spin_unlock_irqrestore(&yz_history_lock, flags);
	if (ret)
		return ret;
	reader = kzalloc(sizeof(*reader), GFP_KERNEL);
	if (!reader)
		return -ENOMEM;
	mutex_init(&reader->lock);
	reader->epoch = cmd.epoch;
	reader->sequence = cmd.after_sequence;
	reader->initial = true;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		kfree(reader);
		return fd;
	}
	file = anon_inode_getfile("[yukizygisk_exit]", &yz_history_fops, reader,
				  O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		kfree(reader);
		goto put_fd;
	}
	cmd.fd = fd;
	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		ret = -EFAULT;
		fput(file);
		goto put_fd;
	}
	fd_install(fd, file);
	return 0;
put_fd:
	put_unused_fd(fd);
	return ret;
}
