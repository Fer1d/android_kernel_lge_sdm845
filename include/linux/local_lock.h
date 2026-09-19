/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Simplified non-RT local_lock for this 4.9 tree (backport of the
 * upstream interface introduced in v5.16). On !PREEMPT_RT local_lock
 * only has to disable preemption, which keeps this_cpu_ptr() data
 * private to the CPU for the duration of the critical section.
 */
#ifndef _LINUX_LOCAL_LOCK_H
#define _LINUX_LOCAL_LOCK_H

#include <linux/preempt.h>
#include <linux/irqflags.h>

typedef struct { } local_lock_t;

#define INIT_LOCAL_LOCK(lockname)		{ }
#define local_lock_init(lock)			do { } while (0)

#define local_lock(lock)			preempt_disable()
#define local_lock_irq(lock)			local_irq_disable()
#define local_lock_irqsave(lock, flags)		local_irq_save(flags)

#define local_unlock(lock)			preempt_enable()
#define local_unlock_irq(lock)			local_irq_enable()
#define local_unlock_irqrestore(lock, flags)	local_irq_restore(flags)

#endif /* _LINUX_LOCAL_LOCK_H */
