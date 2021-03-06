/* arch/arm/mach-msm/pm.c
 *
 * MSM Power Management Routines
 *
 * Copyright (C) 2007 Google, Inc.
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/proc_fs.h>
#include <linux/suspend.h>
#include <linux/reboot.h>
#include <mach/msm_iomap.h>
#include <mach/system.h>
#include <asm/io.h>

#include "smd_private.h"
#include "acpuclock.h"
#include "proc_comm.h"
#ifdef CONFIG_HAS_WAKELOCK
#include <linux/wakelock.h>
#endif

enum {
	MSM_PM_DEBUG_SUSPEND = 1U << 0,
	MSM_PM_DEBUG_POWER_COLLAPSE = 1U << 1,
	MSM_PM_DEBUG_STATE = 1U << 2,
	MSM_PM_DEBUG_CLOCK = 1U << 3,
	MSM_PM_DEBUG_RESET_VECTOR = 1U << 4,
	MSM_PM_DEBUG_SMSM_STATE = 1U << 5,
	MSM_PM_DEBUG_IDLE = 1U << 6,
};
static int msm_pm_debug_mask;
module_param_named(debug_mask, msm_pm_debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);

enum {
	MSM_PM_SLEEP_MODE_POWER_COLLAPSE_SUSPEND,
	MSM_PM_SLEEP_MODE_POWER_COLLAPSE,
	MSM_PM_SLEEP_MODE_APPS_SLEEP,
	MSM_PM_SLEEP_MODE_RAMP_DOWN_AND_WAIT_FOR_INTERRUPT,
	MSM_PM_SLEEP_MODE_WAIT_FOR_INTERRUPT,
};
static int msm_pm_sleep_mode = CONFIG_MSM7X00A_SLEEP_MODE;
module_param_named(sleep_mode, msm_pm_sleep_mode, int, S_IRUGO | S_IWUSR | S_IWGRP);
static int msm_pm_idle_sleep_mode = CONFIG_MSM7X00A_IDLE_SLEEP_MODE;
module_param_named(idle_sleep_mode, msm_pm_idle_sleep_mode, int, S_IRUGO | S_IWUSR | S_IWGRP);
static int msm_pm_idle_sleep_min_time = CONFIG_MSM7X00A_IDLE_SLEEP_MIN_TIME;
module_param_named(idle_sleep_min_time, msm_pm_idle_sleep_min_time, int, S_IRUGO | S_IWUSR | S_IWGRP);
static int msm_pm_idle_spin_time = CONFIG_MSM7X00A_IDLE_SPIN_TIME;
module_param_named(idle_spin_time, msm_pm_idle_spin_time, int, S_IRUGO | S_IWUSR | S_IWGRP);

#define A11S_CLK_SLEEP_EN (MSM_CSR_BASE + 0x11c)
#define A11S_PWRDOWN (MSM_CSR_BASE + 0x440)
#define A11S_STANDBY_CTL (MSM_CSR_BASE + 0x108)
#define A11RAMBACKBIAS (MSM_CSR_BASE + 0x508)

int msm_pm_collapse(void);
int msm_arch_idle(void);
void msm_pm_collapse_exit(void);

int64_t msm_timer_enter_idle(void);
void msm_timer_exit_idle(int low_power);
int msm_irq_idle_sleep_allowed(void);
int msm_irq_pending(void);

static uint32_t *msm_pm_reset_vector;

static uint32_t msm_pm_max_sleep_time;

#ifdef CONFIG_MSM_IDLE_STATS
enum msm_pm_time_stats_id {
	MSM_PM_STAT_REQUESTED_IDLE,
	MSM_PM_STAT_IDLE_SPIN,
	MSM_PM_STAT_IDLE_WFI,
	MSM_PM_STAT_IDLE_SLEEP,
	MSM_PM_STAT_IDLE_FAILED_SLEEP,
	MSM_PM_STAT_NOT_IDLE,
	MSM_PM_STAT_COUNT
};

static struct msm_pm_time_stats {
	const char *name;
	int bucket[CONFIG_MSM_IDLE_STATS_BUCKET_COUNT];
	int64_t min_time[CONFIG_MSM_IDLE_STATS_BUCKET_COUNT];
	int64_t max_time[CONFIG_MSM_IDLE_STATS_BUCKET_COUNT];
	int count;
	int64_t total_time;
} msm_pm_stats[MSM_PM_STAT_COUNT] = {
	[MSM_PM_STAT_REQUESTED_IDLE].name = "idle-request",
	[MSM_PM_STAT_IDLE_SPIN].name = "idle-spin",
	[MSM_PM_STAT_IDLE_WFI].name = "idle-wfi",
	[MSM_PM_STAT_IDLE_SLEEP].name = "idle-sleep",
	[MSM_PM_STAT_IDLE_FAILED_SLEEP].name = "idle-failed-sleep",
	[MSM_PM_STAT_NOT_IDLE].name = "not-idle",
};

static void msm_pm_add_stat(enum msm_pm_time_stats_id id, int64_t t)
{
	int i;
	int64_t bt;
	msm_pm_stats[id].total_time += t;
	msm_pm_stats[id].count++;
	bt = t;
	do_div(bt, CONFIG_MSM_IDLE_STATS_FIRST_BUCKET);
	if (bt < 1ULL << (CONFIG_MSM_IDLE_STATS_BUCKET_SHIFT *
				(CONFIG_MSM_IDLE_STATS_BUCKET_COUNT - 1)))
		i = DIV_ROUND_UP(fls((uint32_t)bt),
					CONFIG_MSM_IDLE_STATS_BUCKET_SHIFT);
	else
		i = CONFIG_MSM_IDLE_STATS_BUCKET_COUNT - 1;
	msm_pm_stats[id].bucket[i]++;
	if (t < msm_pm_stats[id].min_time[i] || !msm_pm_stats[id].max_time[i])
		msm_pm_stats[id].min_time[i] = t;
	if (t > msm_pm_stats[id].max_time[i])
		msm_pm_stats[id].max_time[i] = t;
}
#endif

static int
msm_pm_wait_state(uint32_t wait_state_all_set, uint32_t wait_state_all_clear,
                  uint32_t wait_state_any_set, uint32_t wait_state_any_clear)
{
	int i;
	uint32_t state;

	for (i = 0; i < 100000; i++) {
		state = smsm_get_state();
		if (((state & wait_state_all_set) == wait_state_all_set) &&
		    ((~state & wait_state_all_clear) == wait_state_all_clear) &&
		    (wait_state_any_set == 0 || (state & wait_state_any_set) ||
		     wait_state_any_clear == 0 || (state & wait_state_any_clear)))
			return 0;
	}
	printk(KERN_ERR "msm_pm_wait_state(%x, %x, %x, %x) failed %x\n",
	       wait_state_all_set, wait_state_all_clear,
	       wait_state_any_set, wait_state_any_clear, state);
	return -ETIMEDOUT;
}

static int msm_sleep(int sleep_mode, uint32_t sleep_delay, int from_idle)
{
	uint32_t saved_vector[2];
	int collapsed;
	void msm_irq_enter_sleep1(bool arm9_wake, int from_idle);
	int msm_irq_enter_sleep2(bool arm9_wake, int from_idle);
	void msm_irq_exit_sleep1(void);
	void msm_irq_exit_sleep2(void);
	void msm_irq_exit_sleep3(void);
	void msm_gpio_enter_sleep(int from_idle);
	void msm_gpio_exit_sleep(void);
	void smd_sleep_exit(void);
	uint32_t enter_state;
	uint32_t enter_wait_set = 0;
	uint32_t enter_wait_clear = 0;
	uint32_t exit_state;
	uint32_t exit_wait_clear = 0;
	uint32_t exit_wait_set = 0;
	unsigned long pm_saved_acpu_clk_rate = 0;
	int ret;
	int rv = -EINTR;

	if (msm_pm_debug_mask & MSM_PM_DEBUG_SUSPEND)
		printk(KERN_INFO "msm_sleep(): mode %d delay %u idle %d\n",
		       sleep_mode, sleep_delay, from_idle);

	switch (sleep_mode) {
	case MSM_PM_SLEEP_MODE_POWER_COLLAPSE:
		enter_state = SMSM_PWRC;
		enter_wait_set = SMSM_RSA;
		exit_state = SMSM_WFPI;
		exit_wait_clear = SMSM_RSA;
		break;
	case MSM_PM_SLEEP_MODE_POWER_COLLAPSE_SUSPEND:
		enter_state = SMSM_PWRC_SUSPEND;
		enter_wait_set = SMSM_RSA;
		exit_state = SMSM_WFPI;
		exit_wait_clear = SMSM_RSA;
		break;
	case MSM_PM_SLEEP_MODE_APPS_SLEEP:
		enter_state = SMSM_SLEEP;
		exit_state = SMSM_SLEEPEXIT;
		exit_wait_set = SMSM_SLEEPEXIT;
		break;
	default:
		enter_state = 0;
		exit_state = 0;
	}

	msm_irq_enter_sleep1(!!enter_state, from_idle);
	msm_gpio_enter_sleep(from_idle);

	if (enter_state) {
		if (sleep_delay == 0 && sleep_mode >= MSM_PM_SLEEP_MODE_APPS_SLEEP)
			sleep_delay = 192000*5; /* APPS_SLEEP does not allow infinite timeout */
		smsm_set_sleep_duration(sleep_delay);
		ret = smsm_change_state(SMSM_RUN, enter_state);
		if (ret) {
			printk(KERN_ERR "msm_sleep(): smsm_change_state %x failed\n", enter_state);
			enter_state = 0;
			exit_state = 0;
		}
		ret = msm_pm_wait_state(enter_wait_set, enter_wait_clear, 0, 0);
		if (ret) {
			printk(KERN_INFO "msm_sleep(): msm_pm_wait_state failed, %x\n", smsm_get_state());
			goto enter_failed;
		}
	}
	if (msm_irq_enter_sleep2(!!enter_state, from_idle))
		goto enter_failed;

	if (enter_state) {
		writel(0x1f, A11S_CLK_SLEEP_EN);
		writel(1, A11S_PWRDOWN);

		writel(0, A11S_STANDBY_CTL);
		writel(0, A11RAMBACKBIAS);

		if (msm_pm_debug_mask & MSM_PM_DEBUG_STATE)
			printk(KERN_INFO "msm_sleep(): enter "
			       "A11S_CLK_SLEEP_EN %x, A11S_PWRDOWN %x, "
			       "smsm_get_state %x\n", readl(A11S_CLK_SLEEP_EN),
			       readl(A11S_PWRDOWN), smsm_get_state());
	}

	if (sleep_mode <= MSM_PM_SLEEP_MODE_RAMP_DOWN_AND_WAIT_FOR_INTERRUPT) {
		pm_saved_acpu_clk_rate = acpuclk_power_collapse();
		if (msm_pm_debug_mask & MSM_PM_DEBUG_CLOCK)
			printk(KERN_INFO "msm_sleep(): %ld enter power collapse"
			       "\n", pm_saved_acpu_clk_rate);
		if (pm_saved_acpu_clk_rate == 0)
			goto ramp_down_failed;
	}
	if (sleep_mode < MSM_PM_SLEEP_MODE_APPS_SLEEP) {
		if (msm_pm_debug_mask & MSM_PM_DEBUG_SMSM_STATE)
			smsm_print_sleep_info();
		saved_vector[0] = msm_pm_reset_vector[0];
		saved_vector[1] = msm_pm_reset_vector[1];
		msm_pm_reset_vector[0] = 0xE51FF004; /* ldr pc, 4 */
		msm_pm_reset_vector[1] = virt_to_phys(msm_pm_collapse_exit);
		if (msm_pm_debug_mask & MSM_PM_DEBUG_RESET_VECTOR)
			printk(KERN_INFO "msm_sleep(): vector %x %x -> "
			       "%x %x\n", saved_vector[0], saved_vector[1],
			       msm_pm_reset_vector[0], msm_pm_reset_vector[1]);
		collapsed = msm_pm_collapse();
		msm_pm_reset_vector[0] = saved_vector[0];
		msm_pm_reset_vector[1] = saved_vector[1];
		if (collapsed) {
			cpu_init();
			local_fiq_enable();
			rv = 0;
		}
		if (msm_pm_debug_mask & MSM_PM_DEBUG_POWER_COLLAPSE)
			printk(KERN_INFO "msm_pm_collapse(): returned %d\n",
			       collapsed);
		if (msm_pm_debug_mask & MSM_PM_DEBUG_SMSM_STATE)
			smsm_print_sleep_info();
	} else {
		msm_arch_idle();
		rv = 0;
	}

	if (sleep_mode <= MSM_PM_SLEEP_MODE_RAMP_DOWN_AND_WAIT_FOR_INTERRUPT) {
		if (msm_pm_debug_mask & MSM_PM_DEBUG_CLOCK)
			printk(KERN_INFO "msm_sleep(): exit power collapse %ld"
			       "\n", pm_saved_acpu_clk_rate);
		if (acpuclk_set_rate(pm_saved_acpu_clk_rate, 1) < 0)
			printk(KERN_ERR "msm_sleep(): clk_set_rate %ld "
			       "failed\n", pm_saved_acpu_clk_rate);
	}
	if (msm_pm_debug_mask & MSM_PM_DEBUG_STATE)
		printk(KERN_INFO "msm_sleep(): exit A11S_CLK_SLEEP_EN %x, "
		       "A11S_PWRDOWN %x, smsm_get_state %x\n",
		       readl(A11S_CLK_SLEEP_EN), readl(A11S_PWRDOWN),
		       smsm_get_state());
