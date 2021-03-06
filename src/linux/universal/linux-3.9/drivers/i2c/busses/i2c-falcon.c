/*
 * Lantiq FALC(tm) ON - I2C bus adapter
 *
 * Parts based on i2c-designware.c and other i2c drivers from Linux 2.6.33
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* #define DEBUG */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h> /* for kzalloc, kfree */
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/gpio.h>

#include <falcon/lantiq_soc.h>

/* CURRENT ISSUES:
 * - no high speed support
 * - supports only master mode
 * - ten bit mode is not tested (no slave devices)
 */

/* mapping for access macros */
#define reg_r32(reg)		__raw_readl(reg)
#define reg_w32(val, reg)	__raw_writel(val, reg)
#define reg_w32_mask(clear, set, reg)	\
				reg_w32((reg_r32(reg) & ~(clear)) | (set), reg)
#define reg_r32_table(reg, idx) reg_r32(&((uint32_t *)&reg)[idx])
#define reg_w32_table(val, reg, idx) reg_w32(val, &((uint32_t *)&reg)[idx])
#define i2c	(priv->membase)
#include <falcon/i2c_reg.h>

#define DRV_NAME "i2c-falcon"
#define DRV_VERSION "1.01"

#define FALCON_I2C_BUSY_TIMEOUT		20 /* ms */

#ifdef DEBUG
#define FALCON_I2C_XFER_TIMEOUT		25*HZ
#else
#define FALCON_I2C_XFER_TIMEOUT		HZ
#endif
#if defined(DEBUG) && 0
#define PRINTK(arg...) printk(arg)
#else
#define PRINTK(arg...) do {} while (0)
#endif

#define FALCON_I2C_IMSC_DEFAULT_MASK	(I2C_IMSC_I2C_P_INT_EN | \
					 I2C_IMSC_I2C_ERR_INT_EN)

#define FALCON_I2C_ARB_LOST	(1 << 0)
#define FALCON_I2C_NACK		(1 << 1)
#define FALCON_I2C_RX_UFL	(1 << 2)
#define FALCON_I2C_RX_OFL	(1 << 3)
#define FALCON_I2C_TX_UFL	(1 << 4)
#define FALCON_I2C_TX_OFL	(1 << 5)

struct falcon_i2c {
	struct mutex mutex;

	enum {
		FALCON_I2C_MODE_100	= 1,
		FALCON_I2C_MODE_400	= 2,
		FALCON_I2C_MODE_3400	= 3
	} mode;				/* current speed mode */

	struct clk *clk;		/* clock input for i2c hardware block */
	struct gpon_reg_i2c __iomem *membase;	/* base of mapped registers */
	int irq_lb, irq_b, irq_err, irq_p;	/* last burst, burst, error,
						   protocol IRQs */

	struct i2c_adapter adap;
	struct device *dev;

	struct completion	cmd_complete;

	/* message transfer data */
	/* current message */
	struct i2c_msg		*current_msg;
	/* number of messages to handle */
	int			msgs_num;
	/* current buffer */
	u8			*msg_buf;
	/* remaining length of current buffer */
	u32			msg_buf_len;
	/* error status of the current transfer */
	int			msg_err;

	/* master status codes */
	enum {
		STATUS_IDLE,
		STATUS_ADDR,	/* address phase */
		STATUS_WRITE,
		STATUS_READ,
		STATUS_READ_END,
		STATUS_STOP
	} status;
};

static irqreturn_t falcon_i2c_isr(int irq, void *dev_id);

static inline void enable_burst_irq(struct falcon_i2c *priv)
{
	i2c_w32_mask(0, I2C_IMSC_LBREQ_INT_EN | I2C_IMSC_BREQ_INT_EN, imsc);
}
static inline void disable_burst_irq(struct falcon_i2c *priv)
{
	i2c_w32_mask(I2C_IMSC_LBREQ_INT_EN | I2C_IMSC_BREQ_INT_EN, 0, imsc);
}

static void prepare_msg_send_addr(struct falcon_i2c *priv)
{
	struct i2c_msg *msg = priv->current_msg;
	int rd = !!(msg->flags & I2C_M_RD);	/* extends to 0 or 1 */
	u16 addr = msg->addr;

	/* new i2c_msg */
	priv->msg_buf = msg->buf;
	priv->msg_buf_len = msg->len;
	if (rd)
		priv->status = STATUS_READ;
	else
		priv->status = STATUS_WRITE;

	/* send slave address */
	if (msg->flags & I2C_M_TEN) {
		i2c_w32(0xf0 | ((addr & 0x300) >> 7) | rd, txd);
		i2c_w32(addr & 0xff, txd);
	} else
		i2c_w32((addr & 0x7f) << 1 | rd, txd);
}

