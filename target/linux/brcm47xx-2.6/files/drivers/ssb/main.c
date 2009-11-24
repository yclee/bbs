/*
 * Sonics Silicon Backplane
 * Subsystem core
 *
 * Copyright 2005, Broadcom Corporation
 * Copyright 2006, 2007, Michael Buesch <mb@bu3sch.de>
 *
 * Licensed under the GNU/GPL. See COPYING for details.
 */

#include "ssb_private.h"

#include <linux/delay.h>
#include <linux/ssb/ssb.h>
#include <linux/ssb/ssb_regs.h>

#ifdef CONFIG_SSB_PCIHOST
# include <linux/pci.h>
#endif

#ifdef CONFIG_SSB_PCMCIAHOST
# include <pcmcia/cs_types.h>
# include <pcmcia/cs.h>
# include <pcmcia/cistpl.h>
# include <pcmcia/ds.h>
#endif


MODULE_DESCRIPTION("Sonics Silicon Backplane driver");
MODULE_LICENSE("GPL");


static LIST_HEAD(attach_queue);
static LIST_HEAD(buses);
static unsigned int next_busnumber;
static DEFINE_MUTEX(buses_mutex);

static void ssb_buses_lock(void);
static void ssb_buses_unlock(void);


#ifdef CONFIG_SSB_PCIHOST
struct ssb_bus * ssb_pci_dev_to_bus(struct pci_dev *pdev)
{
	struct ssb_bus *bus;

	ssb_buses_lock();
	list_for_each_entry(bus, &buses, list) {
		if (bus->bustype == SSB_BUSTYPE_PCI &&
		    bus->host_pci == pdev)
			goto found;
	}
	bus = NULL;
found:
	ssb_buses_unlock();

	return bus;
}
#endif /* CONFIG_SSB_PCIHOST */

static struct ssb_device * ssb_device_get(struct ssb_device *dev)
{
	if (dev)
		get_device(dev->dev);
	return dev;
}

static void ssb_device_put(struct ssb_device *dev)
{
	if (dev)
		put_device(dev->dev);
}

static int ssb_bus_resume(struct ssb_bus *bus)
{
	int err;

	ssb_pci_xtal(bus, SSB_GPIO_XTAL | SSB_GPIO_PLL, 1);
	err = ssb_pcmcia_init(bus);
	if (err) {
		/* No need to disable XTAL, as we don't have one on PCMCIA. */
		return err;
	}
	ssb_chipco_resume(&bus->chipco);

	return 0;
}

static int ssb_device_resume(struct device *dev)
{
	struct ssb_device *ssb_dev = dev_to_ssb_dev(dev);
	struct ssb_driver *ssb_drv;
	struct ssb_bus *bus;
	int err = 0;

	bus = ssb_dev->bus;
	if (bus->suspend_cnt == bus->nr_devices) {
		err = ssb_bus_resume(bus);
		if (err)
			return err;
	}
	bus->suspend_cnt--;
	if (dev->driver) {
		ssb_drv = drv_to_ssb_drv(dev->driver);
		if (ssb_drv && ssb_drv->resume)
			err = ssb_drv->resume(ssb_dev);
		if (err)
			goto out;
	}
out:
	return err;
}

static void ssb_bus_suspend(struct ssb_bus *bus, pm_message_t state)
{
	ssb_chipco_suspend(&bus->chipco, state);
	ssb_pci_xtal(bus, SSB_GPIO_XTAL | SSB_GPIO_PLL, 0);

	/* Reset HW state information in memory, so that HW is
	 * completely reinitialized on resume. */
	bus->mapped_device = NULL;
#ifdef CONFIG_SSB_DRIVER_PCICORE
	bus->pcicore.setup_done = 0;
#endif
}

static int ssb_device_suspend(struct device *dev, pm_message_t state)
{
	struct ssb_device *ssb_dev = dev_to_ssb_dev(dev);
	struct ssb_driver *ssb_drv;
	struct ssb_bus *bus;
	int err = 0;

	if (dev->driver) {
		ssb_drv = drv_to_ssb_drv(dev->driver);
		if (ssb_drv && ssb_drv->suspend)
			err = ssb_drv->suspend(ssb_dev, state);
		if (err)
			goto out;
	}

	bus = ssb_dev->bus;
	bus->suspend_cnt++;
	if (bus->suspend_cnt == bus->nr_devices) {
		/* All devices suspended. Shutdown the bus. */
		ssb_bus_suspend(bus, state);
	}

out:
	return err;
}

