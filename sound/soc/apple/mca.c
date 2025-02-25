#define USE_RXB_FOR_CAPTURE

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>

/* relative to cluster base */
#define REG_STATUS		0x0
#define STATUS_MCLK_EN		BIT(0)
#define REG_MCLK_CONF		0x4
#define MCLK_CONF_DIV		GENMASK(11, 8)

#define REG_SYNCGEN_STATUS	0x100
#define SYNCGEN_STATUS_EN	BIT(0)
#define REG_SYNCGEN_MCLK_SEL	0x104
#define SYNCGEN_MCLK_SEL	GENMASK(3, 0)
#define REG_SYNCGEN_HI_PERIOD	0x108
#define REG_SYNCGEN_LO_PERIOD	0x10c

#define REG_PORT_ENABLES	0x600
#define PORT_ENABLES_CLOCKS	GENMASK(2, 1)
#define PORT_ENABLES_TX_DATA	BIT(3)
#define REG_PORT_CLOCK_SEL	0x604
#define PORT_CLOCK_SEL		GENMASK(11, 8)
#define REG_PORT_DATA_SEL	0x608
#define PORT_DATA_SEL_TXA(cl)	(1 << ((cl)*2))
#define PORT_DATA_SEL_TXB(cl)	(2 << ((cl)*2))

#define REG_INTSTATE		0x700
#define REG_INTMASK		0x704

/* bases of serdes units (relative to cluster) */
#define CLUSTER_RXA_OFF	0x200
#define CLUSTER_TXA_OFF	0x300
#define CLUSTER_RXB_OFF	0x400
#define CLUSTER_TXB_OFF	0x500

#define CLUSTER_TX_OFF	CLUSTER_TXA_OFF

#ifndef USE_RXB_FOR_CAPTURE
#define CLUSTER_RX_OFF	CLUSTER_RXA_OFF
#else
#define CLUSTER_RX_OFF	CLUSTER_RXB_OFF
#endif

/* relative to serdes unit base */
#define REG_SERDES_STATUS	0x00
#define SERDES_STATUS_EN	BIT(0)
#define SERDES_STATUS_RST	BIT(1)
#define REG_TX_SERDES_CONF	0x04
#define REG_RX_SERDES_CONF	0x08
#define SERDES_CONF_NCHANS	GENMASK(3, 0)
#define SERDES_CONF_WIDTH_MASK	GENMASK(8, 4)
#define SERDES_CONF_WIDTH_16BIT 0x40
#define SERDES_CONF_WIDTH_20BIT 0x80
#define SERDES_CONF_WIDTH_24BIT 0xc0
#define SERDES_CONF_WIDTH_32BIT 0x100
#define SERDES_CONF_BCLK_POL	0x400
#define SERDES_CONF_LSB_FIRST	0x800
#define SERDES_CONF_UNK1	BIT(12)
#define SERDES_CONF_UNK2	BIT(13)
#define SERDES_CONF_UNK3	BIT(14)
#define SERDES_CONF_NO_DATA_FEEDBACK	BIT(14)
#define SERDES_CONF_SYNC_SEL	GENMASK(18, 16)
#define SERDES_CONF_SOME_RST	BIT(19)
#define REG_TX_SERDES_BITSTART	0x08
#define REG_RX_SERDES_BITSTART	0x0c
#define REG_TX_SERDES_SLOTMASK	0x0c
#define REG_RX_SERDES_SLOTMASK	0x10
#define REG_RX_SERDES_PORT	0x04

/* relative to switch base */
#define REG_DMA_ADAPTER_A(cl)	(0x8000 * (cl))
#define REG_DMA_ADAPTER_B(cl)	(0x8000 * (cl) + 0x4000)
#define DMA_ADAPTER_TX_LSB_PAD	GENMASK(4, 0)
#define DMA_ADAPTER_TX_NCHANS	GENMASK(6, 5)
#define DMA_ADAPTER_RX_MSB_PAD	GENMASK(12, 8)
#define DMA_ADAPTER_RX_NCHANS	GENMASK(14, 13)
#define DMA_ADAPTER_NCHANS	GENMASK(22, 20)

#define SWITCH_STRIDE 0x8000
#define CLUSTER_STRIDE 0x4000

#define MAX_NCLUSTERS 6

struct mca_dai {
	struct mca_route *in_route;
	unsigned int tdm_slots;
	unsigned int tdm_slot_width;
	unsigned int tdm_tx_mask;
	unsigned int tdm_rx_mask;
	unsigned long set_sysclk;
	u32 fmt_bitstart;
	bool fmt_bclk_inv;
};

struct mca_cluster {
	int no;
	struct mca_data *host;
	struct device *pd_dev;
	struct clk *clk_parent;
	struct dma_chan *dma_chans[SNDRV_PCM_STREAM_LAST + 1];
	struct mca_dai port;
};

#define mca_dai_to_cluster(dai) \
		container_of(dai, struct mca_cluster, port)

struct mca_data {
	struct device *dev;

	__iomem void *base;
	__iomem void *switch_base;

	struct device *pd_dev;
	struct device_link *pd_link;

	int nclusters;
	struct mca_cluster clusters[];
};

struct mca_route {
	struct mca_data *host;

	struct clk *clk_parent;
	bool clocks_in_use[SNDRV_PCM_STREAM_LAST + 1];

	struct device_link *pd_link;