static void set_tx_len(struct falcon_i2c *priv)
{
	struct i2c_msg *msg = priv->current_msg;
	int len = (msg->flags & I2C_M_TEN) ? 2 : 1;

	PRINTK("set_tx_len %cX\n", (msg->flags & I2C_M_RD)?'R':'T');

	priv->status = STATUS_ADDR;

	if (!(msg->flags & I2C_M_RD)) {
		len += msg->len;
	} else {
		/* set maximum received packet size (before rx int!) */
		i2c_w32(msg->len, mrps_ctrl);
	}
	i2c_w32(len, tps_ctrl);
	enable_burst_irq(priv);
}

static int falcon_i2c_hw_init(struct i2c_adapter *adap)
{
	struct falcon_i2c *priv = i2c_get_adapdata(adap);

	/* disable bus */
	i2c_w32_mask(I2C_RUN_CTRL_RUN_EN, 0, run_ctrl);

#ifndef DEBUG
	/* set normal operation clock divider */
	i2c_w32(1 << I2C_CLC_RMC_OFFSET, clc);
#else
	/* for debugging a higher divider value! */
	i2c_w32(0xF0 << I2C_CLC_RMC_OFFSET, clc);
#endif

	/* set frequency */
	if (priv->mode == FALCON_I2C_MODE_100) {
		dev_dbg(priv->dev, "set standard mode (100 kHz)\n");
		i2c_w32(0, fdiv_high_cfg);
		i2c_w32((1 << I2C_FDIV_CFG_INC_OFFSET) |
			(499 << I2C_FDIV_CFG_DEC_OFFSET),
			fdiv_cfg);
	} else if (priv->mode == FALCON_I2C_MODE_400) {
		dev_dbg(priv->dev, "set fast mode (400 kHz)\n");
		i2c_w32(0, fdiv_high_cfg);
		i2c_w32((1 << I2C_FDIV_CFG_INC_OFFSET) |
			(124 << I2C_FDIV_CFG_DEC_OFFSET),
			fdiv_cfg);
	} else if (priv->mode == FALCON_I2C_MODE_3400) {
		dev_dbg(priv->dev, "set high mode (3.4 MHz)\n");
		i2c_w32(0, fdiv_cfg);
		/* TODO recalculate value for 100MHz input */
		i2c_w32((41 << I2C_FDIV_HIGH_CFG_INC_OFFSET) |
			(152 << I2C_FDIV_HIGH_CFG_DEC_OFFSET),
			fdiv_high_cfg);
	} else {
		dev_warn(priv->dev, "unknown mode\n");
		return -ENODEV;
	}

	/* configure fifo */
	i2c_w32(I2C_FIFO_CFG_TXFC | /* tx fifo as flow controller */
		I2C_FIFO_CFG_RXFC | /* rx fifo as flow controller */
		I2C_FIFO_CFG_TXFA_TXFA2 | /* tx fifo 4-byte aligned */
		I2C_FIFO_CFG_RXFA_RXFA2 | /* rx fifo 4-byte aligned */
		I2C_FIFO_CFG_TXBS_TXBS0 | /* tx fifo burst size is 1 word */
		I2C_FIFO_CFG_RXBS_RXBS0,  /* rx fifo burst size is 1 word */
		fifo_cfg);

	/* configure address */
	i2c_w32(I2C_ADDR_CFG_SOPE_EN |	/* generate stop when no more data in the
					   fifo */
		I2C_ADDR_CFG_SONA_EN |	/* generate stop when NA received */
		I2C_ADDR_CFG_MnS_EN |	/* we are master device */
		0,			/* our slave address (not used!) */
		addr_cfg);

	/* enable bus */
	i2c_w32_mask(0, I2C_RUN_CTRL_RUN_EN, run_ctrl);

	return 0;
}