#ifdef CONFIG_SSB_PCIHOST
int ssb_devices_freeze(struct ssb_bus *bus)
{
	struct ssb_device *dev;
	struct ssb_driver *drv;
	int err = 0;
	int i;
	pm_message_t state = PMSG_FREEZE;

	for (i = 0; i < bus->nr_devices; i++) {
		dev = &(bus->devices[i]);
		if (!dev->dev->driver)
			continue;
		if (!device_is_registered(dev->dev))
			continue;
		drv = drv_to_ssb_drv(dev->dev->driver);
		if (drv && drv->suspend) {
			err = drv->suspend(dev, state);
			if (err)
				goto out;
		}
	}
out:
	return err;
}

int ssb_devices_thaw(struct ssb_bus *bus)
{
	struct ssb_device *dev;
	struct ssb_driver *drv;
	int err = 0;
	int i;

	for (i = 0; i < bus->nr_devices; i++) {
		dev = &(bus->devices[i]);
		if (!dev->dev->driver)
			continue;
		if (!device_is_registered(dev->dev))
			continue;
		drv = drv_to_ssb_drv(dev->dev->driver);
		if (drv && drv->resume) {
			err = drv->resume(dev);
			if (err)
				goto out;
		}
	}
out:
	return err;
}
#endif /* CONFIG_SSB_PCIHOST */

static void ssb_device_shutdown(struct device *dev)
{
	struct ssb_device *ssb_dev = dev_to_ssb_dev(dev);
	struct ssb_driver *ssb_drv;

	if (!dev->driver)
		return;
	ssb_drv = drv_to_ssb_drv(dev->driver);
	if (ssb_drv && ssb_drv->shutdown)
		ssb_drv->shutdown(ssb_dev);
}

static int ssb_device_remove(struct device *dev)
{
	struct ssb_device *ssb_dev = dev_to_ssb_dev(dev);
	struct ssb_driver *ssb_drv = drv_to_ssb_drv(dev->driver);

	if (ssb_drv && ssb_drv->remove)
		ssb_drv->remove(ssb_dev);
	ssb_device_put(ssb_dev);

	return 0;
}

static int ssb_device_probe(struct device *dev)
{
	struct ssb_device *ssb_dev = dev_to_ssb_dev(dev);
	struct ssb_driver *ssb_drv = drv_to_ssb_drv(dev->driver);
	int err = 0;

	ssb_device_get(ssb_dev);
	if (ssb_drv && ssb_drv->probe)
		err = ssb_drv->probe(ssb_dev, &ssb_dev->id);
	if (err)
		ssb_device_put(ssb_dev);

	return err;
}

static int ssb_match_devid(const struct ssb_device_id *tabid,
			   const struct ssb_device_id *devid)
{
	if ((tabid->vendor != devid->vendor) &&
	    tabid->vendor != SSB_ANY_VENDOR)
		return 0;
	if ((tabid->coreid != devid->coreid) &&
	    tabid->coreid != SSB_ANY_ID)
		return 0;
	if ((tabid->revision != devid->revision) &&
	    tabid->revision != SSB_ANY_REV)
		return 0;
	return 1;
}

static int ssb_bus_match(struct device *dev, struct device_driver *drv)
{
	struct ssb_device *ssb_dev = dev_to_ssb_dev(dev);
	struct ssb_driver *ssb_drv = drv_to_ssb_drv(drv);
	const struct ssb_device_id *id;

	for (id = ssb_drv->id_table;
	     id->vendor || id->coreid || id->revision;
	     id++) {
		if (ssb_match_devid(id, &ssb_dev->id))
			return 1; /* found */
	}

	return 0;
}

static struct bus_type ssb_bustype = {
	.name		= NULL, /* Intentionally NULL to indicate early boot */
	.match		= ssb_bus_match,
	.probe		= ssb_device_probe,
	.remove		= ssb_device_remove,
	.shutdown	= ssb_device_shutdown,
	.suspend	= ssb_device_suspend,
	.resume		= ssb_device_resume,
};

#define is_early_boot()		(ssb_bustype.name == NULL)

static void ssb_buses_lock(void)
{
	if (!is_early_boot())
		mutex_lock(&buses_mutex);
}

static void ssb_buses_unlock(void)
{
	if (!is_early_boot())
		mutex_unlock(&buses_mutex);
}

