/*
 *  Copyright (C) 2004 Florian Schirmer (jolt@tuxbox.org)
 *  Copyright (C) 2008 Michael Buesch <mb@bu3sch.de>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/ssb/ssb.h>

#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/irq_cpu.h>


extern struct ssb_bus ssb;


void plat_irq_dispatch(void)
{
	u32 cause;

	cause = read_c0_cause() & read_c0_status() & CAUSEF_IP;

	clear_c0_status(cause);

	if (cause & CAUSEF_IP7)
		do_IRQ(7);
	if (cause & CAUSEF_IP2)
		do_IRQ(2);
	if (cause & CAUSEF_IP3)
		do_IRQ(3);
	if (cause & CAUSEF_IP4)
		do_IRQ(4);
	if (cause & CAUSEF_IP5)
		do_IRQ(5);
	if (cause & CAUSEF_IP6)
		do_IRQ(6);
}

void __init arch_init_irq(void)
{
	mips_cpu_irq_init();
}

int pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	int res;

	res = ssb_pcibios_map_irq(dev, slot, pin);
	if (res < 0) {
		printk(KERN_ALERT "PCI: Failed to map IRQ of device %s\n",
		       dev->dev.bus_id);
		return 0;
	}
	/* IRQ-0 and IRQ-1 are software interrupts. */
	WARN_ON((res == 0) || (res == 1));

	return res;
}