static int falcon_i2c_wait_bus_not_busy(struct falcon_i2c *priv)
{
	int timeout = FALCON_I2C_BUSY_TIMEOUT;

	while ((i2c_r32(bus_stat) & I2C_BUS_STAT_BS_MASK)
				 != I2C_BUS_STAT_BS_FREE) {
		if (timeout <= 0) {
			dev_warn(priv->dev, "timeout waiting for bus ready\n");
			return -ETIMEDOUT;
		}
		timeout--;
		mdelay(1);
	}

	return 0;
}

static void falcon_i2c_tx(struct falcon_i2c *priv, int last)
{
	if (priv->msg_buf_len && priv->msg_buf) {
		i2c_w32(*priv->msg_buf, txd);

		if (--priv->msg_buf_len)
			priv->msg_buf++;
		else
			priv->msg_buf = NULL;
	} else
		last = 1;

	if (last) {
		disable_burst_irq(priv);
	}
}

static void falcon_i2c_rx(struct falcon_i2c *priv, int last)
{
	u32 fifo_stat,timeout;
	if (priv->msg_buf_len && priv->msg_buf) {
		timeout = 5000000;
		do {
			fifo_stat = i2c_r32(ffs_stat);
		} while (!fifo_stat && --timeout);
		if (!timeout) {
			last = 1;
			PRINTK("\nrx timeout\n");
			goto err;
		}
		while (fifo_stat) {
			*priv->msg_buf = i2c_r32(rxd);
			if (--priv->msg_buf_len)
				priv->msg_buf++;
			else {
				priv->msg_buf = NULL;
				last = 1;
				break;
			}
			#if 0
			fifo_stat = i2c_r32(ffs_stat);
			#else
			/* do not read more than burst size, otherwise no "last
			burst" is generated and the transaction is blocked! */
			fifo_stat = 0;
			#endif
		}
	} else {
		last = 1;
	}
err:
	if (last) {
		disable_burst_irq(priv);

		if (priv->status == STATUS_READ_END) {
			/* do the STATUS_STOP and complete() here, as sometimes
			   the tx_end is already seen before this is finished */
			priv->status = STATUS_STOP;
			complete(&priv->cmd_complete);
		} else {
			i2c_w32(I2C_ENDD_CTRL_SETEND, endd_ctrl);
			priv->status = STATUS_READ_END;
		}
	}
}

static void falcon_i2c_xfer_init(struct falcon_i2c *priv)
{
	/* enable interrupts */
	i2c_w32(FALCON_I2C_IMSC_DEFAULT_MASK, imsc);

	/* trigger transfer of first msg */
	set_tx_len(priv);
}

static void dump_msgs(struct i2c_msg msgs[], int num, int rx)
{
#if defined(DEBUG)
	int i, j;
	printk("Messages %d %s\n", num, rx ? "out" : "in");
	for (i = 0; i < num; i++) {
		printk("%2d %cX Msg(%d) addr=0x%X: ", i,
			(msgs[i].flags & I2C_M_RD)?'R':'T',
			msgs[i].len, msgs[i].addr);
		if (!(msgs[i].flags & I2C_M_RD) || rx) {
			for (j = 0; j < msgs[i].len; j++)
				printk("%02X ", msgs[i].buf[j]);
		}
		printk("\n");
	}
#endif
}

static void falcon_i2c_release_bus(struct falcon_i2c *priv)
{
	if ((i2c_r32(bus_stat) & I2C_BUS_STAT_BS_MASK) == I2C_BUS_STAT_BS_BM)
		i2c_w32(I2C_ENDD_CTRL_SETEND, endd_ctrl);
}

static int falcon_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			   int num)
{
	struct falcon_i2c *priv = i2c_get_adapdata(adap);
	int ret;

	dev_dbg(priv->dev, "xfer %u messages\n", num);
	dump_msgs(msgs, num, 0);

	mutex_lock(&priv->mutex);

	INIT_COMPLETION(priv->cmd_complete);
	priv->current_msg = msgs;
	priv->msgs_num = num;
	priv->msg_err = 0;
	priv->status = STATUS_IDLE;

	/* wait for the bus to become ready */
	ret = falcon_i2c_wait_bus_not_busy(priv);
	if (ret)
		goto done;

	while (priv->msgs_num) {
		/* start the transfers */
		falcon_i2c_xfer_init(priv);

		/* wait for transfers to complete */
		ret = wait_for_completion_interruptible_timeout(
			&priv->cmd_complete, FALCON_I2C_XFER_TIMEOUT);
		if (ret == 0) {
			dev_err(priv->dev, "controller timed out\n");
			falcon_i2c_hw_init(adap);
			ret = -ETIMEDOUT;
			goto done;
		} else if (ret < 0)
			goto done;

		if (priv->msg_err) {
			if (priv->msg_err & FALCON_I2C_NACK)
				ret = -ENXIO;
			else
				ret = -EREMOTEIO;
			goto done;
		}
		if (--priv->msgs_num) {
			priv->current_msg++;
		}
	}
	/* no error? */
	ret = num;

done:
	falcon_i2c_release_bus(priv);

	mutex_unlock(&priv->mutex);

	if (ret>=0)
		dump_msgs(msgs, num, 1);

	PRINTK("XFER ret %d\n", ret);
	return ret;
}