ramp_down_failed:
	msm_irq_exit_sleep1();
enter_failed:
	if (enter_state) {
		writel(0x00, A11S_CLK_SLEEP_EN);
		writel(0, A11S_PWRDOWN);
		smsm_change_state(enter_state, exit_state);
		msm_pm_wait_state(exit_wait_set, exit_wait_clear, 0, 0);
		if (msm_pm_debug_mask & MSM_PM_DEBUG_STATE)
			printk(KERN_INFO "msm_sleep(): sleep exit "
			       "A11S_CLK_SLEEP_EN %x, A11S_PWRDOWN %x, "
			       "smsm_get_state %x\n", readl(A11S_CLK_SLEEP_EN),
			       readl(A11S_PWRDOWN), smsm_get_state());
		if (msm_pm_debug_mask & MSM_PM_DEBUG_SMSM_STATE)
			smsm_print_sleep_info();
	}
	msm_irq_exit_sleep2();
	if (enter_state) {
		smsm_change_state(exit_state, SMSM_RUN);
		msm_pm_wait_state(SMSM_RUN, 0, 0, 0);
		if (msm_pm_debug_mask & MSM_PM_DEBUG_STATE)
			printk(KERN_INFO "msm_sleep(): sleep exit "
			       "A11S_CLK_SLEEP_EN %x, A11S_PWRDOWN %x, "
			       "smsm_get_state %x\n", readl(A11S_CLK_SLEEP_EN),
			       readl(A11S_PWRDOWN), smsm_get_state());
	}
	msm_irq_exit_sleep3();
	msm_gpio_exit_sleep();
	smd_sleep_exit();
	return rv;
}