	/*
	 * Cluster selectors for different facilities
	 * that constitute the 'route'
	 */
	int clock;
	int syncgen;
	int serdes;

	int ndais;
	struct mca_dai *dais[];
};

static struct mca_route *mca_route_for_rtd(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *dai = asoc_rtd_to_cpu(rtd, 0);
	struct mca_data *mca = snd_soc_dai_get_drvdata(dai);
	return mca->clusters[dai->id].port.in_route;
}

static struct mca_dai *mca_dai_for_soc_dai(struct snd_soc_dai *dai)
{
	struct mca_data *mca = snd_soc_dai_get_drvdata(dai);
	return &mca->clusters[dai->id].port;
}

static u32 mca_peek(struct mca_data *mca, int cluster, int regoffset)
{
	int offset = (CLUSTER_STRIDE * cluster) + regoffset;

	return readl_relaxed(mca->base + offset);
}

static void mca_poke(struct mca_data *mca, int cluster,
				int regoffset, u32 val)
{
	int offset = (CLUSTER_STRIDE * cluster) + regoffset;
	dev_dbg(mca->dev, "regs: %x <- %x\n", offset, val);
	writel_relaxed(val, mca->base + offset);
}

static void mca_modify(struct mca_data *mca, int cluster,
				int regoffset, u32 mask, u32 val)
{
	int offset = (CLUSTER_STRIDE * cluster) + regoffset;
	__iomem void *p = mca->base + offset;
	u32 newval = (val & mask) | (readl_relaxed(p) & ~mask);
	dev_dbg(mca->dev, "regs: %x <- %x\n", offset, newval);
	writel_relaxed(newval, p);
}

static int mca_reset_dais(struct mca_route *route,
		struct snd_pcm_substream *substream, int cmd)
{
	struct mca_data *mca = route->host;
	bool is_tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	int serdes_unit = is_tx ? CLUSTER_TX_OFF : CLUSTER_RX_OFF;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		mca_modify(mca, route->serdes,
			serdes_unit + REG_SERDES_STATUS,
			SERDES_STATUS_EN | SERDES_STATUS_RST,
			SERDES_STATUS_RST);
		mca_modify(mca, route->serdes,
			serdes_unit +
			(is_tx ? REG_TX_SERDES_CONF : REG_RX_SERDES_CONF),
			SERDES_CONF_SOME_RST, SERDES_CONF_SOME_RST);
		(void)mca_peek(mca, route->serdes,
			serdes_unit +
			(is_tx ? REG_TX_SERDES_CONF : REG_RX_SERDES_CONF));
		mca_modify(mca, route->serdes,
			serdes_unit +
			(is_tx ? REG_TX_SERDES_CONF : REG_RX_SERDES_CONF),
			SERDES_STATUS_RST, 0);
		WARN_ON(mca_peek(mca, route->serdes, REG_SERDES_STATUS)
					& SERDES_STATUS_RST);

		dev_dbg(mca->dev, "trigger reset\n");
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int mca_trigger_dais(struct mca_route *route,
		struct snd_pcm_substream *substream, int cmd)
{
	struct mca_data *mca = route->host;
	bool is_tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	int serdes_unit = is_tx ? CLUSTER_TX_OFF : CLUSTER_RX_OFF;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		mca_modify(mca, route->serdes,
			serdes_unit + REG_SERDES_STATUS,
			SERDES_STATUS_EN | SERDES_STATUS_RST,
			SERDES_STATUS_EN);

		dev_dbg(mca->dev, "trigger start\n");
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		mca_modify(mca, route->serdes,
			serdes_unit + REG_SERDES_STATUS,
			SERDES_STATUS_EN, 0);

		dev_dbg(mca->dev, "trigger stop\n");
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static bool mca_clocks_in_use(struct mca_route *route)
{
	int stream;

	for_each_pcm_streams(stream)
		if (route->clocks_in_use[stream])
			return true;
	return false;
}

static int mca_prepare(struct snd_soc_component *component,
		struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct mca_route *route = mca_route_for_rtd(rtd);
	struct mca_data *mca = route->host;
	struct mca_cluster *cluster;

	int ret;

	if (!mca_clocks_in_use(route)) {
		ret = clk_prepare_enable(route->clk_parent);
		if (ret) {
			dev_err(mca->dev, "unable to enable parent clock %d: %d\n",
				route->clock, ret);
			return ret;
		}

		/*
		 * We only prop-up PD of the syncgen cluster. That is okay
		 * in combination with the way we are constructing 'routes'
		 * where only single cluster needs powering up.
		 */
		cluster = &mca->clusters[route->syncgen];
		route->pd_link = device_link_add(rtd->dev, cluster->pd_dev,
				DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME |
					DL_FLAG_RPM_ACTIVE);
		if (!route->pd_link) {
			dev_err(mca->dev, "unable to prop-up cluster's power domain "
					"(cluster %d)\n", route->syncgen);
			clk_disable_unprepare(route->clk_parent);
			return -EINVAL;
		}

		mca_poke(mca, route->syncgen, REG_SYNCGEN_MCLK_SEL,
			route->clock + 1);
		mca_modify(mca, route->syncgen,
			REG_SYNCGEN_STATUS, 
			SYNCGEN_STATUS_EN, SYNCGEN_STATUS_EN);
		mca_modify(mca, route->clock,
			REG_STATUS,
			STATUS_MCLK_EN, STATUS_MCLK_EN);
	}

	route->clocks_in_use[substream->stream] = true;

	return 0;
}

static int mca_hw_free(struct snd_soc_component *component,
			struct snd_pcm_substream *substream)
{
	struct mca_route *route = mca_route_for_rtd(asoc_substream_to_rtd(substream));
	struct mca_data *mca = route->host;

