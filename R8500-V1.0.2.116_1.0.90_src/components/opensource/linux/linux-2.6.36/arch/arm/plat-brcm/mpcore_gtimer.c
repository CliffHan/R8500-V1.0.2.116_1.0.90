/*
 *  Copyright (C) 1999 - 2003 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/ioport.h>

#include <plat/mpcore.h>
#include <mach/io_map.h>
#if defined(CONFIG_BUZZZ)
#include <asm/buzzz.h>
#endif  /*  CONFIG_BUZZZ */

#include <wps_led.h>

#include <typedefs.h>
#include <bcmutils.h>
#include <bcmdefs.h>
#include <hndsoc.h>
#include <siutils.h>

/*
 * The ARM9 MPCORE Global Timer is a continously-running 64-bit timer,
 * which is used both as a "clock source" and as a "clock event" -
 * there is a banked per-cpu compare and reload registers that are
 * used to generated either one-shot or periodic interrupts on the cpu
 * that calls the mode_set function.
 *
 * NOTE: This code does not support dynamic change of the source clock
 * frequency. The interrupt interval is only calculated once during
 * initialization.
 */

/*
 * Global Timer Registers
 */
#define	GTIMER_COUNT_LO		0x00	/* Lower 32 of 64 bits counter */
#define	GTIMER_COUNT_HI		0x04	/* Higher 32 of 64 bits counter */
#define	GTIMER_CTRL		0x08	/* Control (partially banked) */
#define	GTIMER_CTRL_EN		(1<<0)	/* Timer enable bit */
#define	GTIMER_CTRL_CMP_EN	(1<<1)	/* Comparator enable */
#define	GTIMER_CTRL_IRQ_EN	(1<<2)	/* Interrupt enable */
#define	GTIMER_CTRL_AUTO_EN	(1<<3)	/* Auto-increment enable */
#define	GTIMER_INT_STAT		0x0C	/* Interrupt Status (banked) */
#define	GTIMER_COMP_LO		0x10	/* Lower half comparator (banked) */
#define	GTIMER_COMP_HI		0x14	/* Upper half comparator (banked) */
#define	GTIMER_RELOAD		0x18	/* Auto-increment (banked) */

#define	GTIMER_MIN_RANGE	30	/* Minimum wrap-around time in sec */

/*
 * The following macro-defines are for ARM-CA7
 */
#define	GTIMER_CTRL_TIMER_EN	(1<<0)	/* timer enable */
#define	GTIMER_CTRL_MASK_EN	(1<<1)	/* mask enable */
#define	GTIMER_CTRL_ISTATUS	(1<<2)	/* timer assert status */

/* Gobal variables */
static void __iomem *gtimer_base;
static u32 ticks_per_jiffy;

extern void soc_watchdog(void);

/* The following inline functions are for BCM53573 generic timer */
/* Get counter frequency register */
static inline u32 gtimer_get_cntfrq(void)
{
	u32 val;
	asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r" (val));
	return val;
}

/* Set counter frequency register */
static inline void gtimer_set_cntfrq(u32 cntfrq)
{
	asm volatile("mcr p15, 0, %0, c14, c0, 0" : : "r" (cntfrq));
}

/* Set time PL1 control register */
static inline void gtimer_set_cntkctl(u32 cntkctl)
{
	asm volatile("mcr p15, 0, %0, c14, c1, 0" : : "r" (cntkctl));
}

/* Get PL1 physical timer controller register */
static inline u32 gtimer_get_cntpctl(void)
{
	u32 val;
	asm volatile("mrc p15, 0, %0, c14, c2, 1" : "=r" (val));
	return val;
}

/* Set PL1 physical timer controller register */
static inline void gtimer_set_cntpctl(u32 cntpctl)
{
	asm volatile("mcr p15, 0, %0, c14, c2, 1" : : "r" (cntpctl));
	isb();
}

/* Get physical counter register */
static inline u64 gtimer_get_cntpct(void)
{
	u64 val;

	isb();
	asm volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r" (val));
	return val;
}