void arch_idle(void)
{
	int ret;
	int spin;
	int64_t sleep_time;
	int low_power = 0;
#ifdef CONFIG_MSM_IDLE_STATS
	int64_t t1;
	static int64_t t2;
	int exit_stat;
#endif
	int allow_sleep =
		msm_pm_idle_sleep_mode < MSM_PM_SLEEP_MODE_WAIT_FOR_INTERRUPT &&
#ifdef CONFIG_HAS_WAKELOCK
		!has_wake_lock(WAKE_LOCK_IDLE) &&
#endif
		msm_irq_idle_sleep_allowed();
	if (msm_pm_reset_vector == NULL)
		return;

	sleep_time = msm_timer_enter_idle();
#ifdef CONFIG_MSM_IDLE_STATS
	t1 = ktime_to_ns(ktime_get());
	msm_pm_add_stat(MSM_PM_STAT_NOT_IDLE, t1 - t2);
	msm_pm_add_stat(MSM_PM_STAT_REQUESTED_IDLE, sleep_time);
#endif
	if (msm_pm_debug_mask & MSM_PM_DEBUG_IDLE)
		printk(KERN_INFO "arch_idle: sleep time %llu, allow_sleep %d\n",
		       sleep_time, allow_sleep);
	spin = msm_pm_idle_spin_time >> 10;
	while (spin-- > 0) {
		if (msm_irq_pending()) {
#ifdef CONFIG_MSM_IDLE_STATS
			exit_stat = MSM_PM_STAT_IDLE_SPIN;
#endif
			goto abort_idle;
		}
		udelay(1);
	}
	if (sleep_time < msm_pm_idle_sleep_min_time || !allow_sleep) {
		unsigned long saved_rate;
		saved_rate = acpuclk_wait_for_irq();
		if (msm_pm_debug_mask & MSM_PM_DEBUG_CLOCK)
			printk(KERN_DEBUG "arch_idle: clk %ld -> swfi\n",
				saved_rate);
		if (saved_rate)
			msm_arch_idle();
		else
			while (!msm_irq_pending())
				udelay(1);
		if (msm_pm_debug_mask & MSM_PM_DEBUG_CLOCK)
			printk(KERN_DEBUG "msm_sleep: clk swfi -> %ld\n",
				saved_rate);
		if (saved_rate && acpuclk_set_rate(saved_rate, 1) < 0)
			printk(KERN_ERR "msm_sleep(): clk_set_rate %ld "
			       "failed\n", saved_rate);
#ifdef CONFIG_MSM_IDLE_STATS
		exit_stat = MSM_PM_STAT_IDLE_WFI;
#endif
  	} else {
		low_power = 1;
		do_div(sleep_time, NSEC_PER_SEC / 32768);
		if (sleep_time > 0x6DDD000) {
			printk("sleep_time too big %lld\n", sleep_time);
			sleep_time = 0x6DDD000;
		}
		ret = msm_sleep(msm_pm_idle_sleep_mode, sleep_time, 1);
#ifdef CONFIG_MSM_IDLE_STATS
		if (ret)
			exit_stat = MSM_PM_STAT_IDLE_FAILED_SLEEP;
		else
			exit_stat = MSM_PM_STAT_IDLE_SLEEP;
#endif
	}
abort_idle:
	msm_timer_exit_idle(low_power);
#ifdef CONFIG_MSM_IDLE_STATS
	t2 = ktime_to_ns(ktime_get());
	msm_pm_add_stat(exit_stat, t2 - t1);
#endif
}