	if (!mca_clocks_in_use(route))
		return 0; /* Nothing to do */

	route->clocks_in_use[substream->stream] = false;

	if (!mca_clocks_in_use(route)) {
		mca_modify(mca, route->syncgen,
			REG_SYNCGEN_STATUS,
			SYNCGEN_STATUS_EN, 0);
		mca_modify(mca, route->clock,
			REG_STATUS,
			STATUS_MCLK_EN, 0);

		device_link_del(route->pd_link);
		clk_disable_unprepare(route->clk_parent);
	}

	return 0;
}

#define div_ceil(A, B) ((A)/(B) + ((A)%(B) ? 1 : 0))

static int mca_configure_serdes(struct mca_data *mca, int cluster, int serdes_unit,
				unsigned int mask, int slots, int nchans, int slot_width, bool is_tx, int port)
{
	u32 serdes_conf;

	serdes_conf = FIELD_PREP(SERDES_CONF_NCHANS, max(slots, 1) - 1);

	switch (slot_width) {
	case 16:
		serdes_conf |= SERDES_CONF_WIDTH_16BIT;
		break;
	case 20:
		serdes_conf |= SERDES_CONF_WIDTH_20BIT;
		break;
	case 24:
		serdes_conf |= SERDES_CONF_WIDTH_24BIT;
		break;
	case 32:
		serdes_conf |= SERDES_CONF_WIDTH_32BIT;
		break;
	default:
		goto err;
	}

	mca_modify(mca, cluster,
		serdes_unit + (is_tx ? REG_TX_SERDES_CONF : REG_RX_SERDES_CONF),
		SERDES_CONF_WIDTH_MASK | SERDES_CONF_NCHANS, serdes_conf);

	if (is_tx) {
		mca_poke(mca, cluster,
			serdes_unit + REG_TX_SERDES_SLOTMASK,
			0xffffffff);
		/*
		 * TODO: Actually consider where the hot bits
		 * are placed in the mask, instead of assuming
		 * it's the bottom bits.
		 */
		mca_poke(mca, cluster,
			serdes_unit + REG_TX_SERDES_SLOTMASK + 0x4,
			~((u32) mask & ((1 << nchans) - 1)));
		mca_poke(mca, cluster,
			serdes_unit + REG_TX_SERDES_SLOTMASK + 0x8,
			0xffffffff);
		mca_poke(mca, cluster,
			serdes_unit + REG_TX_SERDES_SLOTMASK + 0xc,
			~((u32) mask));
	} else {
		mca_poke(mca, cluster,
			serdes_unit + REG_RX_SERDES_SLOTMASK,
			0xffffffff);
		mca_poke(mca, cluster,
			serdes_unit + REG_RX_SERDES_SLOTMASK + 0x4,
			~((u32) mask));
		mca_poke(mca, cluster,
			serdes_unit + REG_RX_SERDES_PORT,
			1 << port);
	}

	return 0;

err:
	dev_err(mca->dev, "unsupported SERDES configuration requested (mask=0x%x slots=%d slot_width=%d)\n",
			mask, slots, slot_width);
	return -EINVAL;
}

static int mca_dai_set_tdm_slot(struct snd_soc_dai *dai, unsigned int tx_mask,
				unsigned int rx_mask, int slots, int slot_width)
{
	struct mca_dai *mdai = mca_dai_for_soc_dai(dai);

	mdai->tdm_slots = slots;
	mdai->tdm_slot_width = slot_width;
	mdai->tdm_tx_mask = tx_mask;
	mdai->tdm_rx_mask = rx_mask;

	return 0;
}

static int mca_dai_set_fmt(struct snd_soc_dai *dai,
				unsigned int fmt)
{
	struct mca_data *mca = snd_soc_dai_get_drvdata(dai);
	struct mca_dai *mdai = mca_dai_for_soc_dai(dai);
	struct mca_route *route = mdai->in_route;
	bool bclk_inv = false, fpol_inv = false;
	u32 bitstart;

	if (WARN_ON(route))
		return -EBUSY;

	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) !=
			SND_SOC_DAIFMT_CBC_CFC)
		goto err;

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		fpol_inv = 0;
		bitstart = 1;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		fpol_inv = 1;
		bitstart = 0;
		break;
	default:
		goto err;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_IF:
	case SND_SOC_DAIFMT_IB_IF:
		fpol_inv ^= 1;
		break;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
	case SND_SOC_DAIFMT_NB_IF:
		bclk_inv = true;
	}

	if (!fpol_inv)
		goto err;

	mdai->fmt_bitstart = bitstart;
	mdai->fmt_bclk_inv = bclk_inv;

	return 0;

err:
	dev_err(mca->dev, "unsupported DAI format (0x%x) requested\n", fmt);
	return -EINVAL;
}

