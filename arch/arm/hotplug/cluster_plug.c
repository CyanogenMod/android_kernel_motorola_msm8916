/*
 * Cluster-plug CPU Hotplug Driver
 * Designed for homogeneous ARM big.LITTLE systems
 *
 * Copyright (C) 2015-2016 Sultan Qasim Khan
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/cpufreq.h>

//#define DEBUG_CLUSTER_PLUG
#undef DEBUG_CLUSTER_PLUG

#define CLUSTER_PLUG_MAJOR_VERSION	2
#define CLUSTER_PLUG_MINOR_VERSION	0

#define DEF_HYSTERESIS			(10)
#define DEF_LOAD_THRESH			(70)
#define DEF_SAMPLING_MS			(200)
#define N_BIG_CPUS			(4)

static DEFINE_MUTEX(cluster_plug_mutex);
static struct delayed_work cluster_plug_work;
static struct workqueue_struct *clusterplug_wq;

static bool cluster_plug_active = false;
static bool low_power_mode = false;

static unsigned int hysteresis = DEF_HYSTERESIS;
module_param(hysteresis, uint, 0664);

static unsigned int load_threshold = DEF_LOAD_THRESH;
module_param(load_threshold, uint, 0664);

static unsigned int sampling_time = DEF_SAMPLING_MS;
module_param(sampling_time, uint, 0664);

static unsigned int cur_hysteresis = DEF_HYSTERESIS;

struct cp_cpu_info {
	u64 prev_cpu_wall;
	u64 prev_cpu_idle;
};

static DEFINE_PER_CPU(struct cp_cpu_info, cp_info);

static inline bool is_big(int cpu) {
	return cpu < N_BIG_CPUS;
}

static unsigned int calculate_loaded_cpus(void)
{
	unsigned int cpu;
	unsigned int loaded_cpus = 0;
	struct cp_cpu_info *l_cp_info;

	for_each_online_cpu(cpu) {
		u64 cur_wall_time, cur_idle_time;
		unsigned int wall_time, idle_time;
		unsigned int cpu_load;
		l_cp_info = &per_cpu(cp_info, cpu);

		/* last parameter 0 means that IO wait is considered idle */
		cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, 0);

		wall_time = (unsigned int)
			(cur_wall_time - l_cp_info->prev_cpu_wall);
		l_cp_info->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int)
			(cur_idle_time - l_cp_info->prev_cpu_idle);
		l_cp_info->prev_cpu_idle = cur_idle_time;

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		cpu_load = 100 * (wall_time - idle_time) / wall_time;

		if (cpu_load > load_threshold)
			loaded_cpus += 1;
	}

	return loaded_cpus;
}

static void __ref plug_clusters(bool big, bool little)
{
	unsigned int cpu;
	int ret;
	bool no_offline = false;

#ifdef DEBUG_CLUSTER_PLUG
	pr_info("plugging big.LITTLE: %i %i\n", (int)big, (int)little);
#endif

	/* We will first online cores, then offline, to avoid situations where
	 * the entire first cluster is offlined before we activate the second one.
	 */
	for_each_present_cpu(cpu) {
		if ((is_big(cpu) && big) || (!is_big(cpu) && little)) {
			if (unlikely(!cpu_online(cpu))) {
				ret = cpu_up(cpu);

				/* PowerHAL or thermal throttling are interfering.
				 * Don't offline cores to avoid a situation with
				 * no cores online. Also bring up little cores.
				 */
				if (ret == -EPERM) {
					no_offline = true;
					little = true;
				}
			}
		}
	}

	if (unlikely(no_offline))
		return;

	for_each_online_cpu(cpu) {
		if ((is_big(cpu) && !big) || (!is_big(cpu) && !little)) {
			cpu_down(cpu);
		}
	}
}