static void ssb_devices_unregister(struct ssb_bus *bus)
{
	struct ssb_device *sdev;
	int i;

	for (i = bus->nr_devices - 1; i >= 0; i--) {
		sdev = &(bus->devices[i]);
		if (sdev->dev)
			device_unregister(sdev->dev);
	}
}

void ssb_bus_unregister(struct ssb_bus *bus)
{
	ssb_buses_lock();
	ssb_devices_unregister(bus);
	list_del(&bus->list);
	ssb_buses_unlock();

	/* ssb_pcmcia_exit(bus); */
	ssb_pci_exit(bus);
	ssb_iounmap(bus);
}
EXPORT_SYMBOL(ssb_bus_unregister);

static void ssb_release_dev(struct device *dev)
{
	struct __ssb_dev_wrapper *devwrap;

	devwrap = container_of(dev, struct __ssb_dev_wrapper, dev);
	kfree(devwrap);
}

static int ssb_devices_register(struct ssb_bus *bus)
{
	struct ssb_device *sdev;
	struct device *dev;
	struct __ssb_dev_wrapper *devwrap;
	int i, err = 0;
	int dev_idx = 0;

	for (i = 0; i < bus->nr_devices; i++) {
		sdev = &(bus->devices[i]);

		/* We don't register SSB-system devices to the kernel,
		 * as the drivers for them are built into SSB. */
		switch (sdev->id.coreid) {
		case SSB_DEV_CHIPCOMMON:
		case SSB_DEV_PCI:
		case SSB_DEV_PCIE:
		case SSB_DEV_PCMCIA:
		case SSB_DEV_MIPS:
		case SSB_DEV_MIPS_3302:
		case SSB_DEV_EXTIF:
			continue;
		}

		devwrap = kzalloc(sizeof(*devwrap), GFP_KERNEL);
		if (!devwrap) {
			ssb_printk(KERN_ERR PFX
				   "Could not allocate device\n");
			err = -ENOMEM;
			goto error;
		}
		dev = &devwrap->dev;
		devwrap->sdev = sdev;

		dev->release = ssb_release_dev;
		dev->bus = &ssb_bustype;
		snprintf(dev->bus_id, sizeof(dev->bus_id),
			 "ssb%u:%d", bus->busnumber, dev_idx);

		switch (bus->bustype) {
		case SSB_BUSTYPE_PCI:
#ifdef CONFIG_SSB_PCIHOST
			sdev->irq = bus->host_pci->irq;
			dev->parent = &bus->host_pci->dev;
#endif
			break;
		case SSB_BUSTYPE_PCMCIA:
#ifdef CONFIG_SSB_PCMCIAHOST
			dev->parent = &bus->host_pcmcia->dev;
#endif
			break;
		case SSB_BUSTYPE_SSB:
			break;
		}

		sdev->dev = dev;
		err = device_register(dev);
		if (err) {
			ssb_printk(KERN_ERR PFX
				   "Could not register %s\n",
				   dev->bus_id);
			/* Set dev to NULL to not unregister
			 * dev on error unwinding. */
			sdev->dev = NULL;
			kfree(devwrap);
			goto error;
		}
		dev_idx++;
	}

	return 0;
error:
	/* Unwind the already registered devices. */
	ssb_devices_unregister(bus);
	return err;
}

/* Needs ssb_buses_lock() */
static int ssb_attach_queued_buses(void)
{
	struct ssb_bus *bus, *n;
	int err = 0;
	int drop_them_all = 0;

	list_for_each_entry_safe(bus, n, &attach_queue, list) {
		if (drop_them_all) {
			list_del(&bus->list);
			continue;
		}
		/* Can't init the PCIcore in ssb_bus_register(), as that
		 * is too early in boot for embedded systems
		 * (no udelay() available). So do it here in attach stage.
		 */
		ssb_pcicore_init(&bus->pcicore);

		err = ssb_devices_register(bus);
		if (err) {
			drop_them_all = 1;
			list_del(&bus->list);
			continue;
		}
		list_move_tail(&bus->list, &buses);
	}

	return err;
}

static u16 ssb_ssb_read16(struct ssb_device *dev, u16 offset)
{
	struct ssb_bus *bus = dev->bus;

	offset += dev->core_index * SSB_CORE_SIZE;
	return readw(bus->mmio + offset);
}

static u32 ssb_ssb_read32(struct ssb_device *dev, u16 offset)
{
	struct ssb_bus *bus = dev->bus;

	offset += dev->core_index * SSB_CORE_SIZE;
	return readl(bus->mmio + offset);
}

