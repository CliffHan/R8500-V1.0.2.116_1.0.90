/*
 *  linux/arch/arm/kernel/smp.c
 *
 *  Copyright (C) 2002 ARM Limited, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/cache.h>
#include <linux/profile.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/seq_file.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <linux/clockchips.h>

#include <asm/atomic.h>
#include <asm/cacheflush.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/processor.h>
#include <asm/tlbflush.h>
#include <asm/ptrace.h>
#include <asm/localtimer.h>
#include <asm/smp_plat.h>

/*Foxconn add start by Hank 05/31/2013*/
#include <typedefs.h>
#include <osl.h>
#include <wps_led.h>
#include <siutils.h>
/*Foxconn add end by Hank 05/31/2013*/

#ifdef WIFI_LED_BLINKING
#include <linux/netdevice.h>
#endif

#ifdef CONFIG_BCM47XX
extern void soc_watchdog(void);
#endif

/*Foxconn add start by Hank 05/31/2013*/
/*declare parameter*/
#define LED_BLINK_RATE_NORMAL   50
#define LED_BLINK_RATE_QUICK    10
static si_t *gpio_sih;
int wps_led_state_smp = 0;
int is_wl_secu_mode_smp = 0;
static int wps_led_is_on_smp = 0;
static int wps_led_state_smp_old = 0;

/* foxconn added start, ken chen, 12/13/2013, Support LED_CONTROL_SETTINGS */
int led_control_settings_smp = 3;    /* 1=enable_blink, 2=disable_blink, 3=turn_off */
/* foxconn added end, ken chen, 12/13/2013, Support LED_CONTROL_SETTINGS */

/*
 * as from 2.5, kernels no longer have an init_tasks structure
 * so we need some other way of telling a new secondary core
 * where to place its SVC stack
 */
struct secondary_data secondary_data;

/*
 * structures for inter-processor calls
 * - A collection of single bit ipi messages.
 */
struct ipi_data {
	spinlock_t lock;
	unsigned long ipi_count;
	unsigned long bits;
};

static DEFINE_PER_CPU(struct ipi_data, ipi_data) = {
	.lock	= SPIN_LOCK_UNLOCKED,
};

enum ipi_msg_type {
	IPI_TIMER,
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CALL_FUNC_SINGLE,
	IPI_CPU_STOP,
};

int __cpuinit __cpu_up(unsigned int cpu)
{
	struct cpuinfo_arm *ci = &per_cpu(cpu_data, cpu);
	struct task_struct *idle = ci->idle;
	pgd_t *pgd;
	pmd_t *pmd;
	int ret;

	/*
	 * Spawn a new process manually, if not already done.
	 * Grab a pointer to its task struct so we can mess with it
	 */
	if (!idle) {
		idle = fork_idle(cpu);
		if (IS_ERR(idle)) {
			printk(KERN_ERR "CPU%u: fork() failed\n", cpu);
			return PTR_ERR(idle);
		}
		ci->idle = idle;
	} else {
		/*
		 * Since this idle thread is being re-used, call
		 * init_idle() to reinitialize the thread structure.
		 */
		init_idle(idle, cpu);
	}

	/*
	 * Allocate initial page tables to allow the new CPU to
	 * enable the MMU safely.  This essentially means a set
	 * of our "standard" page tables, with the addition of
	 * a 1:1 mapping for the physical address of the kernel.
	 */
	pgd = pgd_alloc(&init_mm);
	pmd = pmd_offset(pgd + pgd_index(PHYS_OFFSET), PHYS_OFFSET);
	*pmd = __pmd((PHYS_OFFSET & PGDIR_MASK) |
		     PMD_TYPE_SECT | PMD_SECT_AP_WRITE);
	flush_pmd_entry(pmd);
	outer_clean_range(__pa(pmd), __pa(pmd + 1));

	/*
	 * We need to tell the secondary core where to find
	 * its stack and the page tables.
	 */
	secondary_data.stack = task_stack_page(idle) + THREAD_START_SP;
	secondary_data.pgdir = virt_to_phys(pgd);
	__cpuc_flush_dcache_area(&secondary_data, sizeof(secondary_data));
	outer_clean_range(__pa(&secondary_data), __pa(&secondary_data + 1));

	/*
	 * Now bring the CPU into our world.
	 */
	ret = boot_secondary(cpu, idle);
	if (ret == 0) {
		/*
		 * timeout is in fixed jiffies - for slow processor
	 	 * the HZ is low, making the waiting longer as necesary.
		 */
		unsigned long timeout = 128 ;

		/*
		 * CPU was successfully started, wait for it
		 * to come online or time out.
		 */
		timeout += jiffies ;
		while (time_before(jiffies, timeout)) {
			if (cpu_online(cpu))
				break;

			udelay(10);
			barrier();
		}

		if (!cpu_online(cpu))
			ret = -EIO;
	}

	secondary_data.stack = NULL;
	secondary_data.pgdir = 0;

	*pmd = __pmd(0);
	clean_pmd_entry(pmd);
	pgd_free(&init_mm, pgd);

	if (ret) {
		printk(KERN_CRIT "CPU%u: processor failed to boot\n", cpu);

	}

	return ret;
}


/* Foxconn modified start antony 07/22/2013, for R7000 WIFI Blinking */
#if (defined WIFI_LED_BLINKING)

    #ifndef GPIO_WIFI_2G_LED
    #define GPIO_WIFI_2G_LED        13   
    #endif

    #ifndef GPIO_WIFI_5G_LED
    #define GPIO_WIFI_5G_LED        12   
    #endif

    #if defined(R8000)
        #ifndef GPIO_WIFI_5G_2_LED
        #define GPIO_WIFI_5G_2_LED      16
        #endif
    #endif
//static __u64 wifi_2g_tx_cnt_smp=0;
//static __u64 wifi_2g_rx_cnt_smp=0;
//static __u64 wifi_5g_tx_cnt_smp=0;
//static __u64 wifi_5g_rx_cnt_smp=0;
int wifi_2g_led_state_smp=-1;
int wifi_5g_led_state_smp=-1;
#if defined(R8000)
int wifi_5g_2_led_state_smp=-1;
#endif

