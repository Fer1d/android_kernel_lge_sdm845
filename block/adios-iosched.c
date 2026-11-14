// SPDX-License-Identifier: GPL-2.0
/*
 * ADIOS-named legacy FIFO/batch candidate, NOT the ADIOS 3.3 algorithm.
 * Derived from Baka block/adios-iosched.c at 95970bc2195d (4.9 port),
 * attributed there to Masahito Suzuki (firelzrd), original ADIOS author.
 * Only its real per-operation FIFO/batch policy is retained. There is no
 * latency predictor, deadline enforcement or completion latency model here.
 * The unused model/knobs and sector tree have deliberately been removed.
 * Merge lookup uses the elevator core's hash/last_merge, not sector neighbours.
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>

#define ADIOS_OPTYPES 4
#define ADIOS_READ 0
#define ADIOS_WRITE 1
#define ADIOS_DISCARD 2
#define ADIOS_OTHER 3
#define MAX_BATCH_LIMIT 256

struct adios_data {
	struct request_queue *queue;
	struct list_head fifo[ADIOS_OPTYPES];
	unsigned int batch_limit[ADIOS_OPTYPES];
	unsigned int current_optype;
	unsigned int batch_count;
};

static const unsigned int default_batch_limit[ADIOS_OPTYPES] = { 32, 16, 8, 8 };

static unsigned int adios_optype(struct request *rq)
{
	switch (req_op(rq)) {
	case REQ_OP_READ:
		return ADIOS_READ;
	case REQ_OP_WRITE:
		return ADIOS_WRITE;
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		return ADIOS_DISCARD;
	default:
		/* Real flush sequencing bypasses us via ELEVATOR_INSERT_FLUSH. */
		return ADIOS_OTHER;
	}
}

/* Legacy elevator add/merge/dispatch callbacks hold q->queue_lock. */
static void adios_add_request(struct request_queue *q, struct request *rq)
{
	struct adios_data *ad = q->elevator->elevator_data;

	list_add_tail(&rq->queuelist, &ad->fifo[adios_optype(rq)]);
}

static void adios_merged_requests(struct request_queue *q, struct request *rq,
				  struct request *next)
{
	/* Core calls this only for a sorted victim; keep survivor FIFO age.
	 * Reinitialise before core frees/reuses next, including an only child.
	 * No rb_node ownership: front merges need no reindex callback.
	 */
	list_del_init(&next->queuelist);
}

static int adios_dispatch(struct request_queue *q, int force)
{
	struct adios_data *ad = q->elevator->elevator_data;
	struct request *rq;
	unsigned int op, i;

	/* force drains with the same bounded policy; never idle with work.
	 * Expiry advances BEFORE selection, even when all groups are busy.
	 * Counts are per turn (not accumulated across visits) and <= 256.
	 */
	if (ad->batch_count >= ad->batch_limit[ad->current_optype]) {
		ad->current_optype = (ad->current_optype + 1) % ADIOS_OPTYPES;
		ad->batch_count = 0;
	}
	for (i = 0; i < ADIOS_OPTYPES; i++) {
		op = ad->current_optype;
		if (!list_empty(&ad->fifo[op])) {
			rq = list_first_entry(&ad->fifo[op], struct request,
					      queuelist);
			list_del_init(&rq->queuelist);
			ad->batch_count++;
			/* Core removes merge hash/last_merge, decrements nr_sorted
			 * and links rq on q->queue_head. Exactly one owner.
			 */
			elv_dispatch_add_tail(q, rq);
			return 1;
		}
		ad->current_optype = (op + 1) % ADIOS_OPTYPES;
		ad->batch_count = 0;
	}
	return 0;
}