static void ssb_ssb_write16(struct ssb_device *dev, u16 offset, u16 value)
{
	struct ssb_bus *bus = dev->bus;

	offset += dev->core_index * SSB_CORE_SIZE;
	writew(value, bus->mmio + offset);
}

static void ssb_ssb_write32(struct ssb_device *dev, u16 offset, u32 value)
{
	struct ssb_bus *bus = dev->bus;

	offset += dev->core_index * SSB_CORE_SIZE;
	writel(value, bus->mmio + offset);
}

static const struct ssb_bus_ops ssb_ssb_ops = {
	.read16		= ssb_ssb_read16,
	.read32		= ssb_ssb_read32,
	.write16	= ssb_ssb_write16,
	.write32	= ssb_ssb_write32,
};

static int ssb_fetch_invariants(struct ssb_bus *bus,
				int (*get_invariants)(struct ssb_bus *bus,
						      struct ssb_init_invariants *iv))
{
	struct ssb_init_invariants iv;
	int err;

	memset(&iv, 0, sizeof(iv));
	err = get_invariants(bus, &iv);
	if (err)
		goto out;
	memcpy(&bus->boardinfo, &iv.boardinfo, sizeof(iv.boardinfo));
	memcpy(&bus->sprom, &iv.sprom, sizeof(iv.sprom));
out:
	return err;
}

static int ssb_bus_register(struct ssb_bus *bus,
			    int (*get_invariants)(struct ssb_bus *bus,
			    			  struct ssb_init_invariants *iv),
			    unsigned long baseaddr)
{
	int err;

	spin_lock_init(&bus->bar_lock);
	INIT_LIST_HEAD(&bus->list);

	/* Powerup the bus */
	err = ssb_pci_xtal(bus, SSB_GPIO_XTAL | SSB_GPIO_PLL, 1);
	if (err)
		goto out;
	ssb_buses_lock();
	bus->busnumber = next_busnumber;
	/* Scan for devices (cores) */
	err = ssb_bus_scan(bus, baseaddr);
	if (err)
		goto err_disable_xtal;

	/* Init PCI-host device (if any) */
	err = ssb_pci_init(bus);
	if (err)
		goto err_unmap;
	/* Init PCMCIA-host device (if any) */
	err = ssb_pcmcia_init(bus);
	if (err)
		goto err_pci_exit;

	/* Initialize basic system devices (if available) */
	ssb_chipcommon_init(&bus->chipco);
	ssb_mipscore_init(&bus->mipscore);
	err = ssb_fetch_invariants(bus, get_invariants);
	if (err)
		goto err_pcmcia_exit;

	/* Queue it for attach */
	list_add_tail(&bus->list, &attach_queue);
	if (!is_early_boot()) {
		/* This is not early boot, so we must attach the bus now */
		err = ssb_attach_queued_buses();
		if (err)
			goto err_dequeue;
	}
	next_busnumber++;
	ssb_buses_unlock();

out:
	return err;

err_dequeue:
	list_del(&bus->list);
err_pcmcia_exit:
/*	ssb_pcmcia_exit(bus); */
err_pci_exit:
	ssb_pci_exit(bus);
err_unmap:
	ssb_iounmap(bus);
err_disable_xtal:
	ssb_buses_unlock();
	ssb_pci_xtal(bus, SSB_GPIO_XTAL | SSB_GPIO_PLL, 0);
	return err;
}

#ifdef CONFIG_SSB_PCIHOST
int ssb_bus_pcibus_register(struct ssb_bus *bus,
			    struct pci_dev *host_pci)
{
	int err;

	bus->bustype = SSB_BUSTYPE_PCI;
	bus->host_pci = host_pci;
	bus->ops = &ssb_pci_ops;

	err = ssb_bus_register(bus, ssb_pci_get_invariants, 0);
	if (!err) {
		ssb_printk(KERN_INFO PFX "Sonics Silicon Backplane found on "
			   "PCI device %s\n", host_pci->dev.bus_id);
	}

	return err;
}
EXPORT_SYMBOL(ssb_bus_pcibus_register);
#endif /* CONFIG_SSB_PCIHOST */

