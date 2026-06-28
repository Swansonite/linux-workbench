// SPDX-License-Identifier: GPL-2.0
/*
 * Author:
 *   David Silver
 *
 * Development note: 
 * This implementation was developed through substantial design, testing,
 * and troubleshooting, with AI used as a supporting tool. The intended
 * behavior was defined and the implementation was directed, reviewed, and
 * refined through several build and runtime failures until a working kernel
 * build was achieved.
 *
 * Target kernel:
 *   This implementation was designed and tested for use with the following
 *   RHEL kernel:
 *
 *     kernel-6.12.0-55.9.1.el10_0
 *
 * Build integration:
 *   Add the following lines to kernel/Makefile so this source file is compiled
 *   and linked into the built in kernel image:
 *
 *     # dukesilver demo sysctl
 *     obj-y += duke_sysctl.o
 */

/*
 * ============================================================================
 *                             kernel.duke sysctl demo 
 * ============================================================================
 *
 * Goal / demo:
 * Test the process of adding and building a custom sysctl in the RHEL
 * kernel, while providing a simple, highly visible confirmation that the
 * kernel was rebuilt successfully from modified source.
 *
 *  What this patch adds:
 *    - A new sysctl knob: /proc/sys/kernel/duke
 *        * 0 => disabled (default)
 *        * 1 => enabled
 *
 *    When enabled:
 *      - The kernel prints a large ASCII banner to the kernel ring buffer
 *        (viewable through `dmesg`) every 4 seconds.
 *
 *    When disabled:
 *      - The periodic printing stops quickly and safely.
 *
 *  How to use at runtime:
 *    Enable:
 *      # sysctl -w kernel.duke=1
 *      # OR: echo 1 > /proc/sys/kernel/duke
 *
 *    Disable:
 *      # sysctl -w kernel.duke=0
 *      # OR: echo 0 > /proc/sys/kernel/duke
 *
 *  Implementation overview:
 *    - We register a sysctl under the existing "kernel" sysctl namespace
 *      using register_sysctl_init("kernel", ...).
 *    - The sysctl value is stored in duke_enabled.
 *    - When duke_enabled changes, we start or stop a delayed work item.
 *    - The delayed work prints the banner and schedules itself again 4 seconds
 *      later.
 *
 *  Safety and concurrency:
 *    - sysctl handlers can be called concurrently.
 *    - The workqueue function can run concurrently with a sysctl write.
 *    - We use duke_lock to serialize sysctl writes and state transitions.
 *    - The worker uses READ_ONCE() to check duke_enabled without taking the
 *      same mutex used by the sysctl handler. This avoids deadlock with
 *      cancel_delayed_work_sync().
 *
 *  Notes:
 *    - This is intentionally loud and prints a large banner every 4 seconds.
 *    - The knob is writable only by root because mode is 0644.
 */

#include <linux/init.h>       /* late_initcall() and initialization annotations */
#include <linux/kernel.h>     /* pr_info(), pr_err() */
#include <linux/workqueue.h>  /* delayed_work, schedule_delayed_work(), cancel_delayed_work_sync() */
#include <linux/jiffies.h>    /* msecs_to_jiffies() */
#include <linux/sysctl.h>     /* sysctl definitions and helpers */
#include <linux/mutex.h>      /* mutex primitives */
#include <linux/compiler.h>   /* READ_ONCE(), WRITE_ONCE() */

/*
 * duke_enabled:
 *   Backing storage for /proc/sys/kernel/duke.
 *
 *   We keep it as an int because proc_dointvec_minmax() operates naturally
 *   on integers. The sysctl handler normalizes the value to strict 0 or 1.
 */
static int duke_enabled;

/*
 * duke_lock:
 *   Serializes sysctl writes and state transitions.
 *
 *   Important:
 *     The delayed work function does not take this lock. This avoids a possible
 *     deadlock where the sysctl path calls cancel_delayed_work_sync() while the
 *     worker is waiting on the same mutex.
 */
static DEFINE_MUTEX(duke_lock);

/*
 * Forward declaration of the delayed work function.
 */
static void duke_banner_workfn(struct work_struct *work);

/*
 * duke_work:
 *   Delayed work item used to print the banner periodically.
 */
static DECLARE_DELAYED_WORK(duke_work, duke_banner_workfn);