static int mca_dai_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				unsigned int freq, int dir)
{
	int ret;

	struct mca_dai *mdai = mca_dai_for_soc_dai(dai);
	struct mca_route *route = mdai->in_route;

	if (freq == mdai->set_sysclk)
		return 0;

	if (mca_clocks_in_use(route))
		return -EBUSY;

	ret = clk_set_rate(route->clk_parent, freq);
	if (!ret)
		mdai->set_sysclk = freq;
	return ret;
}

static const struct snd_soc_dai_ops mca_dai_ops = {
	.set_fmt = mca_dai_set_fmt,
	.set_sysclk = mca_dai_set_sysclk,
	.set_tdm_slot = mca_dai_set_tdm_slot,
};

static int mca_set_runtime_hwparams(struct snd_soc_component *component,
				struct snd_pcm_substream *substream, struct dma_chan *chan)
{
	struct device *dma_dev = chan->device->dev;
	struct snd_dmaengine_dai_dma_data dma_data = {};
	int ret;

	struct snd_pcm_hardware hw;

	memset(&hw, 0, sizeof(hw));

	hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			SNDRV_PCM_INFO_INTERLEAVED;
	hw.periods_min = 2;
	hw.periods_max = UINT_MAX;
	hw.period_bytes_min = 256;
	hw.period_bytes_max = dma_get_max_seg_size(dma_dev);
	hw.buffer_bytes_max = SIZE_MAX;
	hw.fifo_size = 16;

	ret = snd_dmaengine_pcm_refine_runtime_hwparams(substream,
						&dma_data, &hw, chan);

	if (ret)
		return ret;

	return snd_soc_set_runtime_hwparams(substream, &hw);
}

static int mca_pcm_open(struct snd_soc_component *component,
		struct snd_pcm_substream *substream)
{
	struct mca_data *mca = snd_soc_component_get_drvdata(component);
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct mca_route *route = mca_route_for_rtd(rtd);
	struct dma_chan *chan = mca->clusters[route->serdes].dma_chans[substream->stream];
	int ret, i;

	if (WARN_ON(!route))
		return -EINVAL;

	for (i = 0; i < route->ndais; i++) {
		int dai_no = mca_dai_to_cluster(route->dais[i])->no;

		mca_poke(mca, dai_no, REG_PORT_ENABLES,
				PORT_ENABLES_CLOCKS | PORT_ENABLES_TX_DATA);
		mca_poke(mca, dai_no, REG_PORT_CLOCK_SEL,
				FIELD_PREP(PORT_CLOCK_SEL, route->syncgen + 1));
		mca_poke(mca, dai_no, REG_PORT_DATA_SEL,
				PORT_DATA_SEL_TXA(route->serdes));
	}

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		mca_modify(mca, route->serdes, CLUSTER_TX_OFF + REG_TX_SERDES_CONF,
				SERDES_CONF_UNK1 | SERDES_CONF_UNK2 | SERDES_CONF_UNK3,
				SERDES_CONF_UNK1 | SERDES_CONF_UNK2 | SERDES_CONF_UNK3);
		mca_modify(mca, route->serdes, CLUSTER_TX_OFF + REG_TX_SERDES_CONF,
				SERDES_CONF_SYNC_SEL,
				FIELD_PREP(SERDES_CONF_SYNC_SEL, route->syncgen + 1));
		break;

	case SNDRV_PCM_STREAM_CAPTURE:
		mca_modify(mca, route->serdes, CLUSTER_RX_OFF + REG_RX_SERDES_CONF,
				SERDES_CONF_UNK1 | SERDES_CONF_UNK2 | SERDES_CONF_UNK3
				| SERDES_CONF_NO_DATA_FEEDBACK,
				SERDES_CONF_UNK1 | SERDES_CONF_UNK2
				| SERDES_CONF_NO_DATA_FEEDBACK);
		mca_modify(mca, route->serdes, CLUSTER_RX_OFF + REG_RX_SERDES_CONF,
				SERDES_CONF_SYNC_SEL,
				FIELD_PREP(SERDES_CONF_SYNC_SEL, route->syncgen + 1));
		break;

	default:
		break;
	}

	ret = mca_set_runtime_hwparams(component, substream, chan);
	if (ret)
		return ret;

	return snd_dmaengine_pcm_open(substream, chan);
}

static int mca_hw_params_dma(struct snd_soc_component *component,
			struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params)
{
	struct dma_chan *chan = snd_dmaengine_pcm_get_chan(substream);
	struct dma_slave_config slave_config;
	int ret;

	memset(&slave_config, 0, sizeof(slave_config));
	ret = snd_hwparams_to_dma_slave_config(substream, params, &slave_config);
	if (ret < 0)
		return ret;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		slave_config.dst_port_window_size = min((int) params_channels(params), 4);
	else
		slave_config.src_port_window_size = min((int) params_channels(params), 4);

	return dmaengine_slave_config(chan, &slave_config);
}