static int msm_pm_enter(suspend_state_t state)
{
	msm_sleep(msm_pm_sleep_mode, msm_pm_max_sleep_time, 0);
	return 0;
}

static struct platform_suspend_ops msm_pm_ops = {
	.enter		= msm_pm_enter,
	.valid		= suspend_valid_only_mem,
};

static uint32_t restart_reason = 0x776655AA;

static void msm_pm_power_off(void)
{
	msm_proc_comm(PCOM_POWER_DOWN, 0, 0);
	for (;;) ;
}

static void msm_pm_restart(char str)
{
	/* If there's a hard reset hook and the restart_reason
	 * is the default, prefer that to the (slower) proc_comm
	 * reset command.
	 */
	if ((restart_reason == 0x776655AA) && msm_hw_reset_hook) {
		msm_hw_reset_hook();
	} else {
		msm_proc_comm(PCOM_RESET_CHIP, &restart_reason, 0);
	}
	for (;;) ;
}

static int msm_reboot_call(struct notifier_block *this, unsigned long code, void *_cmd)
{
	if((code == SYS_RESTART) && _cmd) {
		char *cmd = _cmd;
		if (!strcmp(cmd, "bootloader")) {
			restart_reason = 0x77665500;
		} else if (!strcmp(cmd, "recovery")) {
			restart_reason = 0x77665502;
		} else if (!strcmp(cmd, "eraseflash")) {
			restart_reason = 0x776655EF;
		} else if (!strncmp(cmd, "oem-", 4)) {
			unsigned code = simple_strtoul(cmd + 4, 0, 16) & 0xff;
			restart_reason = 0x6f656d00 | code;
		} else {
			restart_reason = 0x77665501;
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block msm_reboot_notifier =
{
	.notifier_call = msm_reboot_call,
};

#ifdef CONFIG_MSM_IDLE_STATS
static int msm_pm_read_proc(char *page, char **start, off_t off,
                               int count, int *eof, void *data)
{
	int len = 0;
	int i, j;
	char *p = page;

	for (i = 0; i < ARRAY_SIZE(msm_pm_stats); i++) {
		int64_t bucket_time;
		int64_t s;
		uint32_t ns;
		s = msm_pm_stats[i].total_time;
		ns = do_div(s, NSEC_PER_SEC);
		p += sprintf(p,
			"%s:\n"
			"  count: %7d\n"
			"  total_time: %lld.%09u\n",
			msm_pm_stats[i].name,
			msm_pm_stats[i].count,
			s, ns);
		bucket_time = CONFIG_MSM_IDLE_STATS_FIRST_BUCKET;
		for (j = 0; j < CONFIG_MSM_IDLE_STATS_BUCKET_COUNT - 1; j++) {
			s = bucket_time;
			ns = do_div(s, NSEC_PER_SEC);
			p += sprintf(p, "   <%2lld.%09u: %7d (%lld-%lld)\n",
				s, ns, msm_pm_stats[i].bucket[j],
				msm_pm_stats[i].min_time[j],
				msm_pm_stats[i].max_time[j]);
			bucket_time <<= CONFIG_MSM_IDLE_STATS_BUCKET_SHIFT;
		}
		p += sprintf(p, "  >=%2lld.%09u: %7d (%lld-%lld)\n",
			s, ns, msm_pm_stats[i].bucket[j],
			msm_pm_stats[i].min_time[j],
			msm_pm_stats[i].max_time[j]);
	}
	*start = page + off;

	len = p - page;
	if (len > off)
		len -= off;
	else
		len = 0;

	return len < count ? len  : count;
}
#endif

void msm_pm_set_max_sleep_time(int64_t max_sleep_time_ns)
{
	int64_t max_sleep_time_bs = max_sleep_time_ns;

	/* Convert from ns -> BS units */
	do_div(max_sleep_time_bs, NSEC_PER_SEC / 32768);

	if (max_sleep_time_bs > 0x6DDD000)
		msm_pm_max_sleep_time = (uint32_t) 0x6DDD000;
	else
		msm_pm_max_sleep_time = (uint32_t) max_sleep_time_bs;

	if (msm_pm_debug_mask & MSM_PM_DEBUG_SUSPEND)
		printk("%s: Requested %lldns (%lldbs), Giving %ubs\n",
		       __func__, max_sleep_time_ns, 
		       max_sleep_time_bs, 
		       msm_pm_max_sleep_time);
}
EXPORT_SYMBOL(msm_pm_set_max_sleep_time);

static int __init msm_pm_init(void)
{
	pm_power_off = msm_pm_power_off;
	arm_pm_restart = msm_pm_restart;
	msm_pm_max_sleep_time = 0;

	register_reboot_notifier(&msm_reboot_notifier);

	msm_pm_reset_vector = ioremap(0, PAGE_SIZE);
	if (msm_pm_reset_vector == NULL) {
		printk(KERN_ERR "msm_pm_init: failed to map reset vector\n");
		return -ENODEV;
	}

	suspend_set_ops(&msm_pm_ops);

#ifdef CONFIG_MSM_IDLE_STATS
	create_proc_read_entry("msm_pm_stats", S_IRUGO,
				NULL, msm_pm_read_proc, NULL);
#endif
	return 0;
}

__initcall(msm_pm_init);
