/*
 * arch/arm/mach-ixp4xx/ixdp425-setup.c
 *
 * IXDP425/IXCDP1100 board-setup 
 *
 * Copyright (C) 2003-2005 MontaVista Software, Inc.
 *
 * Author: Deepak Saxena <dsaxena@plexity.net>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/serial_8250.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/i2c/at24.h>
#include <linux/io.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>

#include <asm/types.h>
#include <asm/setup.h>
#include <asm/memory.h>
#include <mach/hardware.h>
#include <asm/mach-types.h>
#include <asm/irq.h>
#include <asm/mach/arch.h>
#include <asm/mach/flash.h>
#include <asm/delay.h>

static struct flash_platform_data ixdp425_flash_data = {
	.map_name	= "cfi_probe",
	.width		= 2,
};

static struct resource ixdp425_flash_resource = {
	.flags		= IORESOURCE_MEM,
};

static struct platform_device ixdp425_flash = {
	.name		= "IXP4XX-Flash",
	.id		= 0,
	.dev		= {
		.platform_data = &ixdp425_flash_data,
	},
	.num_resources	= 1,
	.resource	= &ixdp425_flash_resource,
};

#if defined(CONFIG_MTD_NAND_PLATFORM) || \
    defined(CONFIG_MTD_NAND_PLATFORM_MODULE)

#ifdef CONFIG_MTD_PARTITIONS

static struct mtd_partition ixdp425_partitions[] = {
	{
		.name	= "ixp400 NAND FS 0",
		.offset	= 0,
		.size 	= SZ_8M
	}, {
		.name	= "ixp400 NAND FS 1",
		.offset	= MTDPART_OFS_APPEND,
		.size	= MTDPART_SIZ_FULL
	},
};
#endif

static void
ixdp425_flash_nand_cmd_ctrl(struct mtd_info *mtd, int cmd, unsigned int ctrl)
{
	struct nand_chip *this = mtd->priv;
	int offset = (int)this->priv;

	if (ctrl & NAND_CTRL_CHANGE) {
		if (ctrl & NAND_NCE) {
			gpio_line_set(IXDP425_NAND_NCE_PIN, IXP4XX_GPIO_LOW);
			udelay(5);
		} else
			gpio_line_set(IXDP425_NAND_NCE_PIN, IXP4XX_GPIO_HIGH);

		offset = (ctrl & NAND_CLE) ? IXDP425_NAND_CMD_BYTE : 0;
		offset |= (ctrl & NAND_ALE) ? IXDP425_NAND_ADDR_BYTE : 0;
		this->priv = (void *)offset;
	}

	if (cmd != NAND_CMD_NONE)
		writeb(cmd, this->IO_ADDR_W + offset);
}

static struct platform_nand_data ixdp425_flash_nand_data = {
	.chip = {
		.chip_delay		= 30,
#ifdef CONFIG_MTD_PARTITIONS
		.partitions	 	= ixdp425_partitions,
		.nr_partitions	 	= ARRAY_SIZE(ixdp425_partitions),
#endif
	},
	.ctrl = {
		.cmd_ctrl 		= ixdp425_flash_nand_cmd_ctrl
	}
};

static struct resource ixdp425_flash_nand_resource = {
	.flags		= IORESOURCE_MEM,
};

static struct platform_device ixdp425_flash_nand = {
	.name		= "gen_nand",
	.id		= -1,
	.dev		= {
		.platform_data = &ixdp425_flash_nand_data,
	},
	.num_resources	= 1,
	.resource	= &ixdp425_flash_nand_resource,
};
#endif	/* CONFIG_MTD_NAND_PLATFORM */

static struct ixp4xx_spi_pins ixdp425_spi_gpio_pins = {
	.spis_pin       = IXDP425_KSSPI_SELECT,
	.spic_pin       = IXDP425_KSSPI_CLOCK,
	.spid_pin       = IXDP425_KSSPI_TXD,
	.spiq_pin       = IXDP425_KSSPI_RXD
};

static struct platform_device ixdp425_spi_controller = {
    .name               = "IXP4XX-SPI",
	.id                 = 0,
	.dev                = {
		.platform_data  = &ixdp425_spi_gpio_pins,
	},
	.num_resources      = 0
};


static struct ixp4xx_i2c_pins ixdp425_i2c_gpio_pins = {
	.sda_pin	= IXDP425_SDA_PIN,
	.scl_pin	= IXDP425_SCL_PIN,
};

