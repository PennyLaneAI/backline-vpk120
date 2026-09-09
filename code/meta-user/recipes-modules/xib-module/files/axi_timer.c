/*
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched_clock.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/timecounter.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

static void __iomem *clocksource_baseaddr;

struct xilinx_timer {
	void __iomem	*timer_baseaddr;
	u32		irq;
	unsigned int	freq_div_hz;
	unsigned int	timer_clock_freq;
	struct cdev	*cdevice;
	struct class	*cl;
	struct device	*dev;
	struct resource res;
	dev_t           dev_num;
};

struct xilinx_timer *xtimer;

#define TCSR0	(0x00)
#define TLR0	(0x04)
#define TCR0	(0x08)
#define TCSR1	(0x10)
#define TLR1	(0x14)
#define TCR1	(0x18)

#define TCSR_MDT	(1<<0)
#define TCSR_UDT	(1<<1)
#define TCSR_GENT	(1<<2)
#define TCSR_CAPT	(1<<3)
#define TCSR_ARHT	(1<<4)
#define TCSR_LOAD	(1<<5)
#define TCSR_ENIT	(1<<6)
#define TCSR_ENT	(1<<7)
#define TCSR_TINT	(1<<8)
#define TCSR_PWMA	(1<<9)
#define TCSR_ENALL	(1<<10)
#define TCSR_CASC	(1<<11)

static unsigned int (*read_fn)(void __iomem *);
static void (*write_fn)(u32, void __iomem *);

static void timer_write32(u32 val, void __iomem *addr)
{
	iowrite32(val, addr);
}

static unsigned int timer_read32(void __iomem *addr)
{
	return ioread32(addr);
}

static void timer_write32_be(u32 val, void __iomem *addr)
{
	iowrite32be(val, addr);
}

static unsigned int timer_read32_be(void __iomem *addr)
{
	return ioread32be(addr);
}

static inline void xilinx_timer0_stop(void)
{
	void __iomem *timer_baseaddr = xtimer->timer_baseaddr;

	write_fn(read_fn(timer_baseaddr + TCSR0) & ~TCSR_ENT,
		 timer_baseaddr + TCSR0);
}

static inline void xilinx_timer0_start(void)
{
	void __iomem *timer_baseaddr = xtimer->timer_baseaddr;
	unsigned long load_val = xtimer->freq_div_hz;

	if (!load_val)
		load_val = 1;
	/* loading value to timer reg */
	write_fn(load_val, timer_baseaddr + TLR0);

	/* load the initial value */
	write_fn(TCSR_LOAD, timer_baseaddr + TCSR0);

	/* see timer data sheet for detail
	 * CASC - enable Cascade mode for 64bit counter/timer
	 * !ENALL - don't enable 'em all
	 * !PWMA - disable pwm
	 * TINT - clear interrupt status
	 * ENT- enable timer itself
	 * !ENIT - enable interrupt
	 * !LOAD - clear the bit to let go
	 * ARHT - auto reload
	 * !CAPT - no external trigger
	 * !GENT - no external signal
	 * !UDT - set the timer as down counter
	 * !MDT0 - generate mode
	 */
	write_fn(TCSR_TINT|TCSR_ENT|TCSR_ARHT,
		 timer_baseaddr + TCSR0);
}

static ssize_t read_counter(struct file *fp, char *buf, size_t len, loff_t *ofs)
{
	u64 value;
	u64 tcr1;

	value = read_fn(clocksource_baseaddr + TCR0);
	tcr1 = read_fn(clocksource_baseaddr + TCR1);
	value = (tcr1 << 32) | value;
	copy_to_user(buf, &value, len);
	return len;
}

static int mmap_counter(struct file *fp, struct vm_area_struct *vma)
{
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
#ifdef CONFIG_MICROBLAZE
        vm_flags_t flags = vma->vm_flags;
	flags |= VM_MIXEDMAP;
        vm_flags_set(vma, flags);
#else
	vma->vm_flags |= VM_MIXEDMAP;
#endif
	if (remap_pfn_range(vma,
			       vma->vm_start,
			       xtimer->res.start,
			       0x1000,
			       vma->vm_page_prot)) {
		printk("%s Failed to map device memory\n", __func__);
		return -EFAULT;
	}
	return 0;
}

struct file_operations fops = {
	.read = read_counter,
	.mmap = mmap_counter,
};

static int xilinx_clocksource_init(unsigned int timer_clock_freq)
{
	/* stop timer0 */
	write_fn(read_fn(clocksource_baseaddr + TCSR0) & ~TCSR_ENT,
		 clocksource_baseaddr + TCSR0);
	/* start timer0 - up counting without interrupt */
	write_fn(TCSR_TINT|TCSR_ENT|TCSR_ARHT, clocksource_baseaddr + TCSR0);

	return 0;
}

