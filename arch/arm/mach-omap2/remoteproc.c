/*
 * Remote processor machine-specific module for OMAP4
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/remoteproc.h>
#include <linux/dma-contiguous.h>
#include <linux/dma-mapping.h>
#include <linux/platform_data/remoteproc-omap.h>
#include <linux/platform_data/iommu-omap.h>

#include "omap_device.h"
#include "omap_hwmod.h"
#include "soc.h"
#include "control.h"

/* forward declarations */
static int omap_rproc_device_enable(struct platform_device *pdev);
static int omap_rproc_device_shutdown(struct platform_device *pdev);

/*
 * Temporarily define the CMA base address explicitly.
 *
 * This will go away as soon as we have the IOMMU-based generic
 * DMA API in place.
 */
#define OMAP_RPROC_CMA_BASE_IPU	(0x99000000)
#define OMAP_RPROC_CMA_BASE_DSP	(0x98800000)

/*
 * These data structures define platform-specific information
 * needed for each supported remote processor.
 *
 * At this point we only support the remote dual M3 "Ducati" imaging
 * subsystem (aka "ipu"), but later on we'll also add support for the
 * DSP ("Tesla").
 */
static struct omap_rproc_pdata omap4_rproc_data[] = {
#ifdef CONFIG_OMAP_REMOTEPROC_DSP
	{
		.name		= "dsp_c0",
		.firmware	= "tesla-dsp.xe64T",
		.mbox_name	= "mbox-dsp",
		.oh_name	= "dsp",
		.set_bootaddr	= omap_ctrl_write_dsp_boot_addr,
	},
#endif
#ifdef CONFIG_OMAP_REMOTEPROC_IPU
	{
		.name		= "ipu_c0",
		.firmware	= "ducati-m3-core0.xem3",
		.mbox_name	= "mbox-ipu",
		.oh_name	= "ipu",
	},
#endif
};

static struct omap_iommu_arch_data omap4_rproc_iommu[] = {
#ifdef CONFIG_OMAP_REMOTEPROC_DSP
	{ .name = "mmu_dsp" },
#endif
#ifdef CONFIG_OMAP_REMOTEPROC_IPU
	{ .name = "mmu_ipu" },
#endif
};

#ifdef CONFIG_OMAP_REMOTEPROC_DSP
static struct platform_device omap4_tesla = {
	.name	= "omap-rproc",
	.id	= 0,
};
#endif
#ifdef CONFIG_OMAP_REMOTEPROC_IPU
static struct platform_device omap4_ducati = {
	.name	= "omap-rproc",
	.id	= 1,
};
#endif

static struct platform_device *omap4_rproc_devs[] __initdata = {
#ifdef CONFIG_OMAP_REMOTEPROC_DSP
	&omap4_tesla,
#endif
#ifdef CONFIG_OMAP_REMOTEPROC_IPU
	&omap4_ducati,
#endif
};

static int omap_rproc_device_shutdown(struct platform_device *pdev)
{
	int ret = -EINVAL;

	ret = omap_device_idle(pdev);
	if (ret)
		return ret;

	/* assert the resets for the processors */
	if (!strcmp(dev_name(&pdev->dev), "omap-rproc.0"))
		ret = omap_device_assert_hardreset(pdev, "dsp");
	else if (!strcmp(dev_name(&pdev->dev), "omap-rproc.1")) {
		ret = omap_device_assert_hardreset(pdev, "cpu0");
		if (ret)
			return ret;

		ret = omap_device_assert_hardreset(pdev, "cpu1");
		if (ret)
			return ret;
	} else
		pr_err("unsupported remoteproc\n");

	return ret;
}