EXPORT_SYMBOL(wifi_2g_led_state_smp);
EXPORT_SYMBOL(wifi_5g_led_state_smp);
#if defined(R8000)
EXPORT_SYMBOL(wifi_5g_2_led_state_smp);
#endif

#endif
/* Foxconn modified end antony 07/22/2013*/


#if (defined INCLUDE_USB_LED)
/* Foxconn modified start, Wins, 04/11/2011 */
/* Foxconn modified start pling 12/26/2011, for WNDR4000AC */
#ifndef GPIO_USB1_LED
    #define GPIO_USB1_LED       18   /* USB1 USB3.0 LED. */
#endif
#ifndef GPIO_USB2_LED
    #define GPIO_USB2_LED       17   /* USB2 USB2.0 LED. */
#endif
/* Foxconn modified end pling 12/26/2011, for WNDR4000AC */
#define LED_BLINK_RATE  10
int usb1_pkt_cnt_smp;
int usb2_pkt_cnt_smp = 0;
int usb1_led_state_smp = 0;
int usb2_led_state_smp = 0;
static int usb1_led_state_old_smp = 0;
static int usb2_led_state_old_smp = 0;
EXPORT_SYMBOL(usb1_pkt_cnt_smp);
EXPORT_SYMBOL(usb2_pkt_cnt_smp);
EXPORT_SYMBOL(usb1_led_state_smp);
EXPORT_SYMBOL(usb2_led_state_smp);

/*foxconn Han edited start, 06/05/2015 for USB LED blink 5 second when plug in*/
int usb1_led_probe;
int usb2_led_probe;
EXPORT_SYMBOL(usb1_led_probe);
EXPORT_SYMBOL(usb2_led_probe);
/*around 5 seconds*/
#define USB_LED_PROBE_BLINK 100
/*foxconn Han edited end, 06/05/2015 for USB LED blink 5 second when plug in*/
/* Foxconn modified end, Wins, 04/11/2011 */

/*foxconn Han edited start, 05/15/2015 for single firmware support 2 HW*/
int led_wl_2g   = GPIO_WIFI_2G_LED;
int led_wl_5g   = GPIO_WIFI_5G_LED;
int led_wl_5g2  = GPIO_WIFI_5G_2_LED;
int led_usb1    = GPIO_USB1_LED;
int led_usb2    = GPIO_USB2_LED;
int led_wps     = WPS_LED_GPIO;
int is8500 = 0;
/*foxconn Han edited start, 01/18/2016*/
#ifdef U12H335T21
int isR8300 = 0; 
#endif /*U12H335T21*/
/*foxconn Han edited end, 01/18/2016*/

EXPORT_SYMBOL(led_wl_2g);
EXPORT_SYMBOL(led_wl_5g);
EXPORT_SYMBOL(led_wl_5g2);
EXPORT_SYMBOL(led_usb1);
EXPORT_SYMBOL(led_usb2);
EXPORT_SYMBOL(led_wps);
/*foxconn Han edited end, 05/15/2015*/
#if defined(DUAL_TRI_BAND_HW_SUPPORT)
#include "ambitCfg.h"
extern char *nvram_get(const char *name);

/*foxconn Han edited start, 01/18/2016*/
#ifdef U12H335T21
int checkR8300(void)
{
    int ret = 0;
    int i = 0, t = 0;
    //char *pt = NULL;
    char *hwrev = nvram_get("hwrev");
 
    if(hwrev == NULL)
        return 0;
    
    if(sscanf(hwrev,"MP%dT%d", &i, &t)==2)
    {
        if(t == AMBIT_R8300_REV)
            ret = 1;
    }
    return ret;
}
#endif /*U12H335T21*/
/*foxconn Han edited end, 01/18/2016*/


void switch_led_definition(void)
{
    char *hwver = nvram_get("hwver");
    char *tri_hw_ver = TRI_BAND_HW_VER; //nvram_get("tri_band_hw_ver");

    if(hwver == NULL || tri_hw_ver == NULL)
        return ;

    if(memcmp(hwver,tri_hw_ver,5) == 0)
    {
        printk(KERN_EMERG"===================\n%s using R8500\n==================\n",__func__);
        led_wl_2g   = GPIO_WIFI_2G_LED_8500;
        led_wl_5g   = GPIO_WIFI_5G_LED_8500;
        led_wl_5g2  = GPIO_WIFI_5G_2_LED_8500;
        led_usb1    = GPIO_USB1_LED_8500;
        led_usb2    = GPIO_USB2_LED_8500;
        led_wps     = WPS_LED_GPIO_8500;
        is8500 = 1;
    }
    else
        printk(KERN_EMERG"===================\n%s using R7800\n==================\n",__func__);
/*foxconn Han edited start, 01/18/2016*/
#ifdef U12H335T21
   isR8300 = checkR8300(); 
#endif /*U12H335T21*/
/*foxconn Han edited end, 01/18/2016*/

}

#endif /*DUAL_TRI_BAND_HW_SUPPORT*/


#ifdef CONFIG_HOTPLUG_CPU
/*
 * __cpu_disable runs on the processor to be shutdown.
 */
int __cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();
	struct task_struct *p;
	int ret;

	ret = platform_cpu_disable(cpu);
	if (ret)
		return ret;

	/*
	 * Take this CPU offline.  Once we clear this, we can't return,
	 * and we must not schedule until we're ready to give up the cpu.
	 */
	set_cpu_online(cpu, false);

	/*
	 * OK - migrate IRQs away from this CPU
	 */
	migrate_irqs();

	/*
	 * Stop the local timer for this CPU.
	 */
	local_timer_stop();

	/*
	 * Flush user cache and TLB mappings, and then remove this CPU
	 * from the vm mask set of all processes.
	 */
	flush_cache_all();
	local_flush_tlb_all();

	read_lock(&tasklist_lock);
	for_each_process(p) {
		if (p->mm)
			cpumask_clear_cpu(cpu, mm_cpumask(p->mm));
	}
	read_unlock(&tasklist_lock);

	return 0;
}

/*
 * called on the thread which is asking for a CPU to be shutdown -
 * waits until shutdown has completed, or it is timed out.
 */