/* Get PL1 physical timer compare value register */
static inline u64 gtimer_get_cntp_cval(void)
{
	u64 val;

	isb();
	asm volatile("mrrc p15, 2, %Q0, %R0, c14" : "=r" (val));
	return val;
}

/* Set PL1 physical timer compare value register */
static inline void gtimer_set_cntp_cval(u64 val)
{
	asm volatile("mcrr p15, 2, %Q0, %R0, c14" : : "r" (val));
	isb();
}

static void gtimer_enable(unsigned long freq)
{
	if (gtimer_base != NULL) {
		u32 ctrl;
		/* Prescaler = 0; let the Global Timer run at native PERIPHCLK rate */
		ctrl = GTIMER_CTRL_EN;

		/* Enable the free-running global counter */
		writel(ctrl, gtimer_base + GTIMER_CTRL);
	} else {
		/* For BCM53573 */
		u32 cntkctl = 0;
		u32 cntpctl = GTIMER_CTRL_MASK_EN | GTIMER_CTRL_TIMER_EN;

		gtimer_set_cntfrq((u32)freq);
		gtimer_set_cntkctl(cntkctl);
		/* Set mask bit and set enable bit */
		gtimer_set_cntpctl(cntpctl);
	}
}

#define LED_BLINK_RATE_NORMAL   50
#define LED_BLINK_RATE_QUICK    10

int wps_led_pattern = 0;
int wps_led_state = 0;

/* foxconn added start, zacker, 09/17/2009, @wps_led */
int is_wl_secu_mode = 0;
static int wps_led_is_on = 0;
/* foxconn added end, zacker, 09/17/2009, @wps_led */
static int wps_led_state_old = 1;