/*
 * duke_banner:
 *   The ASCII art banner compiled into the kernel image.
 */
static const char duke_banner[] =
"----------------------------------+*%%%%#####+-------------------------------------\n"
"-----------------------=++**###%%%@@@@@@@@@@@@@@@#+--------------------------------\n"
"------------------*@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@%=-----------------------------\n"
"---------------#@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@+--------------------------\n"
"-------------=@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@%-----------------------\n"
"------------%@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@=--------------------\n"
"----------%@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@----------------\n"
"-------+@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@-------------\n"
"------#@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@-----------\n"
"----=@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@--------\n"
"---=@@@@@@@@@@@@@@@@@@%%####%%%@@@@@@@@@@@@@@@@@@%*=--------=%@@@@@@@@@@@@@@@------\n"
"--=@@@@@@@@@@@@@@@@=------------------------------------------*@@@@@@@@@@@@@@@+----\n"
"--=@@@@@@@@@@@@@@@---------------------------------------------+@@@@@@@@@@@@@@@%---\n"
"---#@@@@@@@@@@@@@=----------------------------------------------#@@@@@@@@@@@@@@@=--\n"
"---+@@@@@@@@@@@@@------------------------------------------------@@@@@@@@@@@@@@@+--\n"
"----%@@@@@@@@@@@*------------------------------------------------=@@@@@@@@@@@@@@@=-\n"
"----*@@@@@@@@@@@+-------------------------------------------------#@@@@@@@@@@@@@@+-\n"
"----#@@@@@@@@@@@=--------------------------------------------------#@@@@@@@@@@@@@+-\n"
"----#@@@@@@@@@@*---------------------------------------------------=@@@@@@@@@@@@@=-\n"
"----*@@@@@@@@@@+---------------------------------------------------=@@@@@@@@@@@@*--\n"
"----=@@@@@@@@@#----------------------------------------------------=@@@@@@@@@@@@%--\n"
"-----@@@@@@@@%-----------------------------------------------------=@@@@@@@@@@@@%=-\n"
"-----#@@@@@@%----------=*****+=-----------------------=+**+---------=@@@@@@@@@@*---\n"
"-----=@@@@@@--------=%@@@@@@@@@@@@#=-------------=*#@@@@@@@@@%+-------#@@@@@@@*----\n"
"------=@@@@%------=@@%#+=--=*%@@@@@@@=------=#@@@@@@@@@@*=-=+#%*=------*@@@@@@-----\n"
"-------#@@@#--------------------+#@@@@=-----%@@@@@@@#+------------------@@@@@%-----\n"
"--------@@@+---------------------------------=+++=----------------------@@@@@=-----\n"
"--------@@@=------------------------------------------------------------@@@@=------\n"
"--------@@@-------------------------------------------------------------@@@--------\n"
"--------%@@------------------------------------------------------------=@@*--------\n"
"--------%@@------------------------------------------------------------+@@=--------\n"
"--------#@@------------------------------------------------------------*@@---------\n"
"--------*@@------------------------------------------------------------#@#---------\n"
"--------+@*------------------------------------------------------------#@=---------\n"
"--------------------------------*%@@@#*++*#@@@%+-----------------------------------\n"
"----------------------------=#@@@@@@@@@@@@@@@@@@@@*--------------------------------\n"
"--------------------------=@@@@@@@@@@@@@@@@@@@@@@@@@%=-----------------------------\n"
"------------------------=@@@@@@@@@@@@@@@@@@@@@@@@@@@@@%=---------------------------\n"
"-----------------------+@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@=--------------------------\n"
"-----------------------@@@@@@@@%#**+++====++**##@@@@@@@@*--------------------------\n"
"------------------------=++=-----------------------=+*+=---------------------------\n"
"-----------------------------------------------------------------------------------\n"
"-----------------------------------------------------------------------------------\n"
"-----------------------------------------------------------------------------------\n"
"--------------------------------------------------+#+------------------------------\n"
"-------------------------------**+=---------===+#@+--------------------------------\n"
"---------------------------------+#%@@@@@@@@@@%+-----------------------------------\n"
"-----------------------------------------------------------------------------------\n";