void __cpu_die(unsigned int cpu)
{
	if (!platform_cpu_kill(cpu))
		printk("CPU%u: unable to kill\n", cpu);
}

/*
 * Called from the idle thread for the CPU which has been shutdown.
 *
 * Note that we disable IRQs here, but do not re-enable them
 * before returning to the caller. This is also the behaviour
 * of the other hotplug-cpu capable cores, so presumably coming
 * out of idle fixes this.
 */
void __ref cpu_die(void)
{
	unsigned int cpu = smp_processor_id();

	local_irq_disable();
	idle_task_exit();

	/*
	 * actual CPU shutdown procedure is at least platform (if not
	 * CPU) specific
	 */
	platform_cpu_die(cpu);

	/*
	 * Do not return to the idle loop - jump back to the secondary
	 * cpu initialisation.  There's some initialisation which needs
	 * to be repeated to undo the effects of taking the CPU offline.
	 */
	__asm__("mov	sp, %0\n"
	"	b	secondary_start_kernel"
		:
		: "r" (task_stack_page(current) + THREAD_SIZE - 8));
}
#endif /* CONFIG_HOTPLUG_CPU */

/*
 * This is the secondary CPU boot entry.  We're using this CPUs
 * idle thread stack, but a set of temporary page tables.
 */
asmlinkage void __cpuinit secondary_start_kernel(void)
{
	struct mm_struct *mm = &init_mm;
	unsigned int cpu = smp_processor_id();

	printk("CPU%u: Booted secondary processor\n", cpu);

	/*
	 * All kernel threads share the same mm context; grab a
	 * reference and switch to it.
	 */
	atomic_inc(&mm->mm_users);
	atomic_inc(&mm->mm_count);
	current->active_mm = mm;
	cpumask_set_cpu(cpu, mm_cpumask(mm));
	cpu_switch_mm(mm->pgd, mm);
	enter_lazy_tlb(mm, current);
	local_flush_tlb_all();

	cpu_init();
	preempt_disable();

	/*
	 * Give the platform a chance to do its own initialisation.
	 */
	platform_secondary_init(cpu);

	/*
	 * Enable local interrupts.
	 */
	notify_cpu_starting(cpu);
	local_irq_enable();
	local_fiq_enable();

	/*
	 * Setup the percpu timer for this CPU.
	 */
	percpu_timer_setup();

	calibrate_delay();

	smp_store_cpu_info(cpu);

	/*
	 * OK, now it's safe to let the boot CPU continue
	 */
	set_cpu_online(cpu, true);

	/*
	 * OK, it's off to the idle thread for us
	 */
	cpu_idle();
}

/*
 * Called by both boot and secondaries to move global data into
 * per-processor storage.
 */
void __cpuinit smp_store_cpu_info(unsigned int cpuid)
{
	struct cpuinfo_arm *cpu_info = &per_cpu(cpu_data, cpuid);

	cpu_info->loops_per_jiffy = loops_per_jiffy;
}

void __init smp_cpus_done(unsigned int max_cpus)
{
	int cpu;
	unsigned long bogosum = 0;

	for_each_online_cpu(cpu)
		bogosum += per_cpu(cpu_data, cpu).loops_per_jiffy;

	printk(KERN_INFO "SMP: Total of %d processors activated "
	       "(%lu.%02lu BogoMIPS).\n",
	       num_online_cpus(),
	       bogosum / (500000/HZ),
	       (bogosum / (5000/HZ)) % 100);
}

void __init smp_prepare_boot_cpu(void)
{
	unsigned int cpu = smp_processor_id();

	per_cpu(cpu_data, cpu).idle = current;
}

static void send_ipi_message(const struct cpumask *mask, enum ipi_msg_type msg)
{
	unsigned long flags;
	unsigned int cpu;

	local_irq_save(flags);

	for_each_cpu(cpu, mask) {
		struct ipi_data *ipi = &per_cpu(ipi_data, cpu);

		spin_lock(&ipi->lock);
		ipi->bits |= 1 << msg;
		spin_unlock(&ipi->lock);
	}

	/*
	 * Call the platform specific cross-CPU call function.
	 */
	smp_cross_call(mask);

	local_irq_restore(flags);
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	send_ipi_message(mask, IPI_CALL_FUNC);
}

void arch_send_call_function_single_ipi(int cpu)
{
	send_ipi_message(cpumask_of(cpu), IPI_CALL_FUNC_SINGLE);
}

void show_ipi_list(struct seq_file *p)
{
	unsigned int cpu;

	seq_puts(p, "IPI:");

	for_each_present_cpu(cpu)
		seq_printf(p, " %10lu", per_cpu(ipi_data, cpu).ipi_count);

	seq_putc(p, '\n');
}

void show_local_irqs(struct seq_file *p)
{
	unsigned int cpu;

	seq_printf(p, "LOC: ");

	for_each_present_cpu(cpu)
		seq_printf(p, "%10u ", irq_stat[cpu].local_timer_irqs);

	seq_putc(p, '\n');
}

/*
 * Timer (local or broadcast) support
 */
static DEFINE_PER_CPU(struct clock_event_device, percpu_clockevent);

/*Foxconn add start by Hank 05/31/2013*/
/*add function for blinking or light up WPS LED or USB LED for SMP*/
static int wps_led_init_done = 0; /*foxconn Han edited, 04/29/2015 only init once*/
static int wps_led_init(void)
{
    
    if( wps_led_init_done > 0)
        return 0;
    
    wps_led_init_done = 1;

    if (!(gpio_sih = si_kattach(SI_OSH))) 
    {
        printk("%s failed!\n", __FUNCTION__);
        return -ENODEV;
    }
    #if defined(DUAL_TRI_BAND_HW_SUPPORT)
    switch_led_definition();
    #endif /*DUAL_TRI_BAND_HW_SUPPORT*/

    return 0;
}

static int gpio_control_normal(int pin, int value)
{
    si_gpioreserve(gpio_sih, 1 << pin, GPIO_APP_PRIORITY);
    si_gpioouten(gpio_sih, 1 << pin, 1 << pin, GPIO_APP_PRIORITY);
    si_gpioout(gpio_sih, 1 << pin, value << pin, GPIO_APP_PRIORITY);

    return 0;
}