static si_t *gpio_sih;
static int wps_led_init(void)
{
    if (!(gpio_sih = si_kattach(SI_OSH))) 
    {
        printk("%s failed!\n", __FUNCTION__);
        return -ENODEV;
    }

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

int gpio_led_on_off(int gpio, int value)
{
    int pin = GPIO_PIN(gpio);
    
    /* foxconn added start, zacker, 09/17/2009, @wps_led */
    if (gpio == WPS_LED_GPIO)
#if defined(R7000) || defined(R8000)
        wps_led_is_on = value;
#else
        wps_led_is_on = !value;
#endif        
    /* foxconn added end, zacker, 09/17/2009, @wps_led */
    
    /* Foxconn modified start pling 12/26/2011, for WNDR4000AC */
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
    /* Foxconn modified end pling 12/26/2011, for WNDR4000AC */

    return 0;
}

static void quick_blink2(void)
{
    static int interrupt_count = -1;

    interrupt_count++;
    if (interrupt_count == LED_BLINK_RATE_QUICK * 2)
        interrupt_count = 0;
    
    if (interrupt_count == 0)
        gpio_led_on_off(WPS_LED_GPIO, 0);
    else if (interrupt_count == LED_BLINK_RATE_QUICK)
        gpio_led_on_off(WPS_LED_GPIO, 1);
}

static void quick_blink(void)
{
    /* foxconn modified start, zacker, 09/17/2009, @wps_led */
    //static int blink_interval = 3000; /* 30 seconds */
    static int blink_interval = 500; /* 5 seconds */
    /* foxconn modified end, zacker, 09/17/2009, @wps_led */
    static int interrupt_count = -1;

    blink_interval--;
    interrupt_count++;
    if (interrupt_count == LED_BLINK_RATE_QUICK * 2)
        interrupt_count = 0;
    
    if (interrupt_count == 0)
        gpio_led_on_off(WPS_LED_GPIO, 0);
    else if (interrupt_count == LED_BLINK_RATE_QUICK)
        gpio_led_on_off(WPS_LED_GPIO, 1);
        
    if ( blink_interval <= 0 )
    {
        /* foxconn modified start, zacker, 09/17/2009, @wps_led */
        //blink_interval = 3000;
        blink_interval = 500;
        /* foxconn modified end, zacker, 09/17/2009, @wps_led */
        wps_led_state = 0;
    }
}

static int normal_blink(void)
{
    static int interrupt_count = -1;

    interrupt_count++;
    if (interrupt_count == LED_BLINK_RATE_NORMAL * 2)
        interrupt_count = 0;
    
    if (interrupt_count == 0)
    {
        gpio_led_on_off(WPS_LED_GPIO, 0);
    }
    else if (interrupt_count == LED_BLINK_RATE_NORMAL)
        gpio_led_on_off(WPS_LED_GPIO, 1);
}
/* Foxconn added end */

static int wps_ap_lockdown_blink(void)
{
    static int interrupt_count = -1;

    interrupt_count++;
    if (interrupt_count == LED_BLINK_RATE_QUICK * 10)
        interrupt_count = 0;
    
    if (interrupt_count == 0)
        gpio_led_on_off(WPS_LED_GPIO, 0);
    else if (interrupt_count == LED_BLINK_RATE_QUICK)
        gpio_led_on_off(WPS_LED_GPIO, 1);
}

/* Foxconn added start pling 02/26/2010 */
/* Add USB LED  */
#if (defined INCLUDE_USB_LED)
/* Foxconn modified start, Wins, 04/11/2011 */
#define LED_BLINK_RATE  5
int usb1_pkt_cnt = 0;
int usb2_pkt_cnt = 0;
int usb1_led_state = 0;
int usb2_led_state = 0;
static int usb1_led_state_old = 1;
static int usb2_led_state_old = 1;
EXPORT_SYMBOL(usb1_pkt_cnt);
EXPORT_SYMBOL(usb2_pkt_cnt);
EXPORT_SYMBOL(usb1_led_state);
EXPORT_SYMBOL(usb2_led_state);
/* Foxconn modified end, Wins, 04/11/2011 */

static int gpio_on_off(int gpio_num, int on_off)
{
    si_gpioreserve(gpio_sih, 1 << gpio_num, GPIO_APP_PRIORITY);
    si_gpioouten(gpio_sih, 1 << gpio_num, 1 << gpio_num, GPIO_APP_PRIORITY);
    si_gpioout(gpio_sih, 1 << gpio_num, on_off << gpio_num, GPIO_APP_PRIORITY);
    return 0;
}

/*Foxconn modify start by Hank 06/21/2012*/
/*change LED behavior, avoid blink when have traffic, plug second USB must blink,  plug first USB not blink*/
static int usb1_normal_blink(void)
{
#if (defined WNDR4000AC)
        gpio_led_on_off(GPIO_USB1_LED, 1);
#elif defined(R7000) || defined(R8000)
        gpio_on_off(GPIO_USB2_LED, 0);
#elif defined(R6250) || defined(R6200v2)
        gpio_on_off(GPIO_USB1_LED, 0);
#else
		if(usb2_led_state==0)
			gpio_on_off(GPIO_USB1_LED, 0);
#endif /* WNDR4000AC */

    return 0;
}
/*Foxconn modify end by Hank 06/21/2012*/

#if (!defined WNDR4000AC) && !defined(R6250) && !defined(R6200v2) && !defined(R7000) && !defined(R8000)
/*Foxconn modify start by Hank 06/21/2012*/
/*change LED behavior, avoid blink when have traffic, plug second USB must blink,  plug first USB not blink*/
static int usb2_normal_blink(void)
{
    static int interrupt_count2 = -1;
    static int first_both_usb = 0;

	if(usb1_led_state==1){
		if(usb1_led_state_old==0 || usb2_led_state_old==0)
			first_both_usb=1;
		
		if(first_both_usb){
			interrupt_count2++;
		
			if (interrupt_count2%50 == 0)
				gpio_on_off(GPIO_USB2_LED, 0);
			else if (interrupt_count2%50 == 25)
				gpio_on_off(GPIO_USB2_LED, 1);
		
			if(interrupt_count2>=500){
				interrupt_count2=0;
				first_both_usb=0;
			}
		}else
			gpio_on_off(GPIO_USB2_LED, 0);
		
	}else
		gpio_on_off(GPIO_USB2_LED, 0);

    return 0;
}
/*Foxconn modify end by Hank 06/21/2012*/
#endif /* WNDR4000AC */
#endif
/* Foxconn added end pling 02/26/2010 */

static cycle_t gptimer_count_read(struct clocksource *cs)
{
	u32 count_hi, count_ho, count_lo;
	u64 count;

	if (gtimer_base != NULL) {
		u32 count_hi, count_ho, count_lo;

		/* Avoid unexpected rollover with double-read of upper half */
		do {
			count_hi = readl( gtimer_base + GTIMER_COUNT_HI );
			count_lo = readl( gtimer_base + GTIMER_COUNT_LO );
			count_ho = readl( gtimer_base + GTIMER_COUNT_HI );
		} while( count_hi != count_ho );

		count = (u64) count_hi << 32 | count_lo;
	} else {
		/* For BCM53573 */
		count = gtimer_get_cntpct();
	}

	return count;
}

static struct clocksource clocksource_gptimer = {
	.name		= "mpcore_gtimer",
	.rating		= 300,
	.read		= gptimer_count_read,
	.mask		= CLOCKSOURCE_MASK(64),
	.shift		= 20,
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

static void __init gptimer_clocksource_init(u32 freq)
{
	struct clocksource *cs = &clocksource_gptimer;

	/* <freq> is timer clock in Hz */
        clocksource_calc_mult_shift(cs, freq, GTIMER_MIN_RANGE);

printk(KERN_EMERG"WPS_LED_GPIO=%d\n",WPS_LED_GPIO);
	clocksource_register(cs);
}

/*
 * IRQ handler for the global timer
 * This interrupt is banked per CPU so is handled identically
 */
static irqreturn_t gtimer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	if (gtimer_base != NULL) {
		/* clear the interrupt */
		writel(1, gtimer_base + GTIMER_INT_STAT);

#if defined(BUZZZ_KEVT_LVL) && (BUZZZ_KEVT_LVL >= 2)
		buzzz_kevt_log1(BUZZZ_KEVT_ID_GTIMER_EVENT, (u32)evt->event_handler);
#endif	/* BUZZZ_KEVT_LVL */
	} else {
		u64 count;

		count = gptimer_count_read(NULL);
		count += ticks_per_jiffy;
		/* Set PL1 Physical Comp Value */
		gtimer_set_cntp_cval(count);
	}

	evt->event_handler(evt);
	
	soc_watchdog();

	/* Foxconn added start */
    /* Blink LED depending of WPS status */
    if ( wps_led_state == 0 )
    {
#if defined(R7000) || defined(R8000)
        if (wps_led_state_old != 0)
            gpio_led_on_off(WPS_LED_GPIO, 0);

        /* foxconn added start, zacker, 09/17/2009, @wps_led */
        #if (!defined WNDR4000AC)   /* pling added 02/03/2012, not needed for R6200 */
        if ((!is_wl_secu_mode) && wps_led_is_on)
            gpio_led_on_off(WPS_LED_GPIO, 0);

        if (is_wl_secu_mode && (!wps_led_is_on))
            gpio_led_on_off(WPS_LED_GPIO, 1);
        #endif /* WNDR4000AC */
#else
        if (wps_led_state_old != 0)
            gpio_led_on_off(WPS_LED_GPIO, 1);

        /* foxconn added start, zacker, 09/17/2009, @wps_led */
        #if (!defined WNDR4000AC)   /* pling added 02/03/2012, not needed for R6200 */
        if ((!is_wl_secu_mode) && wps_led_is_on)
            gpio_led_on_off(WPS_LED_GPIO, 1);

        if (is_wl_secu_mode && (!wps_led_is_on))
            gpio_led_on_off(WPS_LED_GPIO, 0);
        #endif /* WNDR4000AC */
#endif
        /* foxconn added end, zacker, 09/17/2009, @wps_led */
        
    }
    else
    if (wps_led_state == 1)
    {
        normal_blink();
    }
    else
    if (wps_led_state == 2)
    {
        quick_blink();
    }
    else
    if (wps_led_state == 3)
    {
        quick_blink2();
    }
    else
    if (wps_led_state == 4)
    {
        wps_ap_lockdown_blink();
    }
    
    wps_led_state_old = wps_led_state;
    /* Foxconn added end */
#if (defined INCLUDE_USB_LED)
    /* Foxconn modified start, Wins, 04/11/2011 */
	/*Foxconn modify start by Hank 06/21/2012*/
	/*change LED behavior, avoid blink when have traffic,
	 plug second USB must blink,  plug first USB not blink*/	
    if (usb1_led_state)
	/*Foxconn modify end by Hank 06/21/2012*/
    {
        usb1_normal_blink();
    }
    else
    {
        if (usb1_led_state_old){
            /* Foxconn modified start pling 12/26/2011, for WNDR4000AC */
            #if (defined WNDR4000AC)
            gpio_led_on_off(GPIO_USB1_LED, 0);
            #elif defined(R6250) || defined(R6200v2)
            gpio_led_on_off(GPIO_USB1_LED, 1); //off
            #else
            /* Foxconn, [MJ], turn on USB1_Led. */
            gpio_on_off(GPIO_USB1_LED, 1);
            #endif
            /* Foxconn modified end pling 12/26/2011 */
        }
    }
	/*Foxconn modify start by Hank 06/21/2012*/
	/*change LED behavior, avoid blink when have traffic,
	 plug second USB must blink,  plug first USB not blink*/

    #if (!defined WNDR4000AC) && !defined(R6250) && !defined(R6200v2) && !defined(R7000) && !defined(R8000)
    if (usb2_led_state)
    {
        usb2_normal_blink();
    }
    else
    {
        if (usb2_led_state_old){
            /* Foxconn, [MJ], turn on USB2_Led. */
            gpio_on_off(GPIO_USB2_LED, 1);
        }
    }
    usb2_led_state_old = usb2_led_state;
    #endif /* WNDR4000AC */
    /* Foxconn modified end, Wins, 04/11/2011 */
	usb1_led_state_old = usb1_led_state;
	/*Foxconn modify end by Hank 06/21/2012*/
#endif
    /* Foxconn added end pling 02/26/2010 */
    
	
	return IRQ_HANDLED;
}

static void gtimer_set_mode(
	enum clock_event_mode mode,
	struct clock_event_device *evt
	)
{
	u32 ctrl, period;
	u64 count;

	if (gtimer_base != NULL) {
		/* Get current register with global enable and prescaler */
		ctrl = readl( gtimer_base + GTIMER_CTRL );

		/* Clear the mode-related bits */
		ctrl &= ~(	GTIMER_CTRL_CMP_EN |
				GTIMER_CTRL_IRQ_EN |
				GTIMER_CTRL_AUTO_EN);
	} else {
		ctrl = gtimer_get_cntpctl();
		/* Set mask bit, i.e., disable interrupt */
		ctrl |= GTIMER_CTRL_MASK_EN;
	}

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		period = ticks_per_jiffy;
		count = gptimer_count_read( NULL );
		count += period;

		if (gtimer_base != NULL) {
			writel(ctrl, gtimer_base + GTIMER_CTRL);
			writel(count & 0xffffffffUL, gtimer_base + GTIMER_COMP_LO);
			writel(count >> 32, gtimer_base + GTIMER_COMP_HI);
			writel(period, gtimer_base + GTIMER_RELOAD);
			ctrl |= GTIMER_CTRL_CMP_EN |
				GTIMER_CTRL_IRQ_EN |
				GTIMER_CTRL_AUTO_EN;
		} else {
			/* For BCM53573 */
			gtimer_set_cntpctl(ctrl);
			/* Set PL1 Physical Comp Value */
			gtimer_set_cntp_cval(count);
			/* Clear mask bit, i.e., enable interrupt */
			ctrl &= ~GTIMER_CTRL_MASK_EN;
		}

		break;

	case CLOCK_EVT_MODE_ONESHOT:
		/* period set, and timer enabled in 'next_event' hook */
		break;

	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
	default:
		break;
	}

	/* Apply the new mode */
	if (gtimer_base != NULL)
		writel(ctrl, gtimer_base + GTIMER_CTRL);
	else
		gtimer_set_cntpctl(ctrl);
}

