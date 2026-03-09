// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung EXYNOS SoC MIPI DSIM PHY isolation driver for Zuma-class DPHY.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#define EXYNOS_MIPI_PHYS_MASTER_NUM	4
#define EXYNOS_MIPI_PHY_M4M4_ISO_BYPASS	BIT(0)

#define MIPI_PHY_MXMX_SHARED		BIT(1)
#define MIPI_PHY_MXMX_INIT_DONE		BIT(2)

enum exynos_mipi_phy_owner {
	EXYNOS_MIPI_PHY_OWNER_DSIM_0 = 0,
	EXYNOS_MIPI_PHY_OWNER_DSIM_1 = 1,
};

struct exynos_mipi_phy_data {
	u8 flags;
	int active_count;
	spinlock_t slock;
};

struct exynos_mipi_phy {
	struct device *dev;
	spinlock_t slock;
	struct regmap *reg_pmu;
	struct regmap *reg_reset;
	enum exynos_mipi_phy_owner owner;
	struct mipi_phy_desc {
		struct phy *phy;
		struct exynos_mipi_phy_data *data;
		unsigned int index;
		unsigned int iso_offset;
		unsigned int rst_bit;
	} phys[EXYNOS_MIPI_PHYS_MASTER_NUM];
};

static int exynos_mipi_phy_set_isolation(struct exynos_mipi_phy *state,
					 unsigned int offset, bool on)
{
	return regmap_update_bits(state->reg_pmu, offset,
				  EXYNOS_MIPI_PHY_M4M4_ISO_BYPASS,
				  on ? EXYNOS_MIPI_PHY_M4M4_ISO_BYPASS : 0);
}

static int exynos_mipi_phy_set_reset(struct exynos_mipi_phy *state,
				     unsigned int bit, bool on)
{
	int ret;

	if (!state->reg_reset)
		return 0;

	ret = regmap_update_bits(state->reg_reset, 0, BIT(bit), 0);
	if (ret || !on)
		return ret;

	return regmap_update_bits(state->reg_reset, 0, BIT(bit), BIT(bit));
}

static int exynos_mipi_phy_init_state(struct exynos_mipi_phy *state,
				      struct mipi_phy_desc *phy_desc)
{
	unsigned int cfg;
	int ret;

	ret = regmap_read(state->reg_pmu, phy_desc->iso_offset, &cfg);
	if (ret)
		return ret;

	if (cfg & EXYNOS_MIPI_PHY_M4M4_ISO_BYPASS)
		phy_desc->data->flags |= MIPI_PHY_MXMX_INIT_DONE;

	return 0;
}

static int exynos_mipi_phy_set_alone(struct exynos_mipi_phy *state,
				     struct mipi_phy_desc *phy_desc, bool on)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&state->slock, flags);

	if (on) {
		ret = exynos_mipi_phy_set_isolation(state, phy_desc->iso_offset, true);
		if (!ret)
			ret = exynos_mipi_phy_set_reset(state, phy_desc->rst_bit, true);
	} else {
		ret = exynos_mipi_phy_set_reset(state, phy_desc->rst_bit, false);
		if (!ret)
			ret = exynos_mipi_phy_set_isolation(state, phy_desc->iso_offset, false);
	}

	spin_unlock_irqrestore(&state->slock, flags);
	return ret;
}

static int exynos_mipi_phy_set_shared(struct exynos_mipi_phy *state,
				      struct mipi_phy_desc *phy_desc, bool on)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&phy_desc->data->slock, flags);

	if (on)
		phy_desc->data->active_count++;
	else
		phy_desc->data->active_count--;

	if (state->owner == EXYNOS_MIPI_PHY_OWNER_DSIM_0 &&
	    (phy_desc->data->flags & MIPI_PHY_MXMX_INIT_DONE)) {
		phy_desc->data->flags &= ~MIPI_PHY_MXMX_INIT_DONE;
		goto out;
	}

	if (on) {
		if (phy_desc->data->active_count == 1) {
			ret = exynos_mipi_phy_set_isolation(state, phy_desc->iso_offset, true);
			if (ret)
				goto out;
		}

		ret = exynos_mipi_phy_set_reset(state, phy_desc->rst_bit, true);
	} else {
		ret = exynos_mipi_phy_set_reset(state, phy_desc->rst_bit, false);
		if (ret)
			goto out;

		if (phy_desc->data->active_count == 0)
			ret = exynos_mipi_phy_set_isolation(state, phy_desc->iso_offset, false);
	}

out:
	spin_unlock_irqrestore(&phy_desc->data->slock, flags);
	return ret;
}

static int exynos_mipi_phy_set_state(struct exynos_mipi_phy *state,
				     struct mipi_phy_desc *phy_desc, bool on)
{
	if (phy_desc->data->flags & MIPI_PHY_MXMX_SHARED)
		return exynos_mipi_phy_set_shared(state, phy_desc, on);

