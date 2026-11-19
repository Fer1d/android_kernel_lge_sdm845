// SPDX-License-Identifier: GPL-2.0
/* Included by zram_drv.c: private policy frontend to the shared B engine. */

/* init_lock and policy_lock held, in that order. No scans and no BIOs. */
static void zram_policy_sample(struct zram *zram, bool force)
{
	bool initialized = !!zram->disksize;
	bool backing = !!zram->backing_dev;
	bool active = zram->demand_active;
	u64 bytes = initialized ?
		(u64)zs_get_total_pages(zram->mem_pool) << PAGE_SHIFT : 0;

	if (!zram->policy_enabled || !initialized || !backing)
		active = false;
	else if (bytes >= (u64)zram->policy_high_mib << 20)
		active = true;
	else if (bytes <= (u64)zram->policy_low_mib << 20)
		active = false;
	force |= active != zram->demand_active ||
		 initialized != zram->demand_initialized ||
		 backing != zram->demand_backing ||
		 zram->wb_instance != zram->demand_instance ||
		 zram->wb_generation != zram->demand_generation;
	zram->demand_bytes = bytes;
	zram->demand_active = active;
	zram->demand_initialized = initialized;
	zram->demand_backing = backing;
	zram->demand_instance = zram->wb_instance;
	zram->demand_generation = zram->wb_generation;
	if (force) {
		if (!++zram->demand_sequence)
			++zram->demand_sequence;
		sysfs_notify(&disk_to_dev(zram->disk)->kobj, NULL, "pool_demand");
	}
}

static void zram_policy_work(struct work_struct *work)
{
	struct zram *zram = container_of(work, struct zram, policy_work);

	down_read(&zram->init_lock);
	mutex_lock(&zram->policy_lock);
	zram_policy_sample(zram, false);
	if (zram->demand_active && !zram->policy_removing)
		schedule_delayed_work(&zram->policy_age_work,
			min_t(u64, (u64)zram->policy_age_seconds * HZ, MAX_JIFFY_OFFSET));
	else
		cancel_delayed_work(&zram->policy_age_work);
	mutex_unlock(&zram->policy_lock);
	up_read(&zram->init_lock);
}

static void zram_policy_age_work(struct work_struct *work)
{
	struct zram *zram = container_of(to_delayed_work(work),
				       struct zram, policy_age_work);

	down_read(&zram->init_lock);
	mutex_lock(&zram->policy_lock);
	/* An active demand needs a bounded aging retry even without new writes. */
	zram_policy_sample(zram, zram->demand_active);
	if (zram->demand_active && !zram->policy_removing)
		schedule_delayed_work(&zram->policy_age_work,
			min_t(u64, (u64)zram->policy_age_seconds * HZ, MAX_JIFFY_OFFSET));
	mutex_unlock(&zram->policy_lock);
	up_read(&zram->init_lock);
}

/* Hot paths may hold slot spinlocks. Coalesce, do not postpone queued work. */
static void zram_policy_event(struct zram *zram)
{
	if (zram->disk && READ_ONCE(zram->policy_enabled) &&
	    !READ_ONCE(zram->policy_removing))
		schedule_work(&zram->policy_work);
}

static void zram_policy_init(struct zram *zram)
{
	mutex_init(&zram->policy_lock);
	INIT_WORK(&zram->policy_work, zram_policy_work);
	INIT_DELAYED_WORK(&zram->policy_age_work, zram_policy_age_work);
	zram->policy_low_mib = 512;
	zram->policy_high_mib = 768;
	zram->policy_age_seconds = 60;
	zram->demand_sequence = 1;
}

static ssize_t pool_policy_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t n;

	mutex_lock(&zram->policy_lock);
	n = scnprintf(buf, PAGE_SIZE, "%u %u %u %u\n", zram->policy_enabled,
		zram->policy_low_mib, zram->policy_high_mib,
		zram->policy_age_seconds);
	mutex_unlock(&zram->policy_lock);
	return n;
}