static int mca_get_dais_tdm_slots(struct mca_route *route, bool is_tx,
				int *slot_width, int *slots, int *mask)
{
	struct mca_dai *mdai;
	unsigned int tdm_slot_width, tdm_tx_mask, tdm_rx_mask;
	unsigned int tdm_slots = 0;
	int i;

#define __pick_up_dai_tdm_param(param) 			\
	{						\
		if (tdm_slots && mdai->param != param) 	\
			return -EINVAL;			\
		param = mdai->param;			\
	}

	for (i = 0; i < route->ndais; i++) {
		mdai = route->dais[i];

		if (mdai->tdm_slots) {
			if (is_tx) {
				__pick_up_dai_tdm_param(tdm_tx_mask);
			} else {
				__pick_up_dai_tdm_param(tdm_rx_mask);
			}

			__pick_up_dai_tdm_param(tdm_slot_width);
			__pick_up_dai_tdm_param(tdm_slots);
		}
	}

	if (tdm_slots) {
		*slots      = tdm_slots;
		*slot_width = tdm_slot_width;
		*mask        = is_tx ? tdm_tx_mask : tdm_rx_mask;
	}

	return 0;
}

static int mca_get_dais_sysclk(struct mca_route *route, unsigned long *sysclk)
{
	struct mca_dai *mdai;
	unsigned long set_sysclk = 0;
	int i;

	for (i = 0; i < route->ndais; i++) {
		mdai = route->dais[i];

		if (!mdai->set_sysclk)
			continue;

		if (set_sysclk && mdai->set_sysclk != set_sysclk)
			return -EINVAL;

		set_sysclk = mdai->set_sysclk;
	}

	if (set_sysclk)
		*sysclk = set_sysclk;

	return 0;
}

static int mca_hw_params_dais(struct mca_route *route,
			struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params)
{
	struct mca_data *mca = route->host;
	struct device *dev = route->host->dev;
	bool is_tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned int samp_rate = params_rate(params);
	bool refine_tdm = false;
	unsigned int tdm_slots, tdm_slot_width, tdm_mask;
	unsigned long bclk_ratio;
	unsigned long sysclk;
	u32 regval, pad;
	int ret, dai_no, nchans_ceiled;

	tdm_slot_width = 0;
	ret = mca_get_dais_tdm_slots(route, is_tx,
		&tdm_slot_width, &tdm_slots, &tdm_mask);

	if (ret < 0) {
		dev_err(dev, "bad dai TDM settings\n");
		return ret;
	}

	if (!tdm_slot_width) {
		/*
		 * We were not given TDM settings from above, set initial
		 * guesses which will later be refined.
		 */
		tdm_slot_width = params_width(params);
		tdm_slots = params_channels(params);
		refine_tdm = true;
	}

	sysclk = 0;
	ret = mca_get_dais_sysclk(route, &sysclk);

	if (ret < 0) {
		dev_err(dev, "bad dai sysclk settings\n");
		return ret;
	}

	if (sysclk) {
		bclk_ratio = sysclk / samp_rate;
	} else {
		bclk_ratio = tdm_slot_width * tdm_slots;
	}	

	if (refine_tdm) {
		int nchannels = params_channels(params);

		if (nchannels > 2) {
			dev_err(dev, "nchannels > 2 and no TDM\n");
			return -EINVAL;
		}

		if ((bclk_ratio % nchannels) != 0) {
			dev_err(dev, "bclk ratio (%ld) not divisible by nchannels (%d)\n",
					bclk_ratio, nchannels);
			return -EINVAL;
		}

		tdm_slot_width = bclk_ratio / nchannels;

		if (tdm_slot_width > 32 && nchannels == 1)
			tdm_slot_width = 32;

		if (tdm_slot_width < params_width(params)) {
			dev_err(dev, "TDM slots too narrow tdm=%d params=%d\n",
					tdm_slot_width, params_width(params));
			return -EINVAL;
		}

		tdm_mask = (1 << tdm_slots) - 1;
	}

	dai_no = mca_dai_to_cluster(route->dais[0])->no;

	ret = mca_configure_serdes(mca, route->serdes, is_tx ? CLUSTER_TX_OFF : CLUSTER_RX_OFF,
				tdm_mask, tdm_slots, params_channels(params),
				tdm_slot_width, is_tx, dai_no);
	if (ret)
		return ret;
	
	pad = 32 - params_width(params);

	/*
	 * TODO: Here the register semantics aren't clear.
	 */
	nchans_ceiled = min((int) params_channels(params), 4);
	regval = FIELD_PREP(DMA_ADAPTER_NCHANS, nchans_ceiled)
			| FIELD_PREP(DMA_ADAPTER_TX_NCHANS, 0x2)
			| FIELD_PREP(DMA_ADAPTER_RX_NCHANS, 0x2)
			| FIELD_PREP(DMA_ADAPTER_TX_LSB_PAD, pad)
			| FIELD_PREP(DMA_ADAPTER_RX_MSB_PAD, pad);

#ifndef USE_RXB_FOR_CAPTURE
	writel_relaxed(regval, mca->switch_base + REG_DMA_ADAPTER_A(route->serdes));
#else
	if (is_tx)
		writel_relaxed(regval, mca->switch_base + REG_DMA_ADAPTER_A(route->serdes));
	else
		writel_relaxed(regval, mca->switch_base + REG_DMA_ADAPTER_B(route->serdes));
#endif

	if (!mca_clocks_in_use(route)) {
		/*
		 * Set up FSYNC duty cycle to be as even as possible.
		 */
		mca_poke(mca, route->syncgen,
			REG_SYNCGEN_HI_PERIOD,
			(bclk_ratio / 2) - 1);
		mca_poke(mca, route->syncgen,
			REG_SYNCGEN_LO_PERIOD,
			((bclk_ratio + 1) / 2) - 1);

		mca_poke(mca, route->clock,
			REG_MCLK_CONF,
			FIELD_PREP(MCLK_CONF_DIV, 0x1));

		ret = clk_set_rate(route->clk_parent, bclk_ratio * samp_rate);
		if (ret) {
			dev_err(mca->dev, "unable to set parent clock %d: %d\n",
				route->clock, ret);
			return ret;
		}
	}

	return 0;
}