static struct platform_device ixdp425_i2c_controller = {
	.name		= "IXP4XX-I2C",
	.id		= 0,
	.dev		= {
		.platform_data = &ixdp425_i2c_gpio_pins,
	},
	.num_resources	= 0
};

static struct resource ixdp425_uart_resources[] = {
#ifndef CONFIG_TONZE
	{
		.start		= IXP4XX_UART1_BASE_PHYS,
		.end		= IXP4XX_UART1_BASE_PHYS + 0x0fff,
		.flags		= IORESOURCE_MEM
	},
#endif
	{
		.start		= IXP4XX_UART2_BASE_PHYS,
		.end		= IXP4XX_UART2_BASE_PHYS + 0x0fff,
		.flags		= IORESOURCE_MEM
	}
};

static struct plat_serial8250_port ixdp425_uart_data[] = {
#ifndef CONFIG_TONZE
	{
		.mapbase	= IXP4XX_UART1_BASE_PHYS,
		.membase	= (char *)IXP4XX_UART1_BASE_VIRT + REG_OFFSET,
		.irq		= IRQ_IXP4XX_UART1,
		.flags		= UPF_BOOT_AUTOCONF | UPF_SKIP_TEST,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
		.uartclk	= IXP4XX_UART_XTAL,
	},
#endif
	{
		.mapbase	= IXP4XX_UART2_BASE_PHYS,
		.membase	= (char *)IXP4XX_UART2_BASE_VIRT + REG_OFFSET,
		.irq		= IRQ_IXP4XX_UART2,
		.flags		= UPF_BOOT_AUTOCONF | UPF_SKIP_TEST,
		.iotype		= UPIO_MEM,
		.regshift	= 2,
		.uartclk	= IXP4XX_UART_XTAL,
	},
	{ },
};

static struct platform_device ixdp425_uart = {
	.name			= "serial8250",
	.id			= PLAT8250_DEV_PLATFORM,
	.dev.platform_data	= ixdp425_uart_data,
#ifndef CONFIG_TONZE
	.num_resources		= 2,
#else
	.num_resources		= 1,
#endif
	.resource		= ixdp425_uart_resources
};

static struct platform_device *ixdp425_devices[] __initdata = {
	&ixdp425_i2c_controller,
	&ixdp425_flash,
#if defined(CONFIG_MTD_NAND_PLATFORM) || \
    defined(CONFIG_MTD_NAND_PLATFORM_MODULE)
	&ixdp425_flash_nand,
#endif
	&ixdp425_uart,
	&ixdp425_spi_controller
};

static struct at24_platform_data avila_eeprom_info = {
	.byte_len	= 1024,
	.page_size	= 16,
	.flags		= AT24_FLAG_READONLY,
//	.setup		= at24_setup,
};

static struct i2c_board_info __initdata avila_i2c_board_info[] = {
	{
		I2C_BOARD_INFO("ds1672", 0x68),
	},
	{
		I2C_BOARD_INFO("ad7418", 0x28),
	},
	{
		I2C_BOARD_INFO("24c08", 0x51),
		.platform_data	= &avila_eeprom_info
	},
};