#define GPIO_PIN(x)                     ((x) & 0x00FF)

static int gpio_led_on_off(int gpio, int value)
{
    int pin = GPIO_PIN(gpio);

    //if (gpio == WPS_LED_GPIO)
    if (gpio == led_wps)
#if defined(R7000) || defined(R8000)
        wps_led_is_on_smp = value;
#else
        wps_led_is_on_smp = !value;
#endif

    /*foxconn Han edited start, 01/18/2016*/
#ifdef U12H335T21
    /*for costco sku, don't blink USB2.0*/
    if(isR8300 && (gpio == led_usb2))
        return 0;
#endif /*U12H335T21*/
    /*foxconn Han edited end, 01/18/2016*/
    
#if (defined GPIO_EXT_CTRL)
    int ctrl_mode = GPIO_CTRL_MODE(gpio);
    
    switch (ctrl_mode)
    {
        case GPIO_CTRL_MODE_CLK_DATA:
            /* implement in ext_led */
            gpio_control_clk_data(pin, value);
            break;
            
        case GPIO_CTRL_MODE_NONE:
        default:
            gpio_control_normal(pin, value);
            break;
    }
#else
    gpio_control_normal(pin, value);
#endif

    return 0;
}

/*foxconn Han edited, 06/06/2015 added for USB LED blink when probe*/
static int usb1_normal_blink_smp( int probe )
{

    static int interrupt_count1 = -1;
    static int usb1_pkt_cnt_old_smp = 0;
    static int probe_count;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
    int led_on, led_off;
    led_on = (led_control_settings_smp == 3) ? 1 : 0;
	led_off = (led_control_settings_smp == 2) ? 0 : 1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */

    interrupt_count1++;
    if (interrupt_count1 == (LED_BLINK_RATE * 2))
    {
        interrupt_count1 = 0;
    }    
    if (interrupt_count1 == 0){
        /*Foxconn, [MJ], turn off USB_Led. */
        //gpio_led_on_off(GPIO_USB1_LED, 0);
        //gpio_led_on_off(GPIO_USB1_LED, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
        gpio_led_on_off(led_usb1, led_on);
        /*
        if(probe)
            printk(KERN_EMERG"%s %d probe=1, probe_count=%d count1=%d\n",__func__,__LINE__,probe_count,interrupt_count1);
        */

    }
    else if (interrupt_count1 == LED_BLINK_RATE)
    {
        /*foxconn Han edited start, 06/06/2015 added for USB LED blink when probe*/
        if(probe)
        {
            gpio_led_on_off(led_usb1, led_off);
            probe_count++;
            //printk(KERN_EMERG"%s %d probe=1, probe_count=%d count1=%d\n",__func__,__LINE__,probe_count,interrupt_count1);
            if(probe_count > USB_LED_PROBE_BLINK)
            {
                probe_count = 0;
                usb1_led_probe = 0;    
            }
            //printk("<1> turn on USB_LED.\n");
        } 
        else
        /*foxconn Han edited end, 06/06/2015 added for USB LED blink when probe*/
        if (usb1_pkt_cnt_smp != usb1_pkt_cnt_old_smp) 
        {
            usb1_pkt_cnt_old_smp = usb1_pkt_cnt_smp;
//old0=usb1_pkt_cnt_old;
            /*Foxconn, [MJ], turn on USB_Led. */
            //gpio_led_on_off(GPIO_USB1_LED, 1);
            //gpio_led_on_off(GPIO_USB1_LED, led_off); /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
            gpio_led_on_off(led_usb1, led_off);
            //printk("<1> turn on USB_LED.\n");
        }
    }

    return 0;
}
/*Foxconn modify end by Hank 06/21/2012*/