static void __ref cluster_plug_work_fn(struct work_struct *work)
{
	if (cluster_plug_active) {
		unsigned int loaded_cpus;

		loaded_cpus = calculate_loaded_cpus();
#ifdef DEBUG_CLUSTER_PLUG
		pr_info("loaded_cpus: %u\n", loaded_cpus);
#endif

		if (loaded_cpus >= N_BIG_CPUS - 1) {
			cur_hysteresis = hysteresis;
			plug_clusters(true, true);
		} else if (cur_hysteresis > 0) {
			cur_hysteresis -= 1;
		} else {
			plug_clusters(true, false);
		}

		mutex_lock(&cluster_plug_mutex);
		queue_delayed_work(clusterplug_wq, &cluster_plug_work,
			msecs_to_jiffies(sampling_time));
		mutex_unlock(&cluster_plug_mutex);
	}
}

static int __ref active_show(char *buf,
		       const struct kernel_param *kp __attribute__ ((unused)))
{
	return sprintf(buf, "%d",(int)cluster_plug_active);
}

static int __ref active_store(const char *buf,
			const struct kernel_param *kp __attribute__ ((unused)))
{
	int r, val;
	bool active;

	r = kstrtoint(buf, 0, &val);
	if (r)
		return -EINVAL;
	active = val ? true : false;

	if (active == cluster_plug_active)
		return 0;

	cluster_plug_active = active;

	mutex_lock(&cluster_plug_mutex);
	cancel_delayed_work(&cluster_plug_work);
	flush_workqueue(clusterplug_wq);

	if (active) {
#ifdef DEBUG_CLUSTER_PLUG
		pr_info("activating cluster_plug\n");
#endif
		plug_clusters(true, true);
		queue_delayed_work(clusterplug_wq, &cluster_plug_work,
			msecs_to_jiffies(10));
	} else {
#ifdef DEBUG_CLUSTER_PLUG
		pr_info("disabling cluster_plug\n");
#endif
	}

	mutex_unlock(&cluster_plug_mutex);

	return 0;
}

static const struct kernel_param_ops param_ops_active = {
	.set = active_store,
	.get = active_show
};

module_param_cb(active, &param_ops_active,
		&cluster_plug_active, 0664);

static int __ref low_power_mode_show(char *buf,
		const struct kernel_param *kp __attribute__ ((unused)))
{
	return sprintf(buf, "%d", (int)low_power_mode);
}

static int __ref low_power_mode_store(const char *buf,
		const struct kernel_param *kp __attribute__ ((unused)))
{
	int r, lpm_i;
	bool lpm;

	r = kstrtoint(buf, 0, &lpm_i);
	if (r)
		return -EINVAL;

	lpm = lpm_i ? true : false;
	if (low_power_mode == lpm)
		return 0;

	low_power_mode = lpm ? true : false;

	mutex_lock(&cluster_plug_mutex);
	cancel_delayed_work(&cluster_plug_work);
	flush_workqueue(clusterplug_wq);

	plug_clusters(!low_power_mode, low_power_mode);

	if (!low_power_mode)
		queue_delayed_work(clusterplug_wq, &cluster_plug_work,
			msecs_to_jiffies(10));

	mutex_unlock(&cluster_plug_mutex);

	return 0;
}

static const struct kernel_param_ops param_ops_low_power_mode = {
	.set = low_power_mode_store,
	.get = low_power_mode_show
};

module_param_cb(low_power_mode, &param_ops_low_power_mode, &low_power_mode, 0664);

int __init cluster_plug_init(void)
{
	pr_info("cluster_plug: version %d.%d by sultanqasim\n",
		 CLUSTER_PLUG_MAJOR_VERSION,
		 CLUSTER_PLUG_MINOR_VERSION);

	clusterplug_wq = alloc_workqueue("clusterplug",
				WQ_HIGHPRI | WQ_UNBOUND, 1);
	INIT_DELAYED_WORK(&cluster_plug_work, cluster_plug_work_fn);
	queue_delayed_work(clusterplug_wq, &cluster_plug_work,
		msecs_to_jiffies(10));

	return 0;
}

MODULE_AUTHOR("Sultan Qasim Khan <sultanqasim@gmail.com>");
MODULE_DESCRIPTION("'cluster_plug' - A cluster based hotplug for homogeneous"
        "ARM big.LITTLE systems where the big cluster is preferred."
);
MODULE_LICENSE("GPL");

late_initcall(cluster_plug_init);
