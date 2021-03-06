#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/kallsyms.h>
#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>

#include <linux/platform_device.h>
#include <linux/suspend.h>

#include <linux/timer.h>
#include <linux/jiffies.h>

#include <mach/fliper.h>
#include <mt_vcore_dvfs.h>

#define SEQ_printf(m, x...)	    \
	do {			    \
		if (m)		    \
			seq_printf(m, x);	\
		else		    \
			pr_err(x);	    \
	} while (0)

#define X_ms 100
#define Y_steps (2000/X_ms)
#define BW_THRESHOLD 2000
#define BW_THRESHOLD_MAX 9000
#define BW_THRESHOLD_MIN 1000


int bw_threshold = 0;
int fliper_enabled = 0;
int fliper_debug = 0;

static void enable_fliper(void);
static void disable_fliper(void);
extern unsigned int get_ddr_type(void)__attribute__((weak));
/* define supported DRAM types */
enum {
	LPDDR2 = 0,
	DDR3_16,
	DDR3_32,
	LPDDR3,
	mDDR,
};

static ssize_t mt_fliper_write(struct file *filp, const char *ubuf,
		size_t cnt, loff_t *data)
{
	char buf[64];
	int val;
	int ret;

	if (cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;

	ret = kstrtoul(buf, 10, (unsigned long *)&val);
	if (ret < 0)
		return ret;
	if (val == 1) {
		enable_fliper();
	} else if (val == 0) {
		disable_fliper();
	} else if (val == 3) {
		fliper_debug ^= 1;
	} else if (val == 4) {
		fliper_set_bw(0);
		fliper_set_bw(10000);
		fliper_set_bw(BW_THRESHOLD_HIGH);
	} else if (val == 5) {
		fliper_restore_bw();
	} else if (val == 6) {
		fliper_set_bw(BW_THRESHOLD_LOW);
	}
	pr_err(" fliper option: %d\n", val);
	return cnt;

}

static int mt_fliper_show(struct seq_file *m, void *v)
{
	SEQ_printf(m, "----------------------------------------\n");
	SEQ_printf(m, "Fliper Enabled:%d, bw:%d\n", fliper_enabled, bw_threshold);
	SEQ_printf(m, "----------------------------------------\n");
	return 0;
}
/*** Seq operation of mtprof ****/
static int mt_fliper_open(struct inode *inode, struct file *file)
{
	return single_open(file, mt_fliper_show, inode->i_private);
}

static const struct file_operations mt_fliper_fops = {
	.open = mt_fliper_open,
	.write = mt_fliper_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
/******* POWER PERF TRANSFORMER *********/
#include <asm/div64.h>
/*Cache info
#include <mach/mt_cpufreq.h>
*/
static void mt_power_pef_transfer(void);
static DEFINE_TIMER(mt_pp_transfer_timer, (void *)mt_power_pef_transfer, 0, 0);
static int pp_index;

static void mt_power_pef_transfer_work(void);
static DECLARE_WORK(mt_pp_work, (void *) mt_power_pef_transfer_work);

/*EMI*/
static void mt_power_pef_transfer_work(void)
{
	unsigned long long emi_bw = 0;
	int perf_mode = -1;
	int ret;
	unsigned long long t1, t2;

	t1 = 0;
	t2 = 0;

	/*  Get EMI*/
	if (fliper_debug == 1) {
		t1 = sched_clock();
		emi_bw = get_mem_bw();
		t2 = sched_clock();
	} else
		emi_bw = get_mem_bw();

	if (emi_bw > bw_threshold)
		perf_mode = 1;

	if (perf_mode == 1) {
		if (pp_index == 0) {
			ret = vcorefs_request_dvfs_opp(KR_EMI_MON, OPPI_PERF);
			pr_err("[EMI DVFS FLIPER] flip to high mode, emi bw is %llu\n", emi_bw);
		}
		pp_index = 1 << Y_steps;
	} else {
		if (pp_index == 1) {
			ret = vcorefs_request_dvfs_opp(KR_EMI_MON, OPPI_UNREQ);
			pr_err("[EMI DVFS FLIPER] flip to low mode, emi bw is %llu\n", emi_bw);
		}
		pp_index = pp_index >> 1;
	}

	if (fliper_debug == 1)
		pr_err("[EMI DVFS FLIPER]:Rate:count:mode %6llu:%4d :%llu ns\n", emi_bw, pp_index, t2-t1);

}

int fliper_set_bw(int bw)
{
	if (bw <= BW_THRESHOLD_MAX && bw >= BW_THRESHOLD_MIN) {
		pr_err("[EMI DVFS FLIPER] Set bdw threshold %d -> %d\n", bw_threshold, bw);
		bw_threshold = bw;
	} else {
		pr_err("[EMI DVFS FLIPER] Set bdw threshold Error: %d (MAX:%d, MIN:%d)\n",
				bw, BW_THRESHOLD_MAX, BW_THRESHOLD_MIN);
	}
	return 0;
}
int fliper_restore_bw(void)
{
	pr_err("[EMI DVFS FLIPER] Restore bdw threshold %d -> %d\n", bw_threshold, BW_THRESHOLD);
	bw_threshold = BW_THRESHOLD;
	return 0;
}
static void enable_fliper(void)
{
	fliper_enabled = 1;
	pr_err("[EMI DVFS FLIPER] fliper enable +++\n");
	mod_timer(&mt_pp_transfer_timer, jiffies + msecs_to_jiffies(X_ms));
}
static void disable_fliper(void)
{
	fliper_enabled = 0;
	pr_err("fliper disable ---\n");
	del_timer(&mt_pp_transfer_timer);
}
static void mt_power_pef_transfer(void)
{
	mod_timer(&mt_pp_transfer_timer, jiffies + msecs_to_jiffies(X_ms));
	schedule_work(&mt_pp_work);
}

static int fliper_pm_callback(struct notifier_block *nb,
		unsigned long action, void *ptr)
{
	int ret;

	switch (action) {

	case PM_SUSPEND_PREPARE:
		ret = vcorefs_request_dvfs_opp(KR_EMI_MON, OPPI_UNREQ);
		pr_err("[EMI DVFS FLIPER] Suspend and flip to E(%d)\n", ret);
		pp_index = 0;
		disable_fliper();
		break;

	case PM_HIBERNATION_PREPARE:
		break;

	case PM_POST_SUSPEND:
		enable_fliper();
		break;

	case PM_POST_HIBERNATION:
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

/*-----------------------------------------------*/
#define TIME_5SEC_IN_MS 5000

static int __init init_fliper(void)
{
	struct proc_dir_entry *pe;

	pe = proc_create("fliper", 0644, NULL, &mt_fliper_fops);
	if (!pe)
		return -ENOMEM;
	bw_threshold = BW_THRESHOLD;
	pr_err("[EMI DVFS FLIPER] prepare mt pp transfer: jiffies:%lu-->%lu\n", jiffies,
			jiffies + msecs_to_jiffies(TIME_5SEC_IN_MS));
	pr_err("[EMI DVFS FLIPER] -  next jiffies:%lu >>> %lu\n", jiffies,
			jiffies + msecs_to_jiffies(X_ms));
	mod_timer(&mt_pp_transfer_timer, jiffies + msecs_to_jiffies(TIME_5SEC_IN_MS));
	fliper_enabled = 1;
	pm_notifier(fliper_pm_callback, 0);

	return 0;
}
module_init(init_fliper);

