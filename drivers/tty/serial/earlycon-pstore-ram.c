// SPDX-License-Identifier: GPL-2.0
/* Early console into a persistent_ram zone: earlycon=pstoreram,<phys addr> */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <asm/early_ioremap.h>

#define PSTORE_RAM_SIG		0x43474244
#define PSTORE_RAM_ZONE_SIZE	SZ_256K

struct pstore_ram_buffer {
	u32 sig;
	u32 start;
	u32 size;
	u8 data[];
};

static struct pstore_ram_buffer __iomem *buf;
static u32 buf_cap;

static void pstoreram_write(struct console *con, const char *s, unsigned int n)
{
	u32 start = readl_relaxed(&buf->start);
	u32 size = readl_relaxed(&buf->size);

	while (n--) {
		writeb_relaxed(*s++, &buf->data[start]);
		if (++start == buf_cap)
			start = 0;
		if (size < buf_cap)
			size++;
	}
	writel_relaxed(start, &buf->start);
	writel(size, &buf->size);
}

static int __init pstoreram_setup(struct earlycon_device *dev, const char *opt)
{
	buf = (void __iomem *)early_ioremap(dev->port.mapbase, PSTORE_RAM_ZONE_SIZE);
	if (!buf)
		return -ENOMEM;

	buf_cap = PSTORE_RAM_ZONE_SIZE - sizeof(*buf);
	writel_relaxed(0, &buf->start);
	writel_relaxed(0, &buf->size);
	writel(PSTORE_RAM_SIG, &buf->sig);

	dev->con->write = pstoreram_write;
	return 0;
}
EARLYCON_DECLARE(pstoreram, pstoreram_setup);
