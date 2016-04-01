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

#ifdef CONFIG_POWERSUSPEND
#include <linux/powersuspend.h>
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

//#define DEBUG_CLUSTER_PLUG
#undef DEBUG_CLUSTER_PLUG

#define CLUSTER_PLUG_MAJOR_VERSION	2
#define CLUSTER_PLUG_MINOR_VERSION	0

#define DEF_HYSTERESIS			(10)
#define DEF_LOAD_THRESH			(70)
#define DEF_SAMPLING_MS			(200)

static DEFINE_MUTEX(cluster_plug_mutex);
static struct delayed_work cluster_plug_work;
static struct workqueue_struct *clusterplug_wq;

static unsigned int cluster_plug_active = 0;
module_param(cluster_plug_active, uint, 0664);

static unsigned int hysteresis = DEF_HYSTERESIS;
module_param(hysteresis, uint, 0664);

static unsigned int load_threshold = DEF_LOAD_THRESH;
module_param(load_threshold, uint, 0664);

static unsigned int sampling_time = DEF_SAMPLING_MS;
module_param(sampling_time, uint, 0664);

static unsigned int cur_hysteresis = DEF_HYSTERESIS;

static unsigned int prefer_big = 1;
module_param(prefer_big, uint, 0664);

static bool suspended = false;

struct cp_cpu_info {
	u64 prev_cpu_wall;
	u64 prev_cpu_idle;
};

static DEFINE_PER_CPU(struct cp_cpu_info, cp_info);

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

	/* CPUs 0-3 are big, and 4-7 are little on MSM8939.
	 * We will first online cores, then offline, to avoid situations where
	 * the entire first cluster is offlined before we activate the second one.
	 */
	for_each_present_cpu(cpu) {
		if ((cpu < 4 && big) || (cpu >= 4 && little)) {
			if (unlikely(!cpu_online(cpu))) {
				ret = cpu_up(cpu);

				/* PowerHAL or thermal throttling are interfering.
				 * Don't offline cores to avoid a situation with
				 * no cores online.
				 */
				if (ret == -EPERM)
					no_offline = true;
			}
		}
	}

	if (unlikely(no_offline))
		return;

	for_each_online_cpu(cpu) {
		if ((cpu < 4 && !big) || (cpu >= 4 && !little)) {
			cpu_down(cpu);
		}
	}
}

static void __ref cluster_plug_work_fn(struct work_struct *work)
{
	unsigned int loaded_cpus, online_cpus;

	if (cluster_plug_active) {
		online_cpus = num_online_cpus();
		loaded_cpus = calculate_loaded_cpus();
#ifdef DEBUG_CLUSTER_PLUG
		pr_info("loaded_cpus: %u\n", loaded_cpus);
#endif

		if (!suspended) {
			if (loaded_cpus >= 3) {
				cur_hysteresis = hysteresis;
				if (online_cpus <= 4) {
					plug_clusters(true, true);
				}
			} else if (cur_hysteresis > 0) {
				cur_hysteresis -= 1;
			} else {
				plug_clusters(prefer_big, !prefer_big);
			}
		}
		queue_delayed_work(clusterplug_wq, &cluster_plug_work,
			msecs_to_jiffies(sampling_time));
	} else {
		/* reduce overhead when inactive */
		queue_delayed_work(clusterplug_wq, &cluster_plug_work,
			msecs_to_jiffies(500));
	}
}

#if defined(CONFIG_POWERSUSPEND) || defined(CONFIG_HAS_EARLYSUSPEND)

#ifdef CONFIG_POWERSUSPEND
static void __ref cluster_plug_suspend(struct power_suspend *handler)
#else
static void __ref cluster_plug_suspend(struct early_suspend *handler)
#endif
{
	flush_workqueue(clusterplug_wq);

	mutex_lock(&cluster_plug_mutex);
	suspended = true;
	mutex_unlock(&cluster_plug_mutex);

	/* prefer little cluster when sleeping */
	if (cluster_plug_active)
		plug_clusters(false, true);
}

#ifdef CONFIG_POWERSUSPEND
static void __ref cluster_plug_resume(struct power_suspend *handler)
#else
static void __ref cluster_plug_resume(struct early_suspend *handler)
#endif
{
	mutex_lock(&cluster_plug_mutex);
	cur_hysteresis = hysteresis;
	suspended = false;
	mutex_unlock(&cluster_plug_mutex);

	if (cluster_plug_active)
		plug_clusters(true, true);

	queue_delayed_work_on(0, clusterplug_wq, &cluster_plug_work,
		msecs_to_jiffies(10));
}

#endif /* CONFIG_POWERSUSPEND) || CONFIG_HAS_EARLYSUSPEND */

#ifdef CONFIG_POWERSUSPEND
static struct power_suspend cluster_plug_power_suspend_driver = {
	.suspend = cluster_plug_suspend,
	.resume = cluster_plug_resume,
};
#endif  /* CONFIG_POWERSUSPEND */

#ifdef CONFIG_HAS_EARLYSUSPEND
static struct early_suspend cluster_plug_early_suspend_driver = {
        .level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
        .suspend = cluster_plug_suspend,
        .resume = cluster_plug_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */

int __init cluster_plug_init(void)
{
	pr_info("cluster_plug: version %d.%d by sultanqasim\n",
		 CLUSTER_PLUG_MAJOR_VERSION,
		 CLUSTER_PLUG_MINOR_VERSION);

#ifdef CONFIG_POWERSUSPEND
	register_power_suspend(&cluster_plug_power_suspend_driver);
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&cluster_plug_early_suspend_driver);
#endif

	clusterplug_wq = alloc_workqueue("clusterplug",
				WQ_HIGHPRI | WQ_UNBOUND, 1);
	INIT_DELAYED_WORK(&cluster_plug_work, cluster_plug_work_fn);
	queue_delayed_work_on(0, clusterplug_wq, &cluster_plug_work,
		msecs_to_jiffies(10));

	return 0;
}

MODULE_AUTHOR("Sultan Qasim Khan <sultanqasim@gmail.com>");
MODULE_DESCRIPTION("'cluster_plug' - A cluster based hotplug for homogeneous"
        "ARM big.LITTLE systems where the big cluster is preferred."
);
MODULE_LICENSE("GPL");

late_initcall(cluster_plug_init);