static irqreturn_t falcon_i2c_isr_burst(int irq, void *dev_id)
{
	struct falcon_i2c *priv = dev_id;
	struct i2c_msg *msg = priv->current_msg;
	int last = (irq == priv->irq_lb);

	if (last)
		PRINTK("LB ");
	else
		PRINTK("B ");

	if (msg->flags & I2C_M_RD) {
		switch (priv->status) {
		case STATUS_ADDR:
			PRINTK("X");
			prepare_msg_send_addr(priv);
			disable_burst_irq(priv);
			break;
		case STATUS_READ:
		case STATUS_READ_END:
			PRINTK("R");
			falcon_i2c_rx(priv, last);
			break;
		default:
			disable_burst_irq(priv);
			printk("Status R %d\n", priv->status);
			break;
		}
	} else {
		switch (priv->status) {
		case STATUS_ADDR:
			PRINTK("x");
			prepare_msg_send_addr(priv);
			break;
		case STATUS_WRITE:
			PRINTK("w");
			falcon_i2c_tx(priv, last);
			break;
		default:
			disable_burst_irq(priv);
			printk("Status W %d\n", priv->status);
			break;
		}
	}

	i2c_w32(I2C_ICR_BREQ_INT_CLR | I2C_ICR_LBREQ_INT_CLR, icr);
	return IRQ_HANDLED;
}

static void falcon_i2c_isr_prot(struct falcon_i2c *priv)
{
	u32 i_pro = i2c_r32(p_irqss);

	PRINTK("i2c-p");

	/* not acknowledge */
	if (i_pro & I2C_P_IRQSS_NACK) {
		priv->msg_err |= FALCON_I2C_NACK;
		PRINTK(" nack");
	}

	/* arbitration lost */
	if (i_pro & I2C_P_IRQSS_AL) {
		priv->msg_err |= FALCON_I2C_ARB_LOST;
		PRINTK(" arb-lost");
	}
	/* tx -> rx switch */
	if (i_pro & I2C_P_IRQSS_RX)
		PRINTK(" rx");

	/* tx end */
	if (i_pro & I2C_P_IRQSS_TX_END)
		PRINTK(" txend");
	PRINTK("\n");

	if (!priv->msg_err) {
		/* tx -> rx switch */
		if (i_pro & I2C_P_IRQSS_RX) {
			priv->status = STATUS_READ;
			enable_burst_irq(priv);
		}
		if (i_pro & I2C_P_IRQSS_TX_END) {
			if (priv->status == STATUS_READ)
				priv->status = STATUS_READ_END;
			else {
				disable_burst_irq(priv);
				priv->status = STATUS_STOP;
			}
		}
	}

	i2c_w32(i_pro, p_irqsc);
}

static irqreturn_t falcon_i2c_isr(int irq, void *dev_id)
{
	u32 i_raw, i_err=0;
	struct falcon_i2c *priv = dev_id;

	i_raw = i2c_r32(mis);
	PRINTK("i_raw 0x%08X\n", i_raw);

	/* error interrupt */
	if (i_raw & I2C_RIS_I2C_ERR_INT_INTOCC) {
		i_err = i2c_r32(err_irqss);
		PRINTK("i_err 0x%08X bus_stat 0x%04X\n",
			i_err, i2c_r32(bus_stat));

		/* tx fifo overflow (8) */
		if (i_err & I2C_ERR_IRQSS_TXF_OFL)
			priv->msg_err |= FALCON_I2C_TX_OFL;

		/* tx fifo underflow (4) */
		if (i_err & I2C_ERR_IRQSS_TXF_UFL)
			priv->msg_err |= FALCON_I2C_TX_UFL;

		/* rx fifo overflow (2) */
		if (i_err & I2C_ERR_IRQSS_RXF_OFL)
			priv->msg_err |= FALCON_I2C_RX_OFL;

		/* rx fifo underflow (1) */
		if (i_err & I2C_ERR_IRQSS_RXF_UFL)
			priv->msg_err |= FALCON_I2C_RX_UFL;

		i2c_w32(i_err, err_irqsc);
	}

	/* protocol interrupt */
	if (i_raw & I2C_RIS_I2C_P_INT_INTOCC)
		falcon_i2c_isr_prot(priv);

	if ((priv->msg_err) || (priv->status == STATUS_STOP))
		complete(&priv->cmd_complete);

	return IRQ_HANDLED;
}