/*
 * duke_banner_workfn()
 *   Function executed by the workqueue when the delayed work runs.
 *
 *   Behavior:
 *     1) Check whether duke_enabled is still enabled.
 *     2) Print the banner.
 *     3) Check again before scheduling another execution.
 *     4) Schedule itself to run again in about 4 seconds if still enabled.
 *
 *   This function intentionally does not take duke_lock. The sysctl handler may
 *   call cancel_delayed_work_sync(), and taking the same mutex here could create
 *   a deadlock.
 */
static void duke_banner_workfn(struct work_struct *work)
{
	if (!READ_ONCE(duke_enabled))
		return;

	pr_info("%s", duke_banner);

	if (READ_ONCE(duke_enabled))
		schedule_delayed_work(&duke_work, msecs_to_jiffies(4000));
}

/*
 * duke_apply_locked()
 *   Applies the current duke_enabled state to delayed work scheduling.
 *
 *   IMPORTANT:
 *     The caller must hold duke_lock.
 *
 *   This is safe because the worker does not take duke_lock.
 */
static void duke_apply_locked(void)
{
	if (duke_enabled) {
		pr_info("duke: enabled via sysctl kernel.duke=1; banner every 4s\n");

		/*
		 * Ensure that a previously queued instance is not left pending.
		 * This also waits for a currently running instance to finish.
		 */
		cancel_delayed_work_sync(&duke_work);

		/* Start immediately. */
		schedule_delayed_work(&duke_work, 0);
	} else {
		pr_info("duke: disabled via sysctl kernel.duke=0\n");

		/*
		 * Stop the periodic execution. If the worker is currently running,
		 * this waits for it to finish before returning.
		 */
		cancel_delayed_work_sync(&duke_work);
	}
}

/*
 * duke_sysctl_handler()
 *   Custom sysctl handler called when users read or write
 *   /proc/sys/kernel/duke or use `sysctl kernel.duke=...`.
 *
 *   We delegate parsing and formatting to proc_dointvec_minmax(), which:
 *     - reads or writes an int
 *     - enforces minimum and maximum values using extra1 and extra2
 *
 *   After a successful write:
 *     - normalize the value to 0 or 1
 *     - apply the scheduling changes
 */
static int duke_sysctl_handler(const struct ctl_table *table, int write,
			       void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	mutex_lock(&duke_lock);

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write) {
		mutex_unlock(&duke_lock);
		return ret;
	}

	/*
	 * Normalize to strict boolean semantics:
	 *   Any nonzero value becomes 1.
	 *
	 * proc_dointvec_minmax() already limits the input to values from 0 through
	 * 1, but keeping this conversion makes the behavior explicit.
	 */
	WRITE_ONCE(duke_enabled, !!duke_enabled);

	duke_apply_locked();

	mutex_unlock(&duke_lock);
	return 0;
}

/*
 * duke_kern_table:
 *   Sysctl table entries registered under the "kernel" namespace.
 *
 *   This creates:
 *     /proc/sys/kernel/duke
 *   and:
 *     sysctl kernel.duke
 *
 *   NOTE:
 *     Do not add a legacy empty terminator entry here.
 *
 *     Newer sized sysctl registration paths use ARRAY_SIZE(table). A trailing
 *     empty { } entry can be counted as a real entry and may cause validation
 *     failure during registration.
 */
static struct ctl_table duke_kern_table[] = {
	{
		.procname	= "duke",
		.data		= &duke_enabled,
		.maxlen		= sizeof(duke_enabled),
		.mode		= 0644,
		.proc_handler	= duke_sysctl_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
};

/*
 * duke_sysctl_init()
 *   Runs late in the boot process to register the sysctl.
 *
 *   register_sysctl_init() is appropriate for built in sysctl registration
 *   during kernel initialization in this source tree.
 *
 *   Expected result:
 *     /proc/sys/kernel/duke
 *     sysctl kernel.duke
 */
static int __init duke_sysctl_init(void)
{
	register_sysctl_init("kernel", duke_kern_table);

	/*
	 * register_sysctl_init() may emit its own warning if registration fails.
	 * Keep this message conservative so the log does not incorrectly guarantee
	 * that the proc entry exists.
	 */
	pr_info("duke: attempted sysctl registration at /proc/sys/kernel/duke\n");

	return 0;
}
late_initcall(duke_sysctl_init);