#ifdef CONFIG_SSB_PCMCIAHOST
int ssb_bus_pcmciabus_register(struct ssb_bus *bus,
			       struct pcmcia_device *pcmcia_dev,
			       unsigned long baseaddr)
{
	int err;

	bus->bustype = SSB_BUSTYPE_PCMCIA;
	bus->host_pcmcia = pcmcia_dev;
	bus->ops = &ssb_pcmcia_ops;

	err = ssb_bus_register(bus, ssb_pcmcia_get_invariants, baseaddr);
	if (!err) {
		ssb_printk(KERN_INFO PFX "Sonics Silicon Backplane found on "
			   "PCMCIA device %s\n", pcmcia_dev->devname);
	}

	return err;
}
EXPORT_SYMBOL(ssb_bus_pcmciabus_register);
#endif /* CONFIG_SSB_PCMCIAHOST */

int ssb_bus_ssbbus_register(struct ssb_bus *bus,
			    unsigned long baseaddr,
			    int (*get_invariants)(struct ssb_bus *bus,
			    			  struct ssb_init_invariants *iv))
{
	int err;

	bus->bustype = SSB_BUSTYPE_SSB;
	bus->ops = &ssb_ssb_ops;

	err = ssb_bus_register(bus, get_invariants, baseaddr);
	if (!err) {
		ssb_printk(KERN_INFO PFX "Sonics Silicon Backplane found at "
			   "address 0x%08lX\n", baseaddr);
	}

	return err;
}

int __ssb_driver_register(struct ssb_driver *drv, struct module *owner)
{
	drv->drv.name = drv->name;
	drv->drv.bus = &ssb_bustype;
	drv->drv.owner = owner;

	return driver_register(&drv->drv);
}
EXPORT_SYMBOL(__ssb_driver_register);

void ssb_driver_unregister(struct ssb_driver *drv)
{
	driver_unregister(&drv->drv);
}
EXPORT_SYMBOL(ssb_driver_unregister);

void ssb_set_devtypedata(struct ssb_device *dev, void *data)
{
	struct ssb_bus *bus = dev->bus;
	struct ssb_device *ent;
	int i;

	for (i = 0; i < bus->nr_devices; i++) {
		ent = &(bus->devices[i]);
		if (ent->id.vendor != dev->id.vendor)
			continue;
		if (ent->id.coreid != dev->id.coreid)
			continue;

		ent->devtypedata = data;
	}
}
EXPORT_SYMBOL(ssb_set_devtypedata);

static u32 clkfactor_f6_resolve(u32 v)
{
	/* map the magic values */
	switch (v) {
	case SSB_CHIPCO_CLK_F6_2:
		return 2;
	case SSB_CHIPCO_CLK_F6_3:
		return 3;
	case SSB_CHIPCO_CLK_F6_4:
		return 4;
	case SSB_CHIPCO_CLK_F6_5:
		return 5;
	case SSB_CHIPCO_CLK_F6_6:
		return 6;
	case SSB_CHIPCO_CLK_F6_7:
		return 7;
	}
	return 0;
}

