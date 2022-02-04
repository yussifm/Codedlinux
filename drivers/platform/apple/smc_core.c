// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC core framework
 * Copyright The Asahi Linux Contributors
 */

#include <linux/device.h>
#include <linux/mfd/core.h>
#include <linux/mutex.h>
#include "smc.h"

struct apple_smc {
	struct device *dev;

	void *be_cookie;
	const struct apple_smc_backend_ops *be;

	struct mutex mutex;

	u32 key_count;
	smc_key first_key;
	smc_key last_key;
};

static const struct mfd_cell apple_smc_devs[] = {
	{
		.name = "macsmc-gpio",
	},
	{
		.name = "macsmc-hid",
	},
};

int apple_smc_read(struct apple_smc *smc, smc_key key, void *buf, size_t size)
{
	int ret;

	mutex_lock(&smc->mutex);
	ret = smc->be->read_key(smc->be_cookie, key, buf, size);
	mutex_unlock(&smc->mutex);

	return ret;
}
EXPORT_SYMBOL(apple_smc_read);

int apple_smc_write(struct apple_smc *smc, smc_key key, void *buf, size_t size)
{
	int ret;

	mutex_lock(&smc->mutex);
	ret = smc->be->write_key(smc->be_cookie, key, buf, size);
	mutex_unlock(&smc->mutex);

	return ret;
}
EXPORT_SYMBOL(apple_smc_write);

int apple_smc_rw(struct apple_smc *smc, smc_key key, void *wbuf, size_t wsize,
		 void *rbuf, size_t rsize)
{
	int ret;

	mutex_lock(&smc->mutex);
	ret = smc->be->rw_key(smc->be_cookie, key, wbuf, wsize, rbuf, rsize);
	mutex_unlock(&smc->mutex);

	return ret;
}
EXPORT_SYMBOL(apple_smc_rw);

int apple_smc_get_key_by_index(struct apple_smc *smc, int index, smc_key *key)
{
	int ret;

	mutex_lock(&smc->mutex);
	ret = smc->be->get_key_by_index(smc->be_cookie, index, key);
	mutex_unlock(&smc->mutex);

	return ret;
}
EXPORT_SYMBOL(apple_smc_get_key_by_index);

int apple_smc_get_key_info(struct apple_smc *smc, smc_key key, struct apple_smc_key_info *info)
{
	int ret;

	mutex_lock(&smc->mutex);
	ret = smc->be->get_key_info(smc->be_cookie, key, info);
	mutex_unlock(&smc->mutex);

	return ret;
}
EXPORT_SYMBOL(apple_smc_get_key_info);

int apple_smc_find_first_key_index(struct apple_smc *smc, smc_key key)
{
	int start = 0, count = smc->key_count;
	int ret;

	if (key <= smc->first_key)
		return 0;
	if (key > smc->last_key)
		return smc->key_count;

	while (count > 1) {
		int pivot = start + ((count - 1) >> 1);
		smc_key pkey;

		ret = apple_smc_get_key_by_index(smc, pivot, &pkey);
		if (ret < 0)
			return ret;

		if (pkey == key) 
			return pivot;

		pivot++;

		if (pkey < key) {
			count -= pivot - start;
			start = pivot;
		} else {
			count = pivot - start;
		}
	}

	return start;
}

int apple_smc_get_key_count(struct apple_smc *smc)
{
	return smc->key_count;
}
EXPORT_SYMBOL(apple_smc_get_key_count);

struct apple_smc *apple_smc_probe(struct device *dev, const struct apple_smc_backend_ops *ops, void *cookie)
{
	struct apple_smc *smc;
	u32 count;
	int ret;

	smc = devm_kzalloc(dev, sizeof(*smc), GFP_KERNEL);
	if (!smc)
		return ERR_PTR(-ENOMEM);

	smc->dev = dev;
	smc->be_cookie = cookie;
	smc->be = ops;
	mutex_init(&smc->mutex);

	ret = apple_smc_read_u32(smc, SMC_KEY(#KEY), &count);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret, "Failed to get key count"));
	smc->key_count = be32_to_cpu(count);

	ret = apple_smc_get_key_by_index(smc, 0, &smc->first_key);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret, "Failed to get first key"));

	ret = apple_smc_get_key_by_index(smc, smc->key_count - 1, &smc->last_key);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret, "Failed to get last key"));

	dev_info(dev, "Initialized (%d keys %p4ch..%p4ch)\n",
		 smc->key_count, &smc->first_key, &smc->last_key);

	dev_set_drvdata(dev, smc);

	ret = mfd_add_devices(dev, -1, apple_smc_devs, ARRAY_SIZE(apple_smc_devs), NULL, 0, NULL);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret, "Subdevice initialization failed"));

	return smc;
}
EXPORT_SYMBOL(apple_smc_probe);

int apple_smc_remove(struct apple_smc *smc)
{
	mfd_remove_devices(smc->dev);

	return 0;
}
EXPORT_SYMBOL(apple_smc_remove);

MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC core");