static int adios_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct elevator_queue *eq;
	struct adios_data *ad;
	unsigned int i;

	/* Allocate private data first: elevator_release() puts the type ref,
	 * but elevator_init() also puts it on callback failure. Never create
	 * then release an elevator object on a failing init callback.
	 * This tree's switch fail_init does NOT put the caller's type ref;
	 * until core ownership is unified, Kconfig restricts us to built-in.
	 */
	ad = kzalloc_node(sizeof(*ad), GFP_KERNEL, q->node);
	if (!ad)
		return -ENOMEM;
	eq = elevator_alloc(q, e);
	if (!eq) {
		kfree(ad);
		return -ENOMEM;
	}
	ad->queue = q;
	for (i = 0; i < ADIOS_OPTYPES; i++) {
		INIT_LIST_HEAD(&ad->fifo[i]);
		ad->batch_limit[i] = default_batch_limit[i];
	}
	eq->elevator_data = ad;
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void adios_exit_queue(struct elevator_queue *e)
{
	struct adios_data *ad = e->elevator_data;
	unsigned int i;

	/* Core must drain before switching/exiting, as for noop. Do not
	 * silently unlink and orphan requests to conceal a lifecycle bug.
	 */
	for (i = 0; i < ADIOS_OPTYPES; i++)
		BUG_ON(!list_empty(&ad->fifo[i]));
	kfree(ad);
}

static ssize_t adios_batch_show(struct elevator_queue *e, char *page,
			       unsigned int op)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long flags;
	unsigned int val;

	spin_lock_irqsave(ad->queue->queue_lock, flags);
	val = ad->batch_limit[op];
	spin_unlock_irqrestore(ad->queue->queue_lock, flags);
	return sprintf(page, "%u\n", val);
}

static ssize_t adios_batch_store(struct elevator_queue *e, const char *page,
				size_t count, unsigned int op)
{
	struct adios_data *ad = e->elevator_data;
	unsigned long flags;
	unsigned int val;
	int ret;

	/* Parse before narrowing; reject junk, overflow, zero and >256.
	 * sysfs supplies a NUL-terminated buffer; kstrtouint accepts one LF.
	 */
	ret = kstrtouint(page, 10, &val);
	if (ret)
		return ret;
	if (!val || val > MAX_BATCH_LIMIT)
		return -EINVAL;
	spin_lock_irqsave(ad->queue->queue_lock, flags);
	ad->batch_limit[op] = val;
	spin_unlock_irqrestore(ad->queue->queue_lock, flags);
	return count;
}

#define ADIOS_BATCH_ATTR(name, op) \
static ssize_t adios_batch_limit_##name##_show(struct elevator_queue *e, \
					      char *page) \
{ \
	return adios_batch_show(e, page, op); \
} \
static ssize_t adios_batch_limit_##name##_store(struct elevator_queue *e, \
				const char *page, size_t count) \
{ \
	return adios_batch_store(e, page, count, op); \
}

ADIOS_BATCH_ATTR(read, ADIOS_READ)
ADIOS_BATCH_ATTR(write, ADIOS_WRITE)
ADIOS_BATCH_ATTR(discard, ADIOS_DISCARD)
ADIOS_BATCH_ATTR(other, ADIOS_OTHER)

static ssize_t adios_version_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "legacy-fifo-batch-candidate (not ADIOS 3.3)\n");
}

static struct elv_fs_entry adios_attrs[] = {
	__ATTR(version, 0444, adios_version_show, NULL),
	__ATTR(batch_limit_read, 0644, adios_batch_limit_read_show, adios_batch_limit_read_store),
	__ATTR(batch_limit_write, 0644, adios_batch_limit_write_show, adios_batch_limit_write_store),
	__ATTR(batch_limit_discard, 0644, adios_batch_limit_discard_show, adios_batch_limit_discard_store),
	__ATTR(batch_limit_other, 0644, adios_batch_limit_other_show, adios_batch_limit_other_store),
	__ATTR_NULL
};

static struct elevator_type iosched_adios = {
	.ops = {
		/* No merge_fn: pointing it at elv_merge would recurse. */
		.elevator_merge_req_fn = adios_merged_requests,
		.elevator_dispatch_fn = adios_dispatch,
		.elevator_add_req_fn = adios_add_request,
		.elevator_init_fn = adios_init_queue,
		.elevator_exit_fn = adios_exit_queue,
	},
	.elevator_attrs = adios_attrs,
	.elevator_name = "adios",
	.elevator_owner = THIS_MODULE,
};

static int __init adios_init(void)
{
	return elv_register(&iosched_adios);
}

static void __exit adios_exit(void)
{
	elv_unregister(&iosched_adios);
}

module_init(adios_init);
module_exit(adios_exit);
MODULE_AUTHOR("Masahito Suzuki (firelzrd); Baka legacy port contributors");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ADIOS-derived legacy FIFO/batch candidate, not ADIOS 3.3");
