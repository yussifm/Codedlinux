// SPDX-License-Identifier: GPL-2.0-only

#include <linux/apple-mailbox.h>
#include <linux/apple-rtkit.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kfifo.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/types.h>

struct apple_compat_mbox;

struct apple_compat_mbox_priv {
                u8 ep;
                struct apple_compat_mbox *mbox;
                bool enabled;
};

struct apple_compat_mbox {
	struct device *dev;

	struct mbox_chan chan[0x100];
        struct apple_compat_mbox_priv chan_priv[0x100];

	struct mbox_controller controller;
        struct mbox_client client;
        struct mbox_chan *hw_chan;

        struct apple_rtkit *rtk;

        void __iomem *sart_regs;
};


static struct mbox_chan *apple_compat_mbox_of_xlate(struct mbox_controller *mbox,
					     const struct of_phandle_args *spec)
{
	struct apple_compat_mbox *apple_mbox = dev_get_drvdata(mbox->dev);
	u8 ep;

	if (spec->args_count != 1)
		return ERR_PTR(-EINVAL);

	ep = spec->args[0];
	apple_mbox->chan[ep].con_priv = &apple_mbox->chan_priv[ep];

	return &apple_mbox->chan[ep];
}

static int apple_compat_mbox_chan_send_data(struct mbox_chan *chan, void *data)
{
	struct apple_compat_mbox_priv *priv = chan->con_priv;
	struct apple_compat_mbox *apple_mbox = priv->mbox;
	struct apple_mbox_msg msg;

	msg.msg0 = (u64)data;
	msg.msg1 = priv->ep;

	return mbox_send_message(apple_mbox->hw_chan, &msg);
}

static int apple_compat_mbox_chan_startup(struct mbox_chan *chan)
{
	struct apple_compat_mbox_priv *priv = chan->con_priv;
	priv->enabled = true;
        apple_rtkit_start_ep(priv->mbox->rtk, priv->ep);
	return 0;
}

static void apple_compat_mbox_chan_shutdown(struct mbox_chan *chan)
{
	struct apple_compat_mbox_priv *priv = chan->con_priv;
	priv->enabled = false;
}

static bool apple_compat_mbox_chan_last_txdone(struct mbox_chan *chan)
{
	return true;
}

static const struct mbox_chan_ops apple_compat_mbox_ops = {
	.send_data = &apple_compat_mbox_chan_send_data,
	.last_tx_done = apple_compat_mbox_chan_last_txdone,
	.startup = &apple_compat_mbox_chan_startup,
	.shutdown = &apple_compat_mbox_chan_shutdown,
};

static int dummy_shmem_verify(void *cookie, dma_addr_t addr, size_t len)
{
        return 0;
}

static void rtk_got_msg(void *cookie, u8 endpoint, u64 message)
{
        struct apple_compat_mbox *mbox = cookie;

        if (!mbox->chan_priv[endpoint].enabled)
                return;

        mbox_chan_received_data(&mbox->chan[endpoint], (void *)message);
}

#define APPLE_SART_CONFIG(idx) (0x00 + 4 * (idx))
#define APPLE_SART_CONFIG_FLAGS GENMASK(31, 24)
#define APPLE_SART_CONFIG_SIZE GENMASK(23, 0)
#define APPLE_SART_CONFIG_SIZE_SHIFT 12

#define APPLE_SART_PADDR(idx) (0x40 + 4 * (idx))
#define APPLE_SART_PADDR_SHIFT 12

#define APPLE_SART_MAX_ENTRIES 16

static void *sart_alloc(void *cookie, size_t size, dma_addr_t *dma_handle,
			    gfp_t flag)
{
        struct apple_compat_mbox *mbox = cookie;
        u32 buffer_config;
        int i;
        void *cpu_addr = dma_alloc_coherent(mbox->dev, size, dma_handle, flag);

	buffer_config = FIELD_PREP(APPLE_SART_CONFIG_FLAGS, 0xff);
 	buffer_config |=
 		FIELD_PREP(APPLE_SART_CONFIG_SIZE,
 			   size >> APPLE_SART_CONFIG_SIZE_SHIFT);

 	for (i = 0; i < APPLE_SART_MAX_ENTRIES; ++i) {
 		u32 config =
 			readl(mbox->sart_regs + APPLE_SART_CONFIG(i));
 		if (FIELD_GET(APPLE_SART_CONFIG_FLAGS, config) != 0)
 			continue;

 		writel((*dma_handle) >> APPLE_SART_PADDR_SHIFT,
 		       mbox->sart_regs + APPLE_SART_PADDR(i));
 		writel(buffer_config,
 		       mbox->sart_regs + APPLE_SART_CONFIG(i));
 		break;
 	}

        return cpu_addr;
}

static struct apple_rtkit_ops shmem_rtkit_ops =
{
        .shmem_owner = APPLE_RTKIT_SHMEM_OWNER_RTKIT,
        .shmem_verify = dummy_shmem_verify,
        .recv_message = rtk_got_msg,
};

static struct apple_rtkit_ops sart_rtkit_ops =
{
        .shmem_owner = APPLE_RTKIT_SHMEM_OWNER_LINUX,
        .shmem_alloc = sart_alloc,
        .recv_message = rtk_got_msg,
};

static int apple_compat_mbox_probe(struct platform_device *pdev)
{
	int ret;
	struct apple_compat_mbox *mbox;
	struct device *dev = &pdev->dev;
        struct resource *res, *sart_res;

        dev_err(dev, "this is a hack, please don't use it.");
        add_taint(TAINT_CRAP, LOCKDEP_STILL_OK);

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	platform_set_drvdata(pdev, mbox);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc");
	if (!res)
		return -EINVAL;

        sart_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sart");
        if (sart_res) {
                mbox->sart_regs = devm_ioremap_resource(dev, sart_res);
                if (IS_ERR(mbox->sart_regs))
                        return PTR_ERR(mbox->sart_regs);
        }

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	mbox->dev = dev;

        if (sart_res)
                mbox->rtk = apple_rtkit_init(dev, mbox, res, "mbox", &sart_rtkit_ops);
        else
                mbox->rtk = apple_rtkit_init(dev, mbox, res, "mbox", &shmem_rtkit_ops);

        apple_rtkit_boot_wait(mbox->rtk);

	mbox->controller.dev = mbox->dev;
	mbox->controller.num_chans = 0x100;
	mbox->controller.chans = mbox->chan;
	mbox->controller.ops = &apple_compat_mbox_ops;
	mbox->controller.of_xlate = &apple_compat_mbox_of_xlate;
	mbox->controller.txdone_poll = true;

	return devm_mbox_controller_register(dev, &mbox->controller);
}

static const struct of_device_id apple_compat_mbox_of_match[] = {
	{ .compatible = "apple,t8103-compat-mailbox", },
	{}
};
MODULE_DEVICE_TABLE(of, apple_compat_mbox_of_match);

static int apple_compat_mbox_remove(struct platform_device *pdev)
{
	//struct apple_compat_mbox *apple_mbox = platform_get_drvdata(pdev);
	return 0;
}

static void apple_compat_mbox_shutdown(struct platform_device *pdev)
{
	apple_compat_mbox_remove(pdev);
}

static struct platform_driver apple_compat_mbox_driver = {
	.driver = {
		.name = "apple-compat-mailbox",
		.of_match_table = apple_compat_mbox_of_match,
	},
	.probe = apple_compat_mbox_probe,
	.remove = apple_compat_mbox_remove,
	.shutdown = apple_compat_mbox_shutdown,
};
module_platform_driver(apple_compat_mbox_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("HACK: Apple mailbox compat layer");
