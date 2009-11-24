#include <linux/init.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/bootinfo.h>

#define IDT434_REG_BASE ((volatile void *) KSEG1ADDR(0x18000000))

#define GPIOF 0x050000
#define GPIOC 0x050004
#define GPIOD 0x050008

#define GPIO_RDY (1 << 0x08)
#define GPIO_WPX (1 << 0x09)
#define GPIO_ALE (1 << 0x0a)
#define GPIO_CLE (1 << 0x0b)

#define DEV2BASE 0x010020

#define LO_WPX   (1 << 0)
#define LO_ALE   (1 << 1)
#define LO_CLE   (1 << 2)
#define LO_CEX   (1 << 3)
#define LO_FOFF  (1 << 5)
#define LO_SPICS (1 << 6)
#define LO_ULED  (1 << 7)

#define MEM32(x) *((volatile unsigned *) (x))
static void __iomem *p_nand;

extern void changeLatchU5(unsigned char orMask, unsigned char nandMask);

static int rb500_dev_ready(struct mtd_info *mtd)
{
	return MEM32(IDT434_REG_BASE + GPIOD) & GPIO_RDY;
}

/*
 * hardware specific access to control-lines
 *
 * ctrl:
 *     NAND_CLE: bit 2 -> bit 3 
 *     NAND_ALE: bit 3 -> bit 2
 */
static void rbmips_hwcontrol500(struct mtd_info *mtd, int cmd,
				unsigned int ctrl)
{
	struct nand_chip *chip = mtd->priv;
	unsigned char orbits, nandbits;

	if (ctrl & NAND_CTRL_CHANGE) {

		orbits = (ctrl & NAND_CLE) << 1;
		orbits |= (ctrl & NAND_ALE) >> 1;

		nandbits = (~ctrl & NAND_CLE) << 1;
		nandbits |= (~ctrl & NAND_ALE) >> 1;

		changeLatchU5(orbits, nandbits);
	}
	if (cmd != NAND_CMD_NONE)
		writeb(cmd, chip->IO_ADDR_W);

}

static struct mtd_partition partition_info[] = {
	{
	      name:"RouterBoard NAND Boot",
	      offset:0,
      size:4 * 1024 * 1024},
	{
	      name:"RouterBoard NAND Main",
	      offset:MTDPART_OFS_NXTBLK,
      size:MTDPART_SIZ_FULL}
};

static struct mtd_info rmtd;
static struct nand_chip rnand;

static unsigned init_ok = 0;

unsigned get_rbnand_block_size(void)
{
	if (init_ok)
		return rmtd.writesize;
	else
		return 0;
}

EXPORT_SYMBOL(get_rbnand_block_size);

int __init rbmips_init(void)
{
	int *b;
	memset(&rmtd, 0, sizeof(rmtd));
	memset(&rnand, 0, sizeof(rnand));

	printk("RB500 nand\n");
	changeLatchU5(LO_FOFF | LO_CEX,
		      LO_ULED | LO_ALE | LO_CLE | LO_WPX);
	rnand.cmd_ctrl = rbmips_hwcontrol500;

	rnand.dev_ready = rb500_dev_ready;
	rnand.IO_ADDR_W = (unsigned char *)
	    KSEG1ADDR(MEM32(IDT434_REG_BASE + DEV2BASE));
	rnand.IO_ADDR_R = rnand.IO_ADDR_W;

	p_nand = (void __iomem *) ioremap((void *) 0x18a20000, 0x1000);
	if (!p_nand) {
		printk("RBnand Unable ioremap buffer");
		return -ENXIO;
	}
	rnand.ecc.mode = NAND_ECC_SOFT;
	rnand.chip_delay = 25;
	rnand.options |= NAND_NO_AUTOINCR;
	rmtd.priv = &rnand;

	b = (int *) KSEG1ADDR(0x18010020);
	printk("dev2base 0x%08x mask 0x%08x c 0x%08x tc 0x%08x\n", b[0],
	       b[1], b[2], b[3]);

	if (nand_scan(&rmtd, 1) && nand_scan(&rmtd, 1)
	    && nand_scan(&rmtd, 1) && nand_scan(&rmtd, 1)) {
		printk("RBxxx nand device not found\n");
		iounmap((void *) p_nand);
		return -ENXIO;
	}

	add_mtd_partitions(&rmtd, partition_info, 2);
	init_ok = 1;
	return 0;
}

module_init(rbmips_init);