static int gtimer_set_next_event(
	unsigned long next,
	struct clock_event_device *evt
	)
{
	u32 ctrl;
	u64 count;

#if defined(BUZZZ_KEVT_LVL) && (BUZZZ_KEVT_LVL >= 2)
	buzzz_kevt_log1(BUZZZ_KEVT_ID_GTIMER_NEXT, (u32)next);
#endif	/* BUZZZ_KEVT_LVL */

	count = gptimer_count_read(NULL);
	count += next;

	if (gtimer_base != NULL) {
		ctrl = readl(gtimer_base + GTIMER_CTRL);
		ctrl &= ~GTIMER_CTRL_CMP_EN;
		writel(ctrl, gtimer_base + GTIMER_CTRL);

		writel(count & 0xffffffffUL, gtimer_base + GTIMER_COMP_LO);
		writel(count >> 32, gtimer_base + GTIMER_COMP_HI);

		/* enable IRQ for the same cpu that loaded comparator */
		ctrl |= GTIMER_CTRL_CMP_EN;
		ctrl |= GTIMER_CTRL_IRQ_EN;

		writel(ctrl, gtimer_base + GTIMER_CTRL);
	} else {
		/* Set PL1 Physical Comp Value for BCM53573 */
		gtimer_set_cntp_cval(count);

		/* Clear mask bit, i.e., enable interrupt */
		ctrl = gtimer_get_cntpctl();
		ctrl &= ~GTIMER_CTRL_MASK_EN;
		gtimer_set_cntpctl(ctrl);
	}

	return 0;
}