/* Added by Foxconn Antony to blink WIFI LED when there is traffic*/
#ifdef WIFI_LED_BLINKING
void wifi_normal_blink_smp()
{
    struct net_device *net_dev;
    static int interrupt_wifi_count = -1;
static __u64 wifi_2g_tx_cnt_old_smp=0;
static __u64 wifi_2g_rx_cnt_old_smp=0;
static __u64 wifi_5g_tx_cnt_old_smp=0;
static __u64 wifi_5g_rx_cnt_old_smp=0;
#if defined(R8000)
static __u64 wifi_5g_2_tx_cnt_old_smp=0;
static __u64 wifi_5g_2_rx_cnt_old_smp=0;
#endif
	struct rtnl_link_stats64 temp;
	const struct rtnl_link_stats64 *stats;
	static int repeat_2g=4,repeat_5g=4;
#if defined(R8000)
	static int repeat_5g_2=4;
#endif
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
	int led_on;
	int led_off;
	led_on = (led_control_settings_smp == 3) ? 1 : 0;
	led_off = (led_control_settings_smp == 2) ? 0 : 1;
	/* foxconn add start ken chen, 12/13/2013, to support LED control Settings */

    interrupt_wifi_count++;

    if (interrupt_wifi_count == (LED_BLINK_RATE * 2))
        interrupt_wifi_count = 0;

    if(wifi_2g_led_state_smp==1)
    {
   	    if (interrupt_wifi_count == 0)
   	    {
        /*Foxconn, [MJ], turn off USB_Led. */
            //gpio_led_on_off(GPIO_WIFI_2G_LED, 0);
			//gpio_led_on_off(GPIO_WIFI_2G_LED, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
			gpio_led_on_off(led_wl_2g, led_on);
        }
        else if(interrupt_wifi_count == LED_BLINK_RATE)
        {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
	          net_dev = dev_get_by_name("eth1");

#else
          	net_dev = dev_get_by_name(&init_net, "eth1");
                  if(net_dev)
                      dev_put(net_dev);
#endif
              
            if(net_dev)
            {
    	          stats = dev_get_stats(net_dev, &temp);

                if((wifi_2g_tx_cnt_old_smp!=stats->tx_packets) || (wifi_2g_rx_cnt_old_smp!=stats->rx_packets))
                {
            				//gpio_led_on_off(GPIO_WIFI_2G_LED, 1);
							//gpio_led_on_off(GPIO_WIFI_2G_LED, led_off);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
							gpio_led_on_off(led_wl_2g, led_off);
            				wifi_2g_tx_cnt_old_smp=stats->tx_packets;
            				wifi_2g_rx_cnt_old_smp=stats->rx_packets;
                    repeat_2g=0;                
                }
                else if( repeat_2g <4)
                {
                    repeat_2g++;
            				//gpio_led_on_off(GPIO_WIFI_2G_LED, 1);
							//gpio_led_on_off(GPIO_WIFI_2G_LED, led_off);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
            				gpio_led_on_off(led_wl_2g, led_off);
            				
            		}
            }
            else
            {
            }
        }
    }    
    else if(wifi_2g_led_state_smp==0)
        //gpio_led_on_off(GPIO_WIFI_2G_LED, 1);
        gpio_led_on_off(led_wl_2g, 1);


    if(wifi_5g_led_state_smp==1)
    {
   	    if (interrupt_wifi_count == 0)
   	    {
        /*Foxconn, [MJ], turn off USB_Led. */
            //gpio_led_on_off(GPIO_WIFI_5G_LED, 0);
			//gpio_led_on_off(GPIO_WIFI_5G_LED, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
            gpio_led_on_off(led_wl_5g, led_on);
        }
        else if(interrupt_wifi_count == LED_BLINK_RATE)
        {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
	          net_dev = dev_get_by_name("eth2");
#else
          	net_dev = dev_get_by_name(&init_net, "eth2");
                  if(net_dev)
                      dev_put(net_dev);
#endif

            if(net_dev)
            {
    	          stats = dev_get_stats(net_dev, &temp);
                if((wifi_5g_tx_cnt_old_smp!=stats->tx_packets) || (wifi_5g_rx_cnt_old_smp!=stats->rx_packets))
                {
            				//gpio_led_on_off(GPIO_WIFI_5G_LED, 1);
							//gpio_led_on_off(GPIO_WIFI_5G_LED, led_off);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
            				gpio_led_on_off(led_wl_5g, led_off);
            				wifi_5g_tx_cnt_old_smp=stats->tx_packets;
            				wifi_5g_rx_cnt_old_smp=stats->rx_packets;
            				repeat_5g=0;
                }
                else if( repeat_5g <4)
                {
                    repeat_5g++;
            		//gpio_led_on_off(GPIO_WIFI_5G_LED, 1);
					//gpio_led_on_off(GPIO_WIFI_5G_LED, led_off);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */            				
            		gpio_led_on_off(led_wl_5g, led_off);
            		}

            }    
            else
            {
            }
        }
    }else if(wifi_5g_led_state_smp==0)
        //gpio_led_on_off(GPIO_WIFI_5G_LED, 1);    
        gpio_led_on_off(led_wl_5g, 1);    

#if defined(R8000)
    if(wifi_5g_2_led_state_smp==1)
    {
   	    if (interrupt_wifi_count == 0)
   	    {
            //gpio_led_on_off(GPIO_WIFI_5G_2_LED, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
            gpio_led_on_off(led_wl_5g2, led_on);
        }
        else if(interrupt_wifi_count == LED_BLINK_RATE)
        {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
	          net_dev = dev_get_by_name("eth3");
#else
          	net_dev = dev_get_by_name(&init_net, "eth3");
                  if(net_dev)
                      dev_put(net_dev);
#endif

            if(net_dev)
            {
    	          stats = dev_get_stats(net_dev, &temp);
                if((wifi_5g_2_tx_cnt_old_smp!=stats->tx_packets) || (wifi_5g_2_rx_cnt_old_smp!=stats->rx_packets))
                {
                    //gpio_led_on_off(GPIO_WIFI_5G_2_LED, led_off);
                    gpio_led_on_off(led_wl_5g2, led_off);
            				wifi_5g_2_tx_cnt_old_smp=stats->tx_packets;
            				wifi_5g_2_rx_cnt_old_smp=stats->rx_packets;
            				repeat_5g_2=0;
                }
                else if( repeat_5g_2 <4)
                {
                    repeat_5g_2++;
                    //gpio_led_on_off(GPIO_WIFI_5G_2_LED, led_off);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */            				
                    gpio_led_on_off(led_wl_5g2, led_off); 
            		}

            }    
            else
            {
            }
        }
    }else if(wifi_5g_2_led_state_smp==0)
        //gpio_led_on_off(GPIO_WIFI_5G_2_LED, 1);    
        gpio_led_on_off(led_wl_5g2, 1);    
#endif
}
#endif
/* Added by Foxconn Antony end */