/* Calculate the speed the backplane would run at a given set of clockcontrol values */
u32 ssb_calc_clock_rate(u32 plltype, u32 n, u32 m)
{
	u32 n1, n2, clock, m1, m2, m3, mc;

	n1 = (n & SSB_CHIPCO_CLK_N1);
	n2 = ((n & SSB_CHIPCO_CLK_N2) >> SSB_CHIPCO_CLK_N2_SHIFT);

	switch (plltype) {
	case SSB_PLLTYPE_6: /* 100/200 or 120/240 only */
		if (m & SSB_CHIPCO_CLK_T6_MMASK)
			return SSB_CHIPCO_CLK_T6_M0;
		return SSB_CHIPCO_CLK_T6_M1;
	case SSB_PLLTYPE_1: /* 48Mhz base, 3 dividers */
	case SSB_PLLTYPE_3: /* 25Mhz, 2 dividers */
	case SSB_PLLTYPE_4: /* 48Mhz, 4 dividers */
	case SSB_PLLTYPE_7: /* 25Mhz, 4 dividers */
		n1 = clkfactor_f6_resolve(n1);
		n2 += SSB_CHIPCO_CLK_F5_BIAS;
		break;
	case SSB_PLLTYPE_2: /* 48Mhz, 4 dividers */
		n1 += SSB_CHIPCO_CLK_T2_BIAS;
		n2 += SSB_CHIPCO_CLK_T2_BIAS;
		assert((n1 >= 2) && (n1 <= 7));
		assert((n2 >= 5) && (n2 <= 23));
		break;
	case SSB_PLLTYPE_5: /* 25Mhz, 4 dividers */
		return 100000000;
	default:
		assert(0);
	}

	switch (plltype) {
	case SSB_PLLTYPE_3: /* 25Mhz, 2 dividers */
	case SSB_PLLTYPE_7: /* 25Mhz, 4 dividers */
		clock = SSB_CHIPCO_CLK_BASE2 * n1 * n2;
		break;
	default:
		clock = SSB_CHIPCO_CLK_BASE1 * n1 * n2;
	}
	if (!clock)
		return 0;

	m1 = (m & SSB_CHIPCO_CLK_M1);
	m2 = ((m & SSB_CHIPCO_CLK_M2) >> SSB_CHIPCO_CLK_M2_SHIFT);
	m3 = ((m & SSB_CHIPCO_CLK_M3) >> SSB_CHIPCO_CLK_M3_SHIFT);
	mc = ((m & SSB_CHIPCO_CLK_MC) >> SSB_CHIPCO_CLK_MC_SHIFT);

	switch (plltype) {
	case SSB_PLLTYPE_1: /* 48Mhz base, 3 dividers */
	case SSB_PLLTYPE_3: /* 25Mhz, 2 dividers */
	case SSB_PLLTYPE_4: /* 48Mhz, 4 dividers */
	case SSB_PLLTYPE_7: /* 25Mhz, 4 dividers */
		m1 = clkfactor_f6_resolve(m1);
		if ((plltype == SSB_PLLTYPE_1) ||
		    (plltype == SSB_PLLTYPE_3))
			m2 += SSB_CHIPCO_CLK_F5_BIAS;
		else
			m2 = clkfactor_f6_resolve(m2);
		m3 = clkfactor_f6_resolve(m3);

		switch (mc) {
		case SSB_CHIPCO_CLK_MC_BYPASS:
			return clock;
		case SSB_CHIPCO_CLK_MC_M1:
			return (clock / m1);
		case SSB_CHIPCO_CLK_MC_M1M2:
			return (clock / (m1 * m2));
		case SSB_CHIPCO_CLK_MC_M1M2M3:
			return (clock / (m1 * m2 * m3));
		case SSB_CHIPCO_CLK_MC_M1M3:
			return (clock / (m1 * m3));
		}
		return 0;
	case SSB_PLLTYPE_2:
		m1 += SSB_CHIPCO_CLK_T2_BIAS;
		m2 += SSB_CHIPCO_CLK_T2M2_BIAS;
		m3 += SSB_CHIPCO_CLK_T2_BIAS;
		assert((m1 >= 2) && (m1 <= 7));
		assert((m2 >= 3) && (m2 <= 10));
		assert((m3 >= 2) && (m3 <= 7));

		if (!(mc & SSB_CHIPCO_CLK_T2MC_M1BYP))
			clock /= m1;
		if (!(mc & SSB_CHIPCO_CLK_T2MC_M2BYP))
			clock /= m2;
		if (!(mc & SSB_CHIPCO_CLK_T2MC_M3BYP))
			clock /= m3;
		return clock;
	default:
		assert(0);
	}
	return 0;
}

/* Get the current speed the backplane is running at */
u32 ssb_clockspeed(struct ssb_bus *bus)
{
	u32 rate;
	u32 plltype;
	u32 clkctl_n, clkctl_m;

	//TODO if EXTIF: PLLTYPE == 1, read n from clockcontrol_n, m from clockcontrol_sb

	if (bus->chipco.dev) {
		ssb_chipco_get_clockcontrol(&bus->chipco, &plltype,
					    &clkctl_n, &clkctl_m);
	} else
		return 0;

	if (bus->chip_id == 0x5365) {
		rate = 100000000;
	} else {
		rate = ssb_calc_clock_rate(plltype, clkctl_n, clkctl_m);
		if (plltype == SSB_PLLTYPE_3) /* 25Mhz, 2 dividers */
			rate /= 2;
	}

	return rate;
}
EXPORT_SYMBOL(ssb_clockspeed);

static u32 ssb_tmslow_reject_bitmask(struct ssb_device *dev)
{
	/* The REJECT bit changed position in TMSLOW between
	 * Backplane revisions. */
	switch (ssb_read32(dev, SSB_IDLOW) & SSB_IDLOW_SSBREV) {
	case SSB_IDLOW_SSBREV_22:
		return SSB_TMSLOW_REJECT_22;
	case SSB_IDLOW_SSBREV_23:
		return SSB_TMSLOW_REJECT_23;
	default:
		assert(0);
	}
	return (SSB_TMSLOW_REJECT_22 | SSB_TMSLOW_REJECT_23);
}

