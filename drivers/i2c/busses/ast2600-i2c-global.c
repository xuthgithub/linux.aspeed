/*
 *  Aspeed I2C Interrupt Controller.
 *
 *  Copyright (C) 2012-2017 ASPEED Technology Inc.
 *  Copyright 2017 IBM Corporation
 *  Copyright 2017 Google, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/clk-provider.h>
#include "ast2600-i2c-global.h"

struct aspeed_i2c_ic {
	void __iomem		*base;
	int			parent_irq;
	u32			i2c_irq_mask;
	struct reset_control	*rst;
	struct irq_domain	*irq_domain;
	int			bus_num;	
};

static const struct of_device_id aspeed_i2c_ic_of_match[] = {
	{ .compatible = "aspeed,ast2600-i2c-global", .data = (void *) 0},	
	{},
};

struct aspeed_i2c_base_clk {
	const char	*name;
	unsigned long	base_freq;
};

/* assign 4 base clock 
 * [31:24] base clk4 : 1M for 1KHz
 * [23:16] base clk3 : 4M for 400KHz	 
 * [15:08] base clk2 : 10M for 1MHz  
 * [00:07] base clk1 : 35M for 3.4MHz	 
*/
#define BASE_CLK_COUNT 4

static const struct aspeed_i2c_base_clk i2c_base_clk[BASE_CLK_COUNT] = {
	/* name	target_freq */
	{  "base_clk0",	1000000 },	//1M
	{  "base_clk1",	4000000 },	//4M
	{  "base_clk2",	10000000 },	//10M
	{  "base_clk3",	35000000 },	//35M
};

static u32 aspeed_i2c_ic_get_new_clk_divider(unsigned long	base_clk, struct device_node *node)
{
	struct clk_hw_onecell_data *onecell;
	struct clk_hw *hw;
	int err;
	u32 clk_divider = 0;
	int i, j;
	unsigned long base_freq;

	onecell = kzalloc(sizeof(*onecell) + (BASE_CLK_COUNT * sizeof(struct clk_hw *)), GFP_KERNEL);
	if (!onecell) {
		pr_err("allocate clk_hw \n");		
		return 0;
	}

	onecell->num = BASE_CLK_COUNT;

//	printk("base_clk %ld \n", base_clk);
	for(j = 0; j < BASE_CLK_COUNT; j++) {
//		printk("target clk : %ld \n", i2c_base_clk[j].base_freq);		
		for(i = 0; i < 0xff; i++) {
			/*
			 * i maps to div:
			 * 0x00: div 1
			 * 0x01: div 1.5
			 * 0x02: div 2
			 * 0x03: div 2.5
			 * 0x04: div 3
			 * ...
			 * 0xFE: div 128
			 * 0xFF: div 128.5
			 */
			base_freq = base_clk * 2 / (2 + i);
			if(base_freq <= i2c_base_clk[j].base_freq)
				break;
		}
		printk("i2cg - %s : %ld \n", i2c_base_clk[j].name, base_freq);
		hw = clk_hw_register_fixed_rate(NULL, i2c_base_clk[j].name, NULL, 0, base_freq);
		if (IS_ERR(hw)) {
			pr_err("failed to register input clock: %ld\n", PTR_ERR(hw));
			break;
		}
		onecell->hws[j] = hw;
		clk_divider |= (i << (8 * j));
	}

	err = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, onecell);
	if (err) {
		pr_err("failed to add i2c base clk provider: %d\n", err);
	}
	
	return clk_divider;
}

static int aspeed_i2c_global_probe(struct platform_device *pdev)
{
	struct aspeed_i2c_ic *i2c_ic;
	struct device_node *node = pdev->dev.of_node;
	const struct of_device_id *match;
	struct clk *parent_clk;	
	unsigned long	parent_clk_frequency;
	u32 clk_divider;
	int ret = 0;

	match = of_match_node(aspeed_i2c_ic_of_match, node);
	if (!match)
		return -ENOMEM;

	i2c_ic = kzalloc(sizeof(*i2c_ic), GFP_KERNEL);
	if (!i2c_ic)
		return -ENOMEM;

	i2c_ic->base = of_iomap(node, 0);
	if (!i2c_ic->base) {
		ret = -ENOMEM;
		goto err_free_ic;
	}

	i2c_ic->bus_num = (int) match->data;

	if (i2c_ic->bus_num) {
		i2c_ic->parent_irq = irq_of_parse_and_map(node, 0);
		if (i2c_ic->parent_irq < 0) {
			ret = i2c_ic->parent_irq;
			goto err_iounmap;
		}
	} 

	i2c_ic->rst = devm_reset_control_get_exclusive(&pdev->dev, NULL);

	if (IS_ERR(i2c_ic->rst)) {
		dev_dbg(&pdev->dev,
			"missing or invalid reset controller device tree entry");
	} else {
		//SCU I2C Reset 
		reset_control_assert(i2c_ic->rst);
		udelay(3);
		reset_control_deassert(i2c_ic->rst);
	}
	
	/* ast2600 init */
	writel(ASPEED_I2CG_SLAVE_PKT_NAK | ASPEED_I2CG_CTRL_NEW_REG | ASPEED_I2CG_CTRL_NEW_CLK_DIV, i2c_ic->base + ASPEED_I2CG_CTRL);
	parent_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(parent_clk))
		return PTR_ERR(parent_clk);
	parent_clk_frequency = clk_get_rate(parent_clk);
//	printk("parent_clk_frequency %ld \n", parent_clk_frequency);
	clk_divider = aspeed_i2c_ic_get_new_clk_divider(parent_clk_frequency, node);
	writel(clk_divider, i2c_ic->base + ASPEED_I2CG_CLK_DIV_CTRL);

	pr_info("i2c global registered \n");

	return 0;

err_iounmap:
	iounmap(i2c_ic->base);
err_free_ic:
	kfree(i2c_ic);
	return ret;
}

static struct platform_driver aspeed_i2c_ic_driver = {
	.probe  = aspeed_i2c_global_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = aspeed_i2c_ic_of_match,
	},
};

static int __init aspeed_i2c_global_init(void)
{
	return platform_driver_register(&aspeed_i2c_ic_driver);
}
postcore_initcall(aspeed_i2c_global_init);

MODULE_AUTHOR("Ryan Chen");
MODULE_DESCRIPTION("ASPEED I2C Global Driver");
MODULE_LICENSE("GPL v2");