static int omap_rproc_device_enable(struct platform_device *pdev)
{
	int ret = -EINVAL;

	/* release the resets for the processors */
	if (!strcmp(dev_name(&pdev->dev), "omap-rproc.0")) {
		ret = omap_device_deassert_hardreset(pdev, "dsp");
		if (ret)
			goto out;
	} else if (!strcmp(dev_name(&pdev->dev), "omap-rproc.1")) {
		ret = omap_device_deassert_hardreset(pdev, "cpu1");
		if (ret)
			goto out;

		ret = omap_device_deassert_hardreset(pdev, "cpu0");
		if (ret)
			goto out;
	} else {
		pr_err("unsupported remoteproc\n");
		goto out;
	}

	ret = omap_device_enable(pdev);

out:
	return ret;
}

void __init omap_rproc_reserve_cma(void)
{
#ifdef CONFIG_OMAP_REMOTEPROC_DSP
	{
	/* reserve CMA memory for OMAP4's dsp "tesla" remote processor */
	int ret = dma_declare_contiguous(&omap4_tesla.dev,
					CONFIG_OMAP_TESLA_CMA_SIZE,
					OMAP_RPROC_CMA_BASE_DSP , 0);
	if (ret)
		pr_err("dma_declare_contiguous failed for dsp %d\n", ret);
	}
#endif
#ifdef CONFIG_OMAP_REMOTEPROC_IPU
	{
	/* reserve CMA memory for OMAP4's M3 "ducati" remote processor */
	int ret = dma_declare_contiguous(&omap4_ducati.dev,
					CONFIG_OMAP_DUCATI_CMA_SIZE,
					OMAP_RPROC_CMA_BASE_IPU, 0);
	if (ret)
		pr_err("dma_declare_contiguous failed for ipu %d\n", ret);
	}
#endif
}

static int __init omap_rproc_init(void)
{
	struct omap_hwmod *oh[2];
	struct omap_device *od;
	int i, ret = 0, oh_count;

	/* names like ipu_cx/dsp_cx might show up on other OMAPs, too */
	if (!cpu_is_omap44xx())
		return 0;

	/* build the remote proc devices */
	for (i = 0; i < ARRAY_SIZE(omap4_rproc_data); i++) {
		const char *oh_name = omap4_rproc_data[i].oh_name;
		const char *oh_name_opt = omap4_rproc_data[i].oh_name_opt;
		struct platform_device *pdev = omap4_rproc_devs[i];
		oh_count = 0;

		oh[0] = omap_hwmod_lookup(oh_name);
		if (!oh[0]) {
			pr_err("could not look up %s\n", oh_name);
			continue;
		}
		oh_count++;

		/*
		 * ipu might have a secondary hwmod entry (for configurations
		 * where we want both M3 cores to be represented by a single
		 * device).
		 */
		if (oh_name_opt) {
			oh[1] = omap_hwmod_lookup(oh_name_opt);
			if (!oh[1]) {
				pr_err("could not look up %s\n", oh_name_opt);
				continue;
			}
			oh_count++;
		}

		omap4_rproc_data[i].device_enable = omap_rproc_device_enable;
		omap4_rproc_data[i].device_shutdown = omap_rproc_device_shutdown;

		device_initialize(&pdev->dev);

		/* Set dev_name early to allow dev_xxx in omap_device_alloc */
		dev_set_name(&pdev->dev, "%s.%d", pdev->name,  pdev->id);

		od = omap_device_alloc(pdev, oh, oh_count);
		if (!od) {
			dev_err(&pdev->dev, "omap_device_alloc failed\n");
			put_device(&pdev->dev);
			ret = PTR_ERR(od);
			continue;
		}

		ret = platform_device_add_data(pdev,
					&omap4_rproc_data[i],
					sizeof(struct omap_rproc_pdata));
		if (ret) {
			dev_err(&pdev->dev, "can't add pdata\n");
			omap_device_delete(od);
			put_device(&pdev->dev);
			continue;
		}

		/* attach the remote processor to its iommu device */
		pdev->dev.archdata.iommu = &omap4_rproc_iommu[i];

		ret = omap_device_register(pdev);
		if (ret) {
			dev_err(&pdev->dev, "omap_device_register failed\n");
			omap_device_delete(od);
			put_device(&pdev->dev);
			continue;
		}
	}

	return ret;
}
device_initcall(omap_rproc_init);