int ssb_device_is_enabled(struct ssb_device *dev)
{
	u32 val;
	u32 reject;

	reject = ssb_tmslow_reject_bitmask(dev);
	val = ssb_read32(dev, SSB_TMSLOW);
	val &= SSB_TMSLOW_CLOCK | SSB_TMSLOW_RESET | reject;

	return (val == SSB_TMSLOW_CLOCK);
}
EXPORT_SYMBOL(ssb_device_is_enabled);

void ssb_device_enable(struct ssb_device *dev, u32 core_specific_flags)
{
	u32 val;

	ssb_device_disable(dev, core_specific_flags);
	ssb_write32(dev, SSB_TMSLOW,
		    SSB_TMSLOW_RESET | SSB_TMSLOW_CLOCK |
		    SSB_TMSLOW_FGC | core_specific_flags);
	/* flush */
	ssb_read32(dev, SSB_TMSLOW);
	udelay(1);

	/* Clear SERR if set. This is a hw bug workaround. */
	if (ssb_read32(dev, SSB_TMSHIGH) & SSB_TMSHIGH_SERR)
		ssb_write32(dev, SSB_TMSHIGH, 0);

	val = ssb_read32(dev, SSB_IMSTATE);
	if (val & (SSB_IMSTATE_IBE | SSB_IMSTATE_TO)) {
		val &= ~(SSB_IMSTATE_IBE | SSB_IMSTATE_TO);
		ssb_write32(dev, SSB_IMSTATE, val);
	}

	ssb_write32(dev, SSB_TMSLOW,
		    SSB_TMSLOW_CLOCK | SSB_TMSLOW_FGC |
		    core_specific_flags);
	/* flush */
	ssb_read32(dev, SSB_TMSLOW);
	udelay(1);

	ssb_write32(dev, SSB_TMSLOW, SSB_TMSLOW_CLOCK |
		    core_specific_flags);
	/* flush */
	ssb_read32(dev, SSB_TMSLOW);
	udelay(1);
}
EXPORT_SYMBOL(ssb_device_enable);

static int ssb_wait_bit(struct ssb_device *dev, u16 reg, u32 bitmask,
			int timeout, int set)
{
	int i;
	u32 val;

	for (i = 0; i < timeout; i++) {
		val = ssb_read32(dev, reg);
		if (set) {
			if (val & bitmask)
				return 0;
		} else {
			if (!(val & bitmask))
				return 0;
		}
		udelay(10);
	}
	printk(KERN_ERR PFX "Timeout waiting for bitmask %08X on "
			    "register %04X to %s.\n",
	       bitmask, reg, (set ? "set" : "clear"));

	return -ETIMEDOUT;
}

void ssb_device_disable(struct ssb_device *dev, u32 core_specific_flags)
{
	u32 reject;

	if (ssb_read32(dev, SSB_TMSLOW) & SSB_TMSLOW_RESET)
		return;

	reject = ssb_tmslow_reject_bitmask(dev);
	ssb_write32(dev, SSB_TMSLOW, reject | SSB_TMSLOW_CLOCK);
	ssb_wait_bit(dev, SSB_TMSLOW, reject, 1000, 1);
	ssb_wait_bit(dev, SSB_TMSHIGH, SSB_TMSHIGH_BUSY, 1000, 0);
	ssb_write32(dev, SSB_TMSLOW,
		    SSB_TMSLOW_FGC | SSB_TMSLOW_CLOCK |
		    reject | SSB_TMSLOW_RESET |
		    core_specific_flags);
	/* flush */
	ssb_read32(dev, SSB_TMSLOW);
	udelay(1);

	ssb_write32(dev, SSB_TMSLOW,
		    reject | SSB_TMSLOW_RESET |
		    core_specific_flags);
	/* flush */
	ssb_read32(dev, SSB_TMSLOW);
	udelay(1);
}
EXPORT_SYMBOL(ssb_device_disable);

u32 ssb_dma_translation(struct ssb_device *dev)
{
	switch(dev->bus->bustype) {
	case SSB_BUSTYPE_SSB:
		return 0;
	case SSB_BUSTYPE_PCI:
	case SSB_BUSTYPE_PCMCIA:
		return SSB_PCI_DMA;
	}
	return 0;
}
EXPORT_SYMBOL(ssb_dma_translation);