static struct resource avila_pata_resources[] = {
	{
		.flags	= IORESOURCE_MEM
	},
	{
		.flags	= IORESOURCE_MEM,
	},
	{
		.name	= "intrq",
		.start	= IRQ_IXP4XX_GPIO12,
		.end	= IRQ_IXP4XX_GPIO12,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct ixp4xx_pata_data avila_pata_data = {
	.cs0_bits	= 0xbfff0043,
	.cs1_bits	= 0xbfff0043,
};

static struct platform_device avila_pata = {
	.name			= "pata_ixp4xx_cf",
	.id			= 0,
	.dev.platform_data      = &avila_pata_data,
	.num_resources		= ARRAY_SIZE(avila_pata_resources),
	.resource		= avila_pata_resources,
};


static void __init ixdp425_init(void)
{
	ixp4xx_sys_init();

	ixdp425_flash_resource.start = IXP4XX_EXP_BUS_BASE(0);
	ixdp425_flash_resource.end =
		IXP4XX_EXP_BUS_BASE(0) + ixp4xx_exp_bus_size - 1;

#if defined(CONFIG_MTD_NAND_PLATFORM) || \
    defined(CONFIG_MTD_NAND_PLATFORM_MODULE)
	ixdp425_flash_nand_resource.start = IXP4XX_EXP_BUS_BASE(3),
	ixdp425_flash_nand_resource.end   = IXP4XX_EXP_BUS_BASE(3) + 0x10 - 1;

	gpio_line_config(IXDP425_NAND_NCE_PIN, IXP4XX_GPIO_OUT);

	/* Configure expansion bus for NAND Flash */
	*IXP4XX_EXP_CS3 = IXP4XX_EXP_BUS_CS_EN |
			  IXP4XX_EXP_BUS_STROBE_T(1) |	/* extend by 1 clock */
			  IXP4XX_EXP_BUS_CYCLES(0) |	/* Intel cycles */
			  IXP4XX_EXP_BUS_SIZE(0) |	/* 512bytes addr space*/
			  IXP4XX_EXP_BUS_WR_EN |
			  IXP4XX_EXP_BUS_BYTE_EN;	/* 8 bit data bus */
#endif

	if (cpu_is_ixp43x()) {
		ixdp425_uart.num_resources = 1;
		ixdp425_uart_data[1].flags = 0;
	}

	platform_add_devices(ixdp425_devices, ARRAY_SIZE(ixdp425_devices));
	avila_pata_resources[0].start = IXP4XX_EXP_BUS_BASE(1);
	avila_pata_resources[0].end = IXP4XX_EXP_BUS_END(1);

	avila_pata_resources[1].start = IXP4XX_EXP_BUS_BASE(2);
	avila_pata_resources[1].end = IXP4XX_EXP_BUS_END(2);

	avila_pata_data.cs0_cfg = IXP4XX_EXP_CS1;
	avila_pata_data.cs1_cfg = IXP4XX_EXP_CS2;

	platform_device_register(&avila_pata);

		i2c_register_board_info(0, avila_i2c_board_info,
				ARRAY_SIZE(avila_i2c_board_info));
}

#ifdef CONFIG_ARCH_IXDP425
MACHINE_START(IXDP425, "Intel IXDP425 Development Platform")
	/* Maintainer: MontaVista Software, Inc. */
	.map_io		= ixp4xx_map_io,
	.init_early	= ixp4xx_init_early,
	.init_irq	= ixp4xx_init_irq,
	.init_time	= ixp4xx_timer_init,
	.atag_offset	= 0x0100,
	.init_machine	= ixdp425_init,
#if defined(CONFIG_PCI)
	.dma_zone_size	= SZ_64M,
#endif
	.restart	= ixp4xx_restart,
MACHINE_END
#endif

#ifdef CONFIG_MACH_IXDP465
MACHINE_START(IXDP465, "Intel IXDP465 Development Platform")
	/* Maintainer: MontaVista Software, Inc. */
	.map_io		= ixp4xx_map_io,
	.init_early	= ixp4xx_init_early,
	.init_irq	= ixp4xx_init_irq,
	.init_time	= ixp4xx_timer_init,
	.atag_offset	= 0x0100,
	.init_machine	= ixdp425_init,
#if defined(CONFIG_PCI)
	.dma_zone_size	= SZ_64M,
#endif
	.restart	= ixp4xx_restart,
MACHINE_END
#endif

#ifdef CONFIG_ARCH_PRPMC1100
MACHINE_START(IXCDP1100, "Intel IXCDP1100 Development Platform")
	/* Maintainer: MontaVista Software, Inc. */
	.map_io		= ixp4xx_map_io,
	.init_early	= ixp4xx_init_early,
	.init_irq	= ixp4xx_init_irq,
	.init_time	= ixp4xx_timer_init,
	.atag_offset	= 0x0100,
	.init_machine	= ixdp425_init,
#if defined(CONFIG_PCI)
	.dma_zone_size	= SZ_64M,
#endif
	.restart	= ixp4xx_restart,
MACHINE_END
#endif

#ifdef CONFIG_MACH_KIXRP435
MACHINE_START(KIXRP435, "Intel KIXRP435 Reference Platform")
	/* Maintainer: MontaVista Software, Inc. */
	.map_io		= ixp4xx_map_io,
	.init_early	= ixp4xx_init_early,
	.init_irq	= ixp4xx_init_irq,
	.init_time	= ixp4xx_timer_init,
	.atag_offset	= 0x0100,
	.init_machine	= ixdp425_init,
#if defined(CONFIG_PCI)
	.dma_zone_size	= SZ_64M,
#endif
	.restart	= ixp4xx_restart,
MACHINE_END
#endif