#if (!defined WNDR4000AC) && !defined(R6250) && !defined(R6200v2) && !defined(R7900)
/*Foxconn modify start by Hank 06/21/2012*/
/*change LED behavior, avoid blink when have traffic, plug second USB must blink,  plug first USB not blink*/
/*foxconn Han edited, 06/06/2015 added for USB LED blink when probe*/
static int usb2_normal_blink_smp(int probe)
{
    //static int interrupt_count2 = -1;
    //static int first_both_usb = 0;
	
	/* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
	int led_on, led_off;
	led_on = (led_control_settings_smp == 3) ? 1 : 0;
	led_off = (led_control_settings_smp == 2) ? 0 : 1;
	/* foxconn add start ken chen, 12/13/2013, to support LED control Settings */

#if 0 /*foxconn Han removed, 04/29/2015*/
	if(usb1_led_state_smp==1){
		if(usb1_led_state_old_smp==0 || usb2_led_state_old_smp==0)
			first_both_usb=1;
		
		if(first_both_usb){
			interrupt_count2++;
		
			if (interrupt_count2%50 == 0)
				gpio_led_on_off(led_usb2, led_on);
				//gpio_led_on_off(GPIO_USB2_LED, 0);
				//gpio_led_on_off(GPIO_USB2_LED, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */            				
			else if (interrupt_count2%50 == 25)
				gpio_led_on_off(led_usb2, led_off);
				//gpio_led_on_off(GPIO_USB2_LED, 1);
				//gpio_led_on_off(GPIO_USB2_LED, led_off);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
		
			if(interrupt_count2>=500){
				interrupt_count2=0;
				first_both_usb=0;
			}
		}else
			gpio_led_on_off(led_usb2, led_on);
			//gpio_led_on_off(GPIO_USB2_LED, 0);
			//gpio_led_on_off(GPIO_USB2_LED, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings	
	}

  if(!first_both_usb)
#endif
  {
    static int interrupt_count2 = -1;
    static int usb2_pkt_cnt_old_smp = 0;
    static int probe_count;

    interrupt_count2++;
    if (interrupt_count2 == LED_BLINK_RATE * 2)
        interrupt_count2 = 0;
    
    if (interrupt_count2 == 0){
        /*Foxconn, [MJ], turn off USB_Led. */
        //gpio_led_on_off(GPIO_USB2_LED, 0);
		//gpio_led_on_off(GPIO_USB2_LED, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */            				
		gpio_led_on_off(led_usb2, led_on);

        /*if(probe)
            printk(KERN_EMERG"%s %d probe=1, probe_count=%d count2=%d\n",__func__,__LINE__,probe_count,interrupt_count2);
        */
    }
    else if (interrupt_count2 == LED_BLINK_RATE)
    {
        /*foxconn Han edited start, 06/06/2015 added for USB LED blink when probe*/
        if(probe)
        {
            gpio_led_on_off(led_usb2, led_off);
            probe_count++;
            //printk(KERN_EMERG"%s %d probe=1, probe_count=%d count2=%d\n",__func__,__LINE__,probe_count,interrupt_count2);
            if(probe_count > USB_LED_PROBE_BLINK)
            {
                probe_count = 0;
                usb2_led_probe = 0;    
            }
        } 
        else
        /*foxconn Han edited end, 06/06/2015 added for USB LED blink when probe*/
        if (usb2_pkt_cnt_smp != usb2_pkt_cnt_old_smp) 
        {
            usb2_pkt_cnt_old_smp = usb2_pkt_cnt_smp;
            //old1=usb2_pkt_cnt_old;
            /*Foxconn, [MJ], turn on USB_Led. */
            //gpio_led_on_off(GPIO_USB2_LED, 1);
			//gpio_led_on_off(GPIO_USB2_LED, led_off);  /*foxconn add ken chen, 12/13/2013, to support LED control Settings */
            gpio_led_on_off(led_usb2, led_off);
            //printk("<1> turn on USB_LED.\n");
        }
    }
  	
  }

    return 0;
}

#endif
#endif


static int normal_blink(void)
{
    static int interrupt_count = -1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
    int led_on;
    led_on = (led_control_settings_smp == 3) ? 0 : 1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */

    interrupt_count++;
    if (interrupt_count == LED_BLINK_RATE_NORMAL * 2)
        interrupt_count = 0;
    
    if (interrupt_count == 0)
        //gpio_led_on_off(WPS_LED_GPIO, 0);
        gpio_led_on_off(led_wps, 0);
    else if (interrupt_count == LED_BLINK_RATE_NORMAL)
        //gpio_led_on_off(WPS_LED_GPIO, 1);
		//gpio_led_on_off(WPS_LED_GPIO, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
		gpio_led_on_off(led_wps, led_on);
}

static void quick_blink(void)
{
    static int blink_interval = 500; /* 5 seconds */
    static int interrupt_count = -1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
    int led_on;
    led_on = (led_control_settings_smp == 3) ? 0 : 1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */

    blink_interval--;
    interrupt_count++;
    if (interrupt_count == LED_BLINK_RATE_QUICK * 2)
        interrupt_count = 0;
    
    if (interrupt_count == 0)
        //gpio_led_on_off(WPS_LED_GPIO, 0);
        gpio_led_on_off(led_wps, 0);
    else if (interrupt_count == LED_BLINK_RATE_QUICK)
        //gpio_led_on_off(WPS_LED_GPIO, 1);
		//gpio_led_on_off(WPS_LED_GPIO, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
		gpio_led_on_off(led_wps, led_on);
        
    if ( blink_interval <= 0 )
    {
        blink_interval = 500;
        wps_led_state_smp = 0;
    }
}

static void quick_blink2(void)
{
    static int interrupt_count = -1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
    int led_on;
    led_on = (led_control_settings_smp == 3) ? 0 : 1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */

    interrupt_count++;
    if (interrupt_count == LED_BLINK_RATE_QUICK * 2)
        interrupt_count = 0;
    
    if (interrupt_count == 0)
        //gpio_led_on_off(WPS_LED_GPIO, 0);
        gpio_led_on_off(led_wps, 0);
    else if (interrupt_count == LED_BLINK_RATE_QUICK)
        //gpio_led_on_off(WPS_LED_GPIO, 1);
        //gpio_led_on_off(WPS_LED_GPIO, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
        gpio_led_on_off(led_wps, led_on);
}

static int wps_ap_lockdown_blink(void)
{
    static int interrupt_count = -1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
    int led_on;
    led_on = (led_control_settings_smp == 3) ? 0 : 1;
    /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */

    interrupt_count++;
    if (interrupt_count == ( LED_BLINK_RATE_NORMAL * 2))
        interrupt_count = 0;
    
    if (interrupt_count == LED_BLINK_RATE_QUICK)
        //gpio_led_on_off(WPS_LED_GPIO, 0);
        gpio_led_on_off(led_wps, 0);
    else if (interrupt_count == 0)
        //gpio_led_on_off(WPS_LED_GPIO, 1);
		//gpio_led_on_off(WPS_LED_GPIO, led_on);  /* foxconn add ken chen, 12/13/2013, to support LED control Settings */
		gpio_led_on_off(led_wps, led_on);
}

/*Foxconn add end by Hank 05/31/2013*/