static int mca_hw_params(struct snd_soc_component *component,
			struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params)
{
	struct mca_route *route = mca_route_for_rtd(
				asoc_substream_to_rtd(substream));
	int ret;

	ret = mca_hw_params_dma(component, substream, params);
	if (ret < 0)
		return ret;

	return mca_hw_params_dais(route, substream, params);
}

static int mca_close(struct snd_soc_component *component,
		struct snd_pcm_substream *substream)
{
	return snd_dmaengine_pcm_close(substream);
}

#if 0
static void mca_flush_adapter_fifo(struct mca_route *route)
{
	struct mca_data *mca = route->host;
	int i;
	u32 ptr;

	ptr = readl_relaxed(mca->switch_base + REG_DMA_ADAPTER_A(route->serdes) + 0x8);
	dev_dbg(route->host->dev, "flush fifo: entered at %x\n", ptr);

	for (i = 0; i < 256; i++) {
		if (ptr == 0xbc)
			break;

		writel_relaxed(0, mca->switch_base + REG_DMA_ADAPTER_A(route->serdes) + 0xc);
		(void) readl_relaxed(mca->switch_base + REG_DMA_ADAPTER_A(route->serdes) + 0xc);

		ptr = readl_relaxed(mca->switch_base + REG_DMA_ADAPTER_A(route->serdes) + 0x8);
	}

	dev_dbg(route->host->dev, "flush fifo: left at %x\n", ptr);
}
#endif

static int mca_trigger(struct snd_soc_component *component,
		struct snd_pcm_substream *substream, int cmd)
{
	struct mca_route *route = mca_route_for_rtd(asoc_substream_to_rtd(substream));
	int ret;
	
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = mca_reset_dais(route, substream, cmd);
		if (ret < 0)
			return ret;

		//mca_flush_adapter_fifo(route);

		ret = snd_dmaengine_pcm_trigger(substream, cmd);
		if (ret < 0)
			return ret;

		ret = mca_trigger_dais(route, substream, cmd);
		if (ret < 0)
			goto revert_dmaengine;
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		ret = mca_trigger_dais(route, substream, cmd);
		if (ret < 0)
			return ret;

		return snd_dmaengine_pcm_trigger(substream, cmd);
	}

revert_dmaengine:
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		snd_dmaengine_pcm_trigger(substream, SNDRV_PCM_TRIGGER_STOP);
		break;

	case SNDRV_PCM_TRIGGER_RESUME:
		snd_dmaengine_pcm_trigger(substream, SNDRV_PCM_TRIGGER_STOP);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		snd_dmaengine_pcm_trigger(substream, SNDRV_PCM_TRIGGER_PAUSE_PUSH);
		break;
	}

	return ret;
}

static snd_pcm_uframes_t mca_pointer(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	return snd_dmaengine_pcm_pointer(substream);
}

static int mca_pcm_new(struct snd_soc_component *component,
			struct snd_soc_pcm_runtime *rtd)
{
	struct mca_data *mca = snd_soc_component_get_drvdata(component);
	struct mca_route *route;
	struct snd_soc_dai *dai;
	struct mca_dai *mdai;
	struct mca_cluster *cluster;
	unsigned int i;
	int ret = 0;

	route = devm_kzalloc(mca->dev, struct_size(route, dais, rtd->num_cpus), GFP_KERNEL);

	if (!route)
		return -ENOMEM;

	route->host = mca;

	for_each_rtd_cpu_dais(rtd, i, dai) {
		if (dai->component != component) {
			dev_err(mca->dev, "foreign CPU dai in PCM\n");
			goto exit_free;
		}

		mdai = &mca->clusters[dai->id].port;

		if (WARN_ON(mdai->in_route)) {
			ret = -EINVAL;
			goto exit_free;
		}

		mdai->in_route = route;
		route->dais[i] = mdai;
	}
	route->ndais = rtd->num_cpus;

	/*
	 * Pick facilities from cluster of the first dai.
	 */
	cluster = mca_dai_to_cluster(route->dais[0]);

	route->clock = cluster->no;
	route->syncgen = cluster->no;
	route->serdes = cluster->no;

	route->clk_parent = cluster->clk_parent;

	for_each_pcm_streams(i) {
		struct snd_pcm_substream *substream = rtd->pcm->streams[i].substream;
		struct dma_chan *chan = cluster->dma_chans[i];

		if (!substream)
			continue;

		if (!chan) {
			dev_err(component->dev, "missing DMA channel for stream %d "
					"on serdes %d\n", i, route->serdes);
			return -EINVAL;
		}

		snd_pcm_set_managed_buffer(substream, SNDRV_DMA_TYPE_DEV_IRAM,
					chan->device->dev, 512*1024*6,
					SIZE_MAX);
	}

	/* Look at the first dai for daifmt settings */
	mdai = route->dais[0];