#define DEV_MINOR_BASE	0
#define DEVS_CNT	1
#define DEV_NAME	"axitimer"

static void create_char_device(void)
{
	dev_t dev_num;
	struct cdev *cdevice = NULL;
	int err;

	/* char device */
	err = alloc_chrdev_region(&dev_num, DEV_MINOR_BASE, DEVS_CNT, DEV_NAME);
	if (err) {
		dev_err(NULL, "Failed to allocate device numbers\n");
		return;
	}

	cdevice = cdev_alloc();
	if (!cdevice) {
		dev_err(NULL, "Failed to allocate cdev\n");
		unregister_chrdev_region(dev_num, DEVS_CNT);
		return;
	}

	xtimer->cdevice = cdevice;
	cdev_init(cdevice, &fops);
	cdevice->owner = THIS_MODULE;
	err = cdev_add(cdevice, dev_num, DEVS_CNT);
	if (err) {
		dev_err(NULL, "Failed to add cdev to VFS\n");
		unregister_chrdev_region(dev_num, DEVS_CNT);
		return;
	}

	/* trigger udev */
	xtimer->cl = class_create("axitimer");
	if (!xtimer->cl) {
		dev_err(NULL, "Failed to create a dev class\n");
		cdev_del(cdevice);
		unregister_chrdev_region(dev_num, DEVS_CNT);
		return;
	}

	xtimer->dev = NULL;
	xtimer->dev = device_create(xtimer->cl, NULL, dev_num, NULL, "%s%d", DEV_NAME, MINOR(dev_num));
	if (!xtimer->dev) {
		dev_err(NULL, "Failed to create device\n");
		class_destroy(xtimer->cl);
		cdev_del(cdevice);
		unregister_chrdev_region(dev_num, DEVS_CNT);
		return;
	}
	xtimer->dev_num = dev_num;
}

int axitimer_init(struct device_node *ernic_np)
{
	struct clk *clk;
	int ret = 0;
	void __iomem *timer_baseaddr;
	unsigned int timer_clock_freq;
	struct device_node *timer;

	timer = of_parse_phandle(ernic_np, "axitimer-handle", 0);
	if (!timer) {
		dev_err(NULL, "could not find DMA node\n");
		return -ENODEV;
	}
	timer_baseaddr = of_iomap(timer, 0);
	if (!timer_baseaddr) {
		pr_err("ERROR: invalid timer base address\n");
		return -ENXIO;
	}

	xtimer = kmalloc(sizeof(*xtimer), GFP_KERNEL);
	if(!xtimer) {
		pr_err("ERROR: Memory allocation failed\n");
		iounmap(timer_baseaddr);
		return -ENOMEM;
	}
	xtimer->timer_baseaddr = timer_baseaddr;

	ret = of_address_to_resource(timer, 0, &xtimer->res);
	if (ret) {
		pr_err("No memory address assigned to the region\n");
		ret = -EFAULT;
		goto end;
	}
	write_fn = timer_write32;
	read_fn = timer_read32;

	write_fn(TCSR_MDT, timer_baseaddr + TCSR0);
	if (!(read_fn(timer_baseaddr + TCSR0) & TCSR_MDT)) {
		write_fn = timer_write32_be;
		read_fn = timer_read32_be;
	}

	clk = of_clk_get(timer, 0);
	if (IS_ERR(clk)) {
		pr_err("ERROR: timer CCF input clock not found\n");
		/* If there is clock-frequency property than use it */
		of_property_read_u32(timer, "clock-frequency",
				    &timer_clock_freq);
	} else {
		timer_clock_freq = clk_get_rate(clk);
	}

	if (!timer_clock_freq) {
		pr_err("ERROR: Using CPU clock frequency\n");
		ret = -EINVAL;
		goto end;
	}

	xtimer->freq_div_hz = timer_clock_freq / HZ;
	clocksource_baseaddr = timer_baseaddr + TCSR0;

	ret = xilinx_clocksource_init(timer_clock_freq);
	if (ret)
		goto end;

	xilinx_timer0_start();
	create_char_device();
	return ret;

end:
	iounmap(xtimer->timer_baseaddr);
	kfree(xtimer);
	return ret;
}

void axitimer_exit(void)
{
	xilinx_timer0_stop();
	if (xtimer->dev)
		device_destroy(xtimer->cl, xtimer->dev_num);
	if (xtimer->cl)
		class_destroy(xtimer->cl);
	if (xtimer->cdevice)
		cdev_del(xtimer->cdevice);
	unregister_chrdev_region(xtimer->dev_num, DEVS_CNT);
	iounmap(xtimer->timer_baseaddr);
	kfree(xtimer);
	return;
}