static u32 falcon_i2c_functionality(struct i2c_adapter *adap)
{
	return	I2C_FUNC_I2C |
		I2C_FUNC_10BIT_ADDR |
		I2C_FUNC_SMBUS_EMUL;
}

static struct i2c_algorithm falcon_i2c_algorithm = {
	.master_xfer	= falcon_i2c_xfer,
	.functionality	= falcon_i2c_functionality,
};

static int falcon_i2c_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct falcon_i2c *priv;
	struct i2c_adapter *adap;
	struct resource *mmres, *ioarea,
			*irqres_lb, *irqres_b, *irqres_err, *irqres_p;
	struct clk *clk;

	dev_dbg(&pdev->dev, "probing\n");

	mmres = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irqres_lb = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
						 "i2c_lb");
	irqres_b = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "i2c_b");
	irqres_err = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
						  "i2c_err");
	irqres_p = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "i2c_p");

	if (!mmres || !irqres_lb || !irqres_b || !irqres_err || !irqres_p) {
		dev_err(&pdev->dev, "no resources\n");
		return -ENODEV;
	}

	clk = clk_get(&pdev->dev, "fpi");
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "failed to get fpi clk\n");
		return -ENOENT;
	}

	if (clk_get_rate(clk) != 100000000) {
		dev_err(&pdev->dev, "input clock is not 100MHz\n");
		return -ENOENT;
	}

	/* allocate private data */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		dev_err(&pdev->dev, "can't allocate private data\n");
		return -ENOMEM;
	}

	adap = &priv->adap;
	i2c_set_adapdata(adap, priv);
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_HWMON | I2C_CLASS_SPD;
	strlcpy(adap->name, DRV_NAME "-adapter", sizeof(adap->name));
	adap->algo = &falcon_i2c_algorithm;

	priv->mode = FALCON_I2C_MODE_100;
	priv->clk = clk;
	priv->dev = &pdev->dev;

	init_completion(&priv->cmd_complete);
	mutex_init(&priv->mutex);

	ret = ltq_gpio_request(107, 0, 0, 0, DRV_NAME":sda");
	if (ret) {
		dev_err(&pdev->dev, "I2C gpio 107 (sda) not available\n");
		ret = -ENXIO;
		goto err_free_priv;
	}
	ret = ltq_gpio_request(108, 0, 0, 0, DRV_NAME":scl");
	if (ret) {
		gpio_free(107);
		dev_err(&pdev->dev, "I2C gpio 108 (scl) not available\n");
		ret = -ENXIO;
		goto err_free_priv;
	}

	ioarea = request_mem_region(mmres->start, resource_size(mmres),
					 pdev->name);

	if (ioarea == NULL) {
		dev_err(&pdev->dev, "I2C region already claimed\n");
		ret = -ENXIO;
		goto err_free_gpio;
	}

	/* map memory */
	priv->membase = ioremap_nocache(mmres->start & ~KSEG1,
		resource_size(mmres));
	if (priv->membase == NULL) {
		ret = -ENOMEM;
		goto err_release_region;
	}

	priv->irq_lb = irqres_lb->start;
	ret = request_irq(priv->irq_lb, falcon_i2c_isr_burst, IRQF_DISABLED,
			  irqres_lb->name, priv);
	if (ret) {
		dev_err(&pdev->dev, "can't get last burst IRQ %d\n", irqres_lb->start);
		ret = -ENODEV;
		goto err_unmap_mem;
	}

	priv->irq_b = irqres_b->start;
	ret = request_irq(priv->irq_b, falcon_i2c_isr_burst, IRQF_DISABLED,
			  irqres_b->name, priv);
	if (ret) {
		dev_err(&pdev->dev, "can't get burst IRQ %d\n", irqres_b->start);
		ret = -ENODEV;
		goto err_free_lb_irq;
	}

	priv->irq_err = irqres_err->start;
	ret = request_irq(priv->irq_err, falcon_i2c_isr, IRQF_DISABLED,
			  irqres_err->name, priv);
	if (ret) {
		dev_err(&pdev->dev, "can't get error IRQ %d\n", irqres_err->start);
		ret = -ENODEV;
		goto err_free_b_irq;
	}

	priv->irq_p = irqres_p->start;
	ret = request_irq(priv->irq_p, falcon_i2c_isr, IRQF_DISABLED,
			  irqres_p->name, priv);
	if (ret) {
		dev_err(&pdev->dev, "can't get protocol IRQ %d\n", irqres_p->start);
		ret = -ENODEV;
		goto err_free_err_irq;
	}

	dev_dbg(&pdev->dev, "mapped io-space to %p\n", priv->membase);
	dev_dbg(&pdev->dev, "use IRQs %d, %d, %d, %d\n", irqres_lb->start,
	    irqres_b->start, irqres_err->start, irqres_p->start);

	/* add our adapter to the i2c stack */
	ret = i2c_add_numbered_adapter(adap);
	if (ret) {
		dev_err(&pdev->dev, "can't register I2C adapter\n");
		goto err_free_p_irq;
	}

	platform_set_drvdata(pdev, priv);
	i2c_set_adapdata(adap, priv);

	/* print module version information */
	dev_dbg(&pdev->dev, "module id=%u revision=%u\n",
		(i2c_r32(id) & I2C_ID_ID_MASK) >> I2C_ID_ID_OFFSET,
		(i2c_r32(id) & I2C_ID_REV_MASK) >> I2C_ID_REV_OFFSET);

	/* initialize HW */
	ret = falcon_i2c_hw_init(adap);
	if (ret) {
		dev_err(&pdev->dev, "can't configure adapter\n");
		goto err_remove_adapter;
	}

	dev_info(&pdev->dev, "version %s\n", DRV_VERSION);

	return 0;