	mca_modify(mca, route->serdes, CLUSTER_TX_OFF + REG_TX_SERDES_CONF,
				SERDES_CONF_BCLK_POL,
				mdai->fmt_bclk_inv ? SERDES_CONF_BCLK_POL : 0);
	mca_poke(mca, route->serdes, CLUSTER_TX_OFF + REG_TX_SERDES_BITSTART,
				mdai->fmt_bitstart);
	mca_modify(mca, route->serdes, CLUSTER_RX_OFF + REG_RX_SERDES_CONF,
				SERDES_CONF_BCLK_POL,
				mdai->fmt_bclk_inv ? SERDES_CONF_BCLK_POL : 0);
	mca_poke(mca, route->serdes, CLUSTER_RX_OFF + REG_RX_SERDES_BITSTART,
				mdai->fmt_bitstart);

	return ret;

exit_free:
	devm_kfree(mca->dev, route);
	return ret;
}

static void mca_pcm_free(struct snd_soc_component *component,
			struct snd_pcm *pcm)
{
	struct mca_data *mca = snd_soc_component_get_drvdata(component);
	struct mca_route *route = mca_route_for_rtd(asoc_pcm_to_rtd(pcm));
	int i;

	for (i = 0; i < route->ndais; i++)
		route->dais[i]->in_route = NULL;

	devm_kfree(mca->dev, route);
}

#if 0
static irqreturn_t mca_interrupt(int irq, void *devid)
{
	struct mca_cluster *cl = devid;
	struct mca_data *mca = cl->host;
	u32 mask = mca_peek(mca, cl->no, REG_INTMASK);
	u32 state = mca_peek(mca, cl->no, REG_INTSTATE);
	u32 cleared;

	mca_poke(mca, cl->no, REG_INTSTATE, state & mask);
	cleared = state & ~mca_peek(mca, cl->no, REG_INTSTATE);

	dev_dbg(mca->dev, "cl%d: took an interrupt. state=%x mask=%x unmasked=%x cleared=%x\n",
			cl->no, state, mask, state & mask, cleared);

	mca_poke(mca, cl->no, REG_INTMASK, mask & (~state | cleared));

	return true ? IRQ_HANDLED : IRQ_NONE;
}
#endif

static const struct snd_soc_component_driver mca_component = {
	.name = "apple-mca",
	.open		= mca_pcm_open,
	.close		= mca_close,
	.prepare	= mca_prepare,
	.hw_free	= mca_hw_free,
	.hw_params	= mca_hw_params,
	.trigger	= mca_trigger,
	.pointer	= mca_pointer,
	.pcm_construct	= mca_pcm_new,
	.pcm_destruct   = mca_pcm_free,
};

static void apple_mca_release(struct mca_data *mca)
{
	int i, stream;

	for (i = 0; i < mca->nclusters; i++) {
		struct mca_cluster *cl = &mca->clusters[i];

		for_each_pcm_streams(stream) {
			if (IS_ERR_OR_NULL(cl->dma_chans[stream]))
				continue;

			dma_release_channel(cl->dma_chans[stream]);
		}

		if (!IS_ERR_OR_NULL(cl->clk_parent))
			clk_put(cl->clk_parent);

		if (!IS_ERR_OR_NULL(cl->pd_dev))
			dev_pm_domain_detach(cl->pd_dev, true);
	}

	if (mca->pd_link)
		device_link_del(mca->pd_link);

	if (!IS_ERR_OR_NULL(mca->pd_dev))
		dev_pm_domain_detach(mca->pd_dev, true);
}