int ssb_dma_set_mask(struct ssb_device *ssb_dev, u64 mask)
{
	struct device *dev = ssb_dev->dev;

#ifdef CONFIG_SSB_PCIHOST
	if (ssb_dev->bus->bustype == SSB_BUSTYPE_PCI &&
	    !dma_supported(dev, mask))
		return -EIO;
#endif
	dev->coherent_dma_mask = mask;
	dev->dma_mask = &dev->coherent_dma_mask;

	return 0;
}
EXPORT_SYMBOL(ssb_dma_set_mask);

int ssb_bus_may_powerdown(struct ssb_bus *bus)
{
	struct ssb_chipcommon *cc;
	int err;

	/* On buses where more than one core may be working
	 * at a time, we must not powerdown stuff if there are
	 * still cores that may want to run. */
	if (bus->bustype == SSB_BUSTYPE_SSB)
		return 0;

	cc = &bus->chipco;
	ssb_chipco_set_clockmode(cc, SSB_CLKMODE_SLOW);
	err = ssb_pci_xtal(bus, SSB_GPIO_XTAL | SSB_GPIO_PLL, 0);
	if (err)
		goto error;

	return 0;
error:
	ssb_printk(KERN_ERR PFX "Bus powerdown failed\n");
	return err;
}
EXPORT_SYMBOL(ssb_bus_may_powerdown);

int ssb_bus_powerup(struct ssb_bus *bus, int dynamic_pctl)
{
	struct ssb_chipcommon *cc;
	int err;
	enum ssb_clkmode mode;

	err = ssb_pci_xtal(bus, SSB_GPIO_XTAL | SSB_GPIO_PLL, 1);
	if (err)
		goto error;
	cc = &bus->chipco;
	mode = dynamic_pctl ? SSB_CLKMODE_DYNAMIC : SSB_CLKMODE_FAST;
	ssb_chipco_set_clockmode(cc, mode);

	return 0;
error:
	ssb_printk(KERN_ERR PFX "Bus powerup failed\n");
	return err;
}
EXPORT_SYMBOL(ssb_bus_powerup);

u32 ssb_admatch_base(u32 adm)
{
	u32 base = 0;

	switch (adm & SSB_ADM_TYPE) {
	case SSB_ADM_TYPE0:
		base = (adm & SSB_ADM_BASE0);
		break;
	case SSB_ADM_TYPE1:
		assert(!(adm & SSB_ADM_NEG)); /* unsupported */
		base = (adm & SSB_ADM_BASE1);
		break;
	case SSB_ADM_TYPE2:
		assert(!(adm & SSB_ADM_NEG)); /* unsupported */
		base = (adm & SSB_ADM_BASE2);
		break;
	default:
		assert(0);
	}

	return base;
}
EXPORT_SYMBOL(ssb_admatch_base);

u32 ssb_admatch_size(u32 adm)
{
	u32 size = 0;

	switch (adm & SSB_ADM_TYPE) {
	case SSB_ADM_TYPE0:
		size = ((adm & SSB_ADM_SZ0) >> SSB_ADM_SZ0_SHIFT);
		break;
	case SSB_ADM_TYPE1:
		assert(!(adm & SSB_ADM_NEG)); /* unsupported */
		size = ((adm & SSB_ADM_SZ1) >> SSB_ADM_SZ1_SHIFT);
		break;
	case SSB_ADM_TYPE2:
		assert(!(adm & SSB_ADM_NEG)); /* unsupported */
		size = ((adm & SSB_ADM_SZ2) >> SSB_ADM_SZ2_SHIFT);
		break;
	default:
		assert(0);
	}
	size = (1 << (size + 1));

	return size;
}
EXPORT_SYMBOL(ssb_admatch_size);

static int __init ssb_modinit(void)
{
	int err;

	ssb_bustype.name = "ssb";
	err = bus_register(&ssb_bustype);
	if (err)
		return err;

	/* Maybe we already registered some buses at early boot.
	 * Check for this and attach them
	 */
	ssb_buses_lock();
	err = ssb_attach_queued_buses();
	ssb_buses_unlock();
	if (err)
		bus_unregister(&ssb_bustype);

	return err;
}
subsys_initcall(ssb_modinit);

static void __exit ssb_modexit(void)
{
	bus_unregister(&ssb_bustype);
}
module_exit(ssb_modexit)
