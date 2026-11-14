/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_BORE_H
#define _LINUX_SCHED_BORE_H

#include <linux/sched.h>
#include <linux/sched/sysctl.h>

#ifdef CONFIG_SCHED_BORE
void bore_fork(struct task_struct *p, struct task_struct *parent);
void bore_sleep(struct task_struct *p);
u64 bore_account_delta(struct task_struct *p, u64 runtime, u64 delta);
#endif
#endif