static void ipi_timer(void)
{
	struct clock_event_device *evt = &__get_cpu_var(percpu_clockevent);
#ifdef CONFIG_BCM47XX
	int cpu = smp_processor_id();
#endif
	irq_enter();
	evt->event_handler(evt);
#ifdef CONFIG_BCM47XX
	if (cpu == 0)
		soc_watchdog();
#endif
	/*Foxconn add start by Hank 05/31/2013*/
	/*add feature for blinking or light up WPS LED or USB LED for SMP*/
	if (cpu == 0){
		if ( wps_led_state_smp == 0 ){
#if defined(R7000) || defined(R8000)
            /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
            int led_on;
            led_on = (led_control_settings_smp == 3) ? 0 : 1;
            /* foxconn add start ken chen, 12/13/2013, to support LED control Settings */
			
			if (wps_led_state_smp_old != 0)
				//gpio_led_on_off(WPS_LED_GPIO, 0);
				gpio_led_on_off(led_wps, 0);

			if ((!is_wl_secu_mode_smp) && wps_led_is_on_smp)
				//gpio_led_on_off(WPS_LED_GPIO, 0);
				gpio_led_on_off(led_wps, 0);

            //if (is_wl_secu_mode_smp && (!wps_led_is_on_smp))
            //    gpio_led_on_off(WPS_LED_GPIO, 1);
				
            /* foxconn add start, ken chen, 12/13/2013, to support LED control Settings */
            if (led_on) {
                if (is_wl_secu_mode_smp && (!wps_led_is_on_smp)) {
                    //gpio_led_on_off(WPS_LED_GPIO, led_on);
                    gpio_led_on_off(led_wps, led_on);
                }
            }
			else {
                if (is_wl_secu_mode_smp && (wps_led_is_on_smp)) {
                    //gpio_led_on_off(WPS_LED_GPIO, led_on);  
                    gpio_led_on_off(led_wps, led_on);  
                }
            }
            /* foxconn add end, ken chen, 12/13/2013, to support LED control Settings */
#else
			if (wps_led_state_smp_old != 0)
				//gpio_led_on_off(WPS_LED_GPIO, 1);
				gpio_led_on_off(led_wps, 1);

			if ((!is_wl_secu_mode_smp) && wps_led_is_on_smp)
				//gpio_led_on_off(WPS_LED_GPIO, 1);
				gpio_led_on_off(led_wps, 1);

			if (is_wl_secu_mode_smp && (!wps_led_is_on_smp))
				//gpio_led_on_off(WPS_LED_GPIO, 0);
				gpio_led_on_off(led_wps, 0);
#endif
		}else if (wps_led_state_smp == 1){
			normal_blink();
		}else if (wps_led_state_smp == 2){
			quick_blink();
		}else if (wps_led_state_smp == 3){
			quick_blink2();
		}else if (wps_led_state_smp == 4){
			wps_ap_lockdown_blink();
		}
    
		wps_led_state_smp_old = wps_led_state_smp;
#if (defined INCLUDE_USB_LED)
        
    	/* plug second USB must blink,  plug first USB not blink*/	
        /*foxconn Han edited, 06/06/2015 added for USB LED blink when probe*/
        if(usb1_led_probe)
        {
            usb1_normal_blink_smp(1);
        } 
        else 
        if (usb1_led_state_smp)
        {
            usb1_normal_blink_smp(0);
        }
        else
        {
            if (usb1_led_state_old_smp)
            {
            /* Foxconn modified start pling 12/26/2011, for WNDR4000AC */
                //gpio_led_on_off(GPIO_USB1_LED, 1);
                gpio_led_on_off(led_usb1, 1);
            /* Foxconn modified end pling 12/26/2011 */
            }
        }
	    /*Foxconn modify start by Hank 06/21/2012*/
	    /*change LED behavior, avoid blink when have traffic,
	    plug second USB must blink,  plug first USB not blink*/
#ifdef WIFI_LED_BLINKING
        wifi_normal_blink_smp();
#endif

#if (!defined WNDR4000AC) && !defined(R6250) && !defined(R6200v2) && !defined(R7900)
        /*foxconn Han edited, 06/06/2015 added for USB LED blink when probe*/
        if(usb2_led_probe)
        {
            usb2_normal_blink_smp(1);
        } 
        else 
        if (usb2_led_state_smp)
        {
            usb2_normal_blink_smp(0);
        }
        else
        {
            if (usb2_led_state_old_smp)
            {
                /* Foxconn, [MJ], turn on USB2_Led. */
                //gpio_led_on_off(GPIO_USB2_LED, 1);
                gpio_led_on_off(led_usb2, 1);
            }
        }
        usb2_led_state_old_smp = usb2_led_state_smp;
#endif /* WNDR4000AC */
        /* Foxconn modified end, Wins, 04/11/2011 */
	    usb1_led_state_old_smp = usb1_led_state_smp;
	    /*Foxconn modify end by Hank 06/21/2012*/
#endif
	}
	/*Foxconn add end by Hank 05/31/2013*/
	
	irq_exit();
}

#ifdef CONFIG_LOCAL_TIMERS
asmlinkage void __exception do_local_timer(struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	int cpu = smp_processor_id();

	if (local_timer_ack()) {
		irq_stat[cpu].local_timer_irqs++;
		ipi_timer();
	}

	set_irq_regs(old_regs);
}
#endif

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
static void smp_timer_broadcast(const struct cpumask *mask)
{
	send_ipi_message(mask, IPI_TIMER);
}
#else
#define smp_timer_broadcast	NULL
#endif

#ifndef CONFIG_LOCAL_TIMERS
static void broadcast_timer_set_mode(enum clock_event_mode mode,
	struct clock_event_device *evt)
{
}

static void local_timer_setup(struct clock_event_device *evt)
{
	evt->name	= "dummy_timer";
	evt->features	= CLOCK_EVT_FEAT_ONESHOT |
			  CLOCK_EVT_FEAT_PERIODIC |
			  CLOCK_EVT_FEAT_DUMMY;
	evt->rating	= 400;
	evt->mult	= 1;
	evt->set_mode	= broadcast_timer_set_mode;

	clockevents_register_device(evt);
}
#endif

void __cpuinit percpu_timer_setup(void)
{
	unsigned int cpu = smp_processor_id();
	struct clock_event_device *evt = &per_cpu(percpu_clockevent, cpu);

	evt->cpumask = cpumask_of(cpu);
	evt->broadcast = smp_timer_broadcast;

	local_timer_setup(evt);
}

static DEFINE_SPINLOCK(stop_lock);

/*
 * ipi_cpu_stop - handle IPI from smp_send_stop()
 */