err_remove_adapter:
	i2c_del_adapter(adap);
	platform_set_drvdata(pdev, NULL);

err_free_p_irq:
	free_irq(priv->irq_p, priv);

err_free_err_irq:
	free_irq(priv->irq_err, priv);

err_free_b_irq:
	free_irq(priv->irq_b, priv);

err_free_lb_irq:
	free_irq(priv->irq_lb, priv);

err_unmap_mem:
	iounmap(priv->membase);

err_release_region:
	release_mem_region(mmres->start, resource_size(mmres));

err_free_gpio:
	gpio_free(108);
	gpio_free(107);

err_free_priv:
	kfree(priv);

	return ret;
}

static int falcon_i2c_remove(struct platform_device *pdev)
{
	struct falcon_i2c *priv = platform_get_drvdata(pdev);
	struct resource *mmres;

	/* disable bus */
	i2c_w32_mask(I2C_RUN_CTRL_RUN_EN, 0, run_ctrl);

	/* remove driver */
	platform_set_drvdata(pdev, NULL);
	i2c_del_adapter(&priv->adap);

	free_irq(priv->irq_lb, priv);
	free_irq(priv->irq_b, priv);
	free_irq(priv->irq_err, priv);
	free_irq(priv->irq_p, priv);

	iounmap(priv->membase);

	gpio_free(108);
	gpio_free(107);

	kfree(priv);

	mmres = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(mmres->start, resource_size(mmres));

	dev_dbg(&pdev->dev, "removed\n");

	return 0;
}

static struct platform_driver falcon_i2c_driver = {
	.probe	= falcon_i2c_probe,
	.remove	= falcon_i2c_remove,
	.driver	= {
		.name	= DRV_NAME,
		.owner	= THIS_MODULE,
	},
};

static int __init falcon_i2c_init(void)
{
	int ret;

	ret = platform_driver_register(&falcon_i2c_driver);

	if (ret)
		pr_debug(DRV_NAME ": can't register platform driver\n");

	return ret;
}

static void __exit falcon_i2c_exit(void)
{
	platform_driver_unregister(&falcon_i2c_driver);
}

module_init(falcon_i2c_init);
module_exit(falcon_i2c_exit);

MODULE_DESCRIPTION("Lantiq FALC(tm) ON - I2C bus adapter");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