static int apple_mca_probe(struct platform_device *pdev)
{
	struct mca_data *mca;
	struct mca_cluster *clusters;
	struct snd_soc_dai_driver *dai_drivers;
	int nclusters;
	int irq, ret, i;

	ret = of_property_read_u32(pdev->dev.of_node, "apple,nclusters", &nclusters);
	if (ret || nclusters > MAX_NCLUSTERS) {
		dev_err(&pdev->dev, "missing or invalid apple,nclusters property\n");
		return -EINVAL;
	}

	mca = devm_kzalloc(&pdev->dev, struct_size(mca, clusters, nclusters),
				GFP_KERNEL);
	if (!mca)
		return -ENOMEM;

	mca->dev = &pdev->dev;
	mca->nclusters = nclusters;
	platform_set_drvdata(pdev, mca);
	clusters = mca->clusters;

	mca->base = devm_platform_ioremap_resource_byname(pdev, "clusters");
	if (IS_ERR(mca->base)) {
		dev_err(&pdev->dev, "unable to obtain clusters MMIO resource: %ld\n",
					PTR_ERR(mca->base));
		return PTR_ERR(mca->base);
	}

	mca->switch_base = devm_platform_ioremap_resource_byname(pdev, "switch");
	if (IS_ERR(mca->switch_base)) {
		dev_err(&pdev->dev, "unable to obtain switch MMIO resource: %ld\n",
					PTR_ERR(mca->switch_base));
		return PTR_ERR(mca->switch_base);
	}

	{
		struct reset_control *rst;
		rst = of_reset_control_array_get(pdev->dev.of_node, true, true, false);
		if (IS_ERR(rst)) {
			dev_err(&pdev->dev, "unable to obtain reset control: %ld\n",
					PTR_ERR(rst));
		} else if (rst) {
			reset_control_reset(rst);
			reset_control_put(rst);
		}
	}

	dai_drivers = devm_kzalloc(&pdev->dev, sizeof(*dai_drivers) * nclusters,
					GFP_KERNEL);
	if (!dai_drivers)
		return -ENOMEM;

	mca->pd_dev = dev_pm_domain_attach_by_id(&pdev->dev, 0);
	if (IS_ERR(mca->pd_dev))
		return -EINVAL;

	mca->pd_link = device_link_add(&pdev->dev, mca->pd_dev,
				DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME |
					DL_FLAG_RPM_ACTIVE);
	if (!mca->pd_link) {
		ret = -EINVAL;
		goto err_release;
	}

	for (i = 0; i < nclusters; i++) {
		struct mca_cluster *cl = &clusters[i];
		struct snd_soc_dai_driver *drv = &dai_drivers[i];
		int stream;

		cl->host = mca;
		cl->no = i;

		cl->clk_parent = of_clk_get(pdev->dev.of_node, i);
		if (IS_ERR(cl->clk_parent)) {
			dev_err(&pdev->dev, "unable to obtain clock %d: %ld\n",
				i, PTR_ERR(cl->clk_parent));
			ret = PTR_ERR(cl->clk_parent);
			goto err_release;
		}

		cl->pd_dev = dev_pm_domain_attach_by_id(&pdev->dev, i + 1);
		if (IS_ERR(cl->pd_dev)) {
			dev_err(&pdev->dev, "unable to obtain cluster %d PD: %ld\n",
				i, PTR_ERR(cl->pd_dev));
			ret = PTR_ERR(cl->pd_dev);
			goto err_release;
		}

#if 0
		ret = clk_rate_exclusive_get(clk);
		if (ret) {
			dev_err(&pdev->dev, "unable to get clock rate exclusivity\n");
			goto err_release;
		}

#endif

		irq = platform_get_irq_optional(pdev, i);
		if (irq >= 0) {
			dev_dbg(&pdev->dev, "have IRQs for cluster %d\n", i);
#if 0
			ret = devm_request_irq(&pdev->dev, irq, mca_interrupt,
						0, dev_name(&pdev->dev), cl);
 			if (ret) {
	 			dev_err(&pdev->dev, "unable to register interrupt: %d\n", ret);
 				goto err_release;
 			}
 			mca_poke(mca, i, REG_INTMASK, 0xffffffff);
#endif
		}

		for_each_pcm_streams(stream) {
			struct dma_chan *chan;
			bool is_tx = (stream == SNDRV_PCM_STREAM_PLAYBACK);
#ifndef USE_RXB_FOR_CAPTURE
			char *name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					is_tx ? "tx%da" : "rx%da", i);
#else
			char *name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					is_tx ? "tx%da" : "rx%db", i);
#endif

			chan = of_dma_request_slave_channel(pdev->dev.of_node, name);
			if (IS_ERR(chan)) {
				if (PTR_ERR(chan) != -EPROBE_DEFER)
					dev_err(&pdev->dev, "no %s DMA channel: %ld\n",
						name, PTR_ERR(chan));

				ret = PTR_ERR(chan);
				goto err_release;
			}

			cl->dma_chans[stream] = chan;
		}

		drv->id = i;
		drv->name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
						"mca-i2s-%d", i);
		if (!drv->name) {
			ret = -ENOMEM;
			goto err_release;
		}
		drv->ops = &mca_dai_ops;
		drv->playback.channels_min = 1;
		drv->playback.channels_max = 32;
		drv->playback.rates = SNDRV_PCM_RATE_8000_192000;
		drv->playback.formats = SNDRV_PCM_FMTBIT_S16_LE |
					SNDRV_PCM_FMTBIT_S24_LE |
					SNDRV_PCM_FMTBIT_S32_LE;
		drv->capture.channels_min = 1;
		drv->capture.channels_max = 32;
		drv->capture.rates = SNDRV_PCM_RATE_8000_192000;
		drv->capture.formats = SNDRV_PCM_FMTBIT_S16_LE |
					SNDRV_PCM_FMTBIT_S24_LE |
					SNDRV_PCM_FMTBIT_S32_LE;
		drv->symmetric_rate = 1;
	}

	ret = devm_snd_soc_register_component(&pdev->dev, &mca_component,
						dai_drivers, nclusters);
	if (ret) {
		dev_err(&pdev->dev, "unable to register ASoC component: %d\n", ret);
		goto err_release;
	}

	dev_dbg(&pdev->dev, "all good, ready to go!\n");
	return 0;

err_release:
	apple_mca_release(mca);
	return ret;
}

static int apple_mca_remove(struct platform_device *pdev)
{
	struct mca_data *mca = platform_get_drvdata(pdev);

	apple_mca_release(mca);
	/* TODO */

	return 0;
}

static const struct of_device_id apple_mca_of_match[] = {
	{ .compatible = "apple,mca", },
	{}
};
MODULE_DEVICE_TABLE(of, apple_mca_of_match);

static struct platform_driver apple_mca_driver = {
	.driver = {
		.name = "apple-mca",
		.owner = THIS_MODULE,
		.of_match_table = apple_mca_of_match,
	},
	.probe = apple_mca_probe,
	.remove = apple_mca_remove,
};
module_platform_driver(apple_mca_driver);

MODULE_AUTHOR("Martin Povišer <povik+lin@cutebit.org>");
MODULE_DESCRIPTION("ASoC platform driver for Apple Silicon SoCs");
MODULE_LICENSE("GPL");