static void ipi_cpu_stop(unsigned int cpu)
{
	if (system_state == SYSTEM_BOOTING ||
	    system_state == SYSTEM_RUNNING) {
		spin_lock(&stop_lock);
		printk(KERN_CRIT "CPU%u: stopping\n", cpu);
		dump_stack();
		spin_unlock(&stop_lock);
	}

	set_cpu_online(cpu, false);

	local_fiq_disable();
	local_irq_disable();

	while (1)
		cpu_relax();
}

/*
 * Main handler for inter-processor interrupts
 *
 * For ARM, the ipimask now only identifies a single
 * category of IPI (Bit 1 IPIs have been replaced by a
 * different mechanism):
 *
 *  Bit 0 - Inter-processor function call
 */
asmlinkage void __exception do_IPI(struct pt_regs *regs)
{
	unsigned int cpu = smp_processor_id();
	struct ipi_data *ipi = &per_cpu(ipi_data, cpu);
	struct pt_regs *old_regs = set_irq_regs(regs);
	/*Foxconn add start by Hank 05/31/2013*/
	wps_led_init();
	/*Foxconn add end by Hank 05/31/2013*/

	ipi->ipi_count++;

	for (;;) {
		unsigned long msgs;

		spin_lock(&ipi->lock);
		msgs = ipi->bits;
		ipi->bits = 0;
		spin_unlock(&ipi->lock);

		if (!msgs)
			break;

		do {
			unsigned nextmsg;

			nextmsg = msgs & -msgs;
			msgs &= ~nextmsg;
			nextmsg = ffz(~nextmsg);

			switch (nextmsg) {
			case IPI_TIMER:
				ipi_timer();
				break;

			case IPI_RESCHEDULE:
				/*
				 * nothing more to do - eveything is
				 * done on the interrupt return path
				 */
				break;

			case IPI_CALL_FUNC:
				generic_smp_call_function_interrupt();
				break;

			case IPI_CALL_FUNC_SINGLE:
				generic_smp_call_function_single_interrupt();
				break;

			case IPI_CPU_STOP:
				ipi_cpu_stop(cpu);
				break;

			default:
				printk(KERN_CRIT "CPU%u: Unknown IPI message 0x%x\n",
				       cpu, nextmsg);
				break;
			}
		} while (msgs);
	}

	set_irq_regs(old_regs);
}

void smp_send_reschedule(int cpu)
{
	send_ipi_message(cpumask_of(cpu), IPI_RESCHEDULE);
}

void smp_send_stop(void)
{
	cpumask_t mask = cpu_online_map;
	cpu_clear(smp_processor_id(), mask);
	send_ipi_message(&mask, IPI_CPU_STOP);
}

/*
 * not supported here
 */
int setup_profiling_timer(unsigned int multiplier)
{
	return -EINVAL;
}

static void
on_each_cpu_mask(void (*func)(void *), void *info, int wait,
		const struct cpumask *mask)
{
	preempt_disable();

	smp_call_function_many(mask, func, info, wait);
	if (cpumask_test_cpu(smp_processor_id(), mask))
		func(info);

	preempt_enable();
}

/**********************************************************************/

/*
 * TLB operations
 */
struct tlb_args {
	struct vm_area_struct *ta_vma;
	unsigned long ta_start;
	unsigned long ta_end;
};

static inline void ipi_flush_tlb_all(void *ignored)
{
	local_flush_tlb_all();
}

static inline void ipi_flush_tlb_mm(void *arg)
{
	struct mm_struct *mm = (struct mm_struct *)arg;

	local_flush_tlb_mm(mm);
}

static inline void ipi_flush_tlb_page(void *arg)
{
	struct tlb_args *ta = (struct tlb_args *)arg;

	local_flush_tlb_page(ta->ta_vma, ta->ta_start);
}

static inline void ipi_flush_tlb_kernel_page(void *arg)
{
	struct tlb_args *ta = (struct tlb_args *)arg;

	local_flush_tlb_kernel_page(ta->ta_start);
}

static inline void ipi_flush_tlb_range(void *arg)
{
	struct tlb_args *ta = (struct tlb_args *)arg;

	local_flush_tlb_range(ta->ta_vma, ta->ta_start, ta->ta_end);
}

static inline void ipi_flush_tlb_kernel_range(void *arg)
{
	struct tlb_args *ta = (struct tlb_args *)arg;

	local_flush_tlb_kernel_range(ta->ta_start, ta->ta_end);
}

void flush_tlb_all(void)
{
	if (tlb_ops_need_broadcast())
		on_each_cpu(ipi_flush_tlb_all, NULL, 1);
	else
		local_flush_tlb_all();
}

void flush_tlb_mm(struct mm_struct *mm)
{
	if (tlb_ops_need_broadcast())
		on_each_cpu_mask(ipi_flush_tlb_mm, mm, 1, mm_cpumask(mm));
	else
		local_flush_tlb_mm(mm);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long uaddr)
{
	if (tlb_ops_need_broadcast()) {
		struct tlb_args ta;
		ta.ta_vma = vma;
		ta.ta_start = uaddr;
		on_each_cpu_mask(ipi_flush_tlb_page, &ta, 1, mm_cpumask(vma->vm_mm));
	} else
		local_flush_tlb_page(vma, uaddr);
}

void flush_tlb_kernel_page(unsigned long kaddr)
{
	if (tlb_ops_need_broadcast()) {
		struct tlb_args ta;
		ta.ta_start = kaddr;
		on_each_cpu(ipi_flush_tlb_kernel_page, &ta, 1);
	} else
		local_flush_tlb_kernel_page(kaddr);
}

void flush_tlb_range(struct vm_area_struct *vma,
                     unsigned long start, unsigned long end)
{
	if (tlb_ops_need_broadcast()) {
		struct tlb_args ta;
		ta.ta_vma = vma;
		ta.ta_start = start;
		ta.ta_end = end;
		on_each_cpu_mask(ipi_flush_tlb_range, &ta, 1, mm_cpumask(vma->vm_mm));
	} else
		local_flush_tlb_range(vma, start, end);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	if (tlb_ops_need_broadcast()) {
		struct tlb_args ta;
		ta.ta_start = start;
		ta.ta_end = end;
		on_each_cpu(ipi_flush_tlb_kernel_range, &ta, 1);
	} else
		local_flush_tlb_kernel_range(start, end);
}