static struct clock_event_device gtimer_clockevent = {
	.name		= "mpcore_gtimer",
	.shift		= 20,
	.features       = CLOCK_EVT_FEAT_PERIODIC,
	.set_mode	= gtimer_set_mode,
	.set_next_event	= gtimer_set_next_event,
	.rating		= 300,
	.cpumask	= cpu_all_mask,
};

static struct irqaction gtimer_irq = {
	.name		= "mpcore_gtimer",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_PERCPU,
	.handler	= gtimer_interrupt,
	.dev_id		= &gtimer_clockevent,
};

static void __init gtimer_clockevents_init(u32 freq, unsigned timer_irq)
{
	struct clock_event_device *evt = &gtimer_clockevent;

	evt->irq = timer_irq;
        ticks_per_jiffy = DIV_ROUND_CLOSEST(freq, HZ);

        clockevents_calc_mult_shift(evt, freq, GTIMER_MIN_RANGE);

	evt->max_delta_ns = clockevent_delta2ns(0xffffffff, evt);
	evt->min_delta_ns = clockevent_delta2ns(0xf, evt);

	/* Register the device to install handler before enabing IRQ */
	clockevents_register_device(evt);
	setup_irq(timer_irq, &gtimer_irq);
}

/*
 * MPCORE Global Timer initialization function
 */
void __init mpcore_gtimer_init( 
	void __iomem *base, 
	unsigned long freq,
	unsigned int timer_irq)
{
	u64 count;

	gtimer_base = base;

	printk(KERN_INFO "MPCORE Global Timer Clock %luHz\n",
		(unsigned long) freq);

	/* Init PMU ALP/ILP period for BCM53573 */
	if (gtimer_base == NULL) {
		void * __iomem reg_base = (void *)SOC_PMU_BASE_VA;

		/* Configure ALP period, 0x199 = 16384/40 for using 40KHz crystal */
		writel(0x10199, reg_base + 0x6dc);

		writel(0x10000, reg_base + 0x674);
	}

	/* Enable the timer */
	gtimer_enable(freq);

	/* Self-test the timer is running */
	count = gptimer_count_read(NULL);

	/* Register as time source */
	gptimer_clocksource_init(freq);

	/* Register as system timer */
	gtimer_clockevents_init(freq, timer_irq);

	count = gptimer_count_read(NULL) - count ;
	if( count == 0 )
		printk(KERN_CRIT "MPCORE Global Timer Dead!!\n");
    wps_led_init(); /* Foxconn added */		
}