static ssize_t pool_policy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned int enabled, low, high, age;
	char value[4][16], extra;

	if (sscanf(buf, "%15s %15s %15s %15s %c", value[0], value[1],
		   value[2], value[3], &extra) != 4 ||
	    kstrtouint(value[0], 10, &enabled) ||
	    kstrtouint(value[1], 10, &low) ||
	    kstrtouint(value[2], 10, &high) ||
	    kstrtouint(value[3], 10, &age) ||
	    enabled > 1 || !low || low >= high || !age)
		return -EINVAL;
	down_read(&zram->init_lock);
	mutex_lock(&zram->policy_lock);
	if (zram->policy_removing) {
		mutex_unlock(&zram->policy_lock);
		up_read(&zram->init_lock);
		return -ENODEV;
	}
	zram->policy_enabled = enabled;
	zram->policy_low_mib = low;
	zram->policy_high_mib = high;
	zram->policy_age_seconds = age;
	zram_policy_sample(zram, true);
	if (enabled)
		zram_policy_event(zram);
	else
		cancel_delayed_work(&zram->policy_age_work);
	mutex_unlock(&zram->policy_lock);
	up_read(&zram->init_lock);
	return len;
}

static ssize_t pool_demand_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t n;

	down_read(&zram->init_lock);
	mutex_lock(&zram->policy_lock);
	zram_policy_sample(zram, false);
	n = scnprintf(buf, PAGE_SIZE,
		"sequence %llu instance %llu generation %llu enabled %u initialized %u backing %u active %u physical_pool_bytes %llu low_mib %u high_mib %u min_age_seconds %u\n",
		zram->demand_sequence, zram->demand_instance, zram->demand_generation,
		zram->policy_enabled, zram->demand_initialized, zram->demand_backing,
		zram->demand_active, zram->demand_bytes, zram->policy_low_mib,
		zram->policy_high_mib, zram->policy_age_seconds);
	mutex_unlock(&zram->policy_lock);
	up_read(&zram->init_lock);
	return n;
}

static DEVICE_ATTR(pool_policy, 0600, pool_policy_show, pool_policy_store);
static DEVICE_ATTR(pool_demand, 0400, pool_demand_show, NULL);

static int zram_wb_policy_validate(const struct zram_wb_policy_exec *p)
{
	const struct zram_wb_exec *r = &p->exec;

	if (r->size != sizeof(*p))
		return -EINVAL;
	if (r->version != ZRAM_WB_POLICY_ABI_VERSION)
		return -EPROTONOSUPPORT;
	if (r->flags || r->mode || r->page_index || r->idle_count ||
	    !r->request_id || !p->demand_sequence || r->reserved0 ||
	    memchr_inv(r->reserved, 0, sizeof(r->reserved)) ||
	    memchr_inv(&r->scanned_pages, 0,
		       sizeof(*r) - offsetof(struct zram_wb_exec, scanned_pages)) ||
	    p->next_index || memchr_inv(p->reserved, 0, sizeof(p->reserved)) ||
	    !r->max_installed_pages ||
	    r->max_installed_pages > r->max_attempt_pages ||
	    r->max_attempt_pages > p->max_scanned_pages ||
	    p->max_scanned_pages == U64_MAX || !p->min_age_ns ||
	    p->min_age_ns > S64_MAX || p->start_index > U32_MAX)
		return -EINVAL;
	return 0;
}

static int zram_wb_policy_ioctl(struct zram *zram, void __user *argp)
{
	struct zram_wb_policy_exec p;
	struct zram_wb_exec *r = &p.exec;
	u64 start;
	int err;

	BUILD_BUG_ON(sizeof(p) != 224);
	if (copy_from_user(&p, argp, sizeof(p)))
		return -EFAULT;
	err = zram_wb_policy_validate(&p);
	if (err)
		return err;
	start = ktime_get_ns();
	p.next_index = p.start_index;
	down_read(&zram->init_lock);
	mutex_lock(&zram->policy_lock);
	zram_policy_sample(zram, false);
	if (r->expected_instance != zram->wb_instance ||
	    r->expected_generation != zram->wb_generation ||
	    p.demand_sequence != zram->demand_sequence)
		zram_wb_stop(r, ZRAM_WB_STOP_STALE, -ESTALE, ZRAM_WB_STAGE_NONE);
	else if (!zram->demand_active)
		zram_wb_stop(r, ZRAM_WB_STOP_NOT_READY, -EAGAIN, ZRAM_WB_STAGE_NONE);
	else if (p.min_age_ns < (u64)zram->policy_age_seconds * NSEC_PER_SEC ||
		 p.start_index >= zram->disksize >> PAGE_SHIFT)
		zram_wb_stop(r, ZRAM_WB_STOP_ERROR, -EINVAL, ZRAM_WB_STAGE_NONE);
	else
		zram_writeback_engine_policy(zram, r, &p);
	zram_policy_sample(zram, false);
	mutex_unlock(&zram->policy_lock);
	up_read(&zram->init_lock);
	r->duration_ns = ktime_get_ns() - start;
	return copy_to_user(argp, &p, sizeof(p)) ? -EFAULT : 0;
}