	return exynos_mipi_phy_set_alone(state, phy_desc, on);
}

static struct exynos_mipi_phy_data mipi_phy_m4m4 = {
	.flags = MIPI_PHY_MXMX_SHARED,
	.active_count = 0,
	.slock = __SPIN_LOCK_UNLOCKED(mipi_phy_m4m4.slock),
};

static const struct of_device_id exynos_mipi_phy_of_table[] = {
	{ .compatible = "samsung,mipi-phy-m4m4", .data = &mipi_phy_m4m4 },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_mipi_phy_of_table);

#define to_exynos_mipi_phy(desc) \
	container_of((desc), struct exynos_mipi_phy, phys[(desc)->index])

static int exynos_mipi_dsim_phy_init(struct phy *phy)
{
	struct mipi_phy_desc *phy_desc = phy_get_drvdata(phy);
	struct exynos_mipi_phy *state = to_exynos_mipi_phy(phy_desc);

	return exynos_mipi_phy_init_state(state, phy_desc);
}

static int exynos_mipi_dsim_phy_power_on(struct phy *phy)
{
	struct mipi_phy_desc *phy_desc = phy_get_drvdata(phy);
	struct exynos_mipi_phy *state = to_exynos_mipi_phy(phy_desc);

	return exynos_mipi_phy_set_state(state, phy_desc, true);
}

static int exynos_mipi_dsim_phy_power_off(struct phy *phy)
{
	struct mipi_phy_desc *phy_desc = phy_get_drvdata(phy);
	struct exynos_mipi_phy *state = to_exynos_mipi_phy(phy_desc);

	return exynos_mipi_phy_set_state(state, phy_desc, false);
}

static struct phy *exynos_mipi_phy_of_xlate(struct device *dev,
					    const struct of_phandle_args *args)
{
	struct exynos_mipi_phy *state = dev_get_drvdata(dev);

	if (WARN_ON(args->args[0] >= EXYNOS_MIPI_PHYS_MASTER_NUM))
		return ERR_PTR(-ENODEV);

	return state->phys[args->args[0]].phy;
}

static const struct phy_ops exynos_mipi_phy_ops = {
	.init = exynos_mipi_dsim_phy_init,
	.power_on = exynos_mipi_dsim_phy_power_on,
	.power_off = exynos_mipi_dsim_phy_power_off,
	.owner = THIS_MODULE,
};

static int exynos_mipi_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	struct exynos_mipi_phy *state;
	struct phy_provider *provider;
	struct exynos_mipi_phy_data *phy_data;
	unsigned int iso[EXYNOS_MIPI_PHYS_MASTER_NUM] = {};
	unsigned int rst[EXYNOS_MIPI_PHYS_MASTER_NUM] = {};
	int i, count, ret;

	match = of_match_node(exynos_mipi_phy_of_table, dev->of_node);
	if (!match)
		return -EINVAL;

	state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->dev = dev;
	state->reg_pmu = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(state->reg_pmu))
		return PTR_ERR(state->reg_pmu);

	state->reg_reset = syscon_regmap_lookup_by_phandle(dev->of_node,
							   "samsung,reset-sysreg");
	if (IS_ERR(state->reg_reset))
		state->reg_reset = NULL;

	of_property_read_u32(dev->of_node, "owner", (u32 *)&state->owner);

	count = of_property_count_u32_elems(dev->of_node, "isolation");
	if (count <= 0 || count > EXYNOS_MIPI_PHYS_MASTER_NUM)
		return -EINVAL;

	ret = of_property_read_u32_array(dev->of_node, "isolation", iso, count);
	if (ret)
		return ret;

	if (state->reg_reset) {
		ret = of_property_read_u32_array(dev->of_node, "reset", rst, count);
		if (ret)
			return ret;
	}

	spin_lock_init(&state->slock);
	dev_set_drvdata(dev, state);
	phy_data = (struct exynos_mipi_phy_data *)match->data;

	for (i = 0; i < count; i++) {
		struct phy *phy = devm_phy_create(dev, NULL, &exynos_mipi_phy_ops);

		if (IS_ERR(phy))
			return PTR_ERR(phy);

		state->phys[i].phy = phy;
		state->phys[i].data = phy_data;
		state->phys[i].index = i;
		state->phys[i].iso_offset = iso[i];
		state->phys[i].rst_bit = rst[i];
		phy_set_drvdata(phy, &state->phys[i]);
	}

	provider = devm_of_phy_provider_register(dev, exynos_mipi_phy_of_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static struct platform_driver exynos_mipi_dsim_phy_driver = {
	.probe = exynos_mipi_phy_probe,
	.driver = {
		.name = "exynos-mipi-dsim-phy",
		.of_match_table = exynos_mipi_phy_of_table,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(exynos_mipi_dsim_phy_driver);

MODULE_DESCRIPTION("Samsung EXYNOS Zuma MIPI DSIM PHY isolation driver");
MODULE_LICENSE("GPL");
