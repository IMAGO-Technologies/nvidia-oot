// SPDX-License-Identifier: GPL-2.0-only
//
// tegra210_mvc.c - Tegra210 MVC driver
//
// Copyright (c) 2014-2021 NVIDIA CORPORATION.  All rights reserved.

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "tegra210_ahub.h"
#include "tegra210_mvc.h"
#include "tegra_cif.h"

static const struct reg_default tegra210_mvc_reg_defaults[] = {
	{ TEGRA210_MVC_AXBAR_RX_INT_MASK, 0x00000001},
	{ TEGRA210_MVC_AXBAR_RX_CIF_CTRL, 0x00007700},
	{ TEGRA210_MVC_AXBAR_TX_INT_MASK, 0x00000001},
	{ TEGRA210_MVC_AXBAR_TX_CIF_CTRL, 0x00007700},
	{ TEGRA210_MVC_CG, 0x1},
	{ TEGRA210_MVC_CTRL, 0x40000001},
	{ TEGRA210_MVC_INIT_VOL, 0x00800000},
	{ TEGRA210_MVC_TARGET_VOL, 0x00800000},
	{ TEGRA210_MVC_DURATION, 0x000012c0},
	{ TEGRA210_MVC_DURATION_INV, 0x0006d3a0},
	{ TEGRA210_MVC_POLY_N1, 0x0000007d},
	{ TEGRA210_MVC_POLY_N2, 0x00000271},
	{ TEGRA210_MVC_PEAK_CTRL, 0x000012c0},
	{ TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL, 0x00004000},
};

static int tegra210_mvc_runtime_suspend(struct device *dev)
{
	struct tegra210_mvc *mvc = dev_get_drvdata(dev);

	regcache_cache_only(mvc->regmap, true);
	regcache_mark_dirty(mvc->regmap);

	return 0;
}

static int tegra210_mvc_runtime_resume(struct device *dev)
{
	struct tegra210_mvc *mvc = dev_get_drvdata(dev);

	regcache_cache_only(mvc->regmap, false);
	regcache_sync(mvc->regmap);
	regmap_update_bits(mvc->regmap, TEGRA210_MVC_CTRL,
			   TEGRA210_MVC_CURVE_TYPE_MASK,
			   mvc->curve_type << TEGRA210_MVC_CURVE_TYPE_SHIFT);

	return 0;
}

static int tegra210_mvc_write_ram(struct tegra210_mvc *mvc,
				  unsigned int addr, unsigned int coef)
{
	unsigned int reg, val;
	int err;

	err = regmap_read_poll_timeout(mvc->regmap,
				       TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL,
				       val, !(val & 0x80000000), 10, 10000);
	if (err < 0)
		return err;

	reg = (addr << TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL_RAM_ADDR_SHIFT) &
	      TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL_RAM_ADDR_MASK;
	reg |= TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL_ADDR_INIT_EN;
	reg |= TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL_RW_WRITE;
	reg |= TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL_SEQ_ACCESS_EN;

	regmap_write(mvc->regmap, TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL, reg);
	regmap_write(mvc->regmap, TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_DATA,
		     coef);

	return 0;
}

static int tegra210_mvc_get_vol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *cmpnt = snd_soc_kcontrol_component(kcontrol);
	struct tegra210_mvc *mvc = snd_soc_component_get_drvdata(cmpnt);
	unsigned int reg = mc->reg;

	if (reg == TEGRA210_MVC_TARGET_VOL) {
		s32 val = mvc->volume;

		if (mvc->curve_type == CURVE_POLY)
			val = ((val >> 16) * 100) >> 8;
		else {
			val = (val * 100) >> 8;
			val += 12000;
		}
		ucontrol->value.integer.value[0] = val;
	} else {
		u32 val;

		regmap_read(mvc->regmap, reg, &val);
		ucontrol->value.integer.value[0] =
			((val & TEGRA210_MVC_MUTE_MASK) != 0);
	}
	return 0;
}

static int tegra210_mvc_put_vol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *cmpnt = snd_soc_kcontrol_component(kcontrol);
	struct tegra210_mvc *mvc = snd_soc_component_get_drvdata(cmpnt);
	unsigned int reg = mc->reg;
	unsigned int value;
	int err;
	s32 val;

	pm_runtime_get_sync(cmpnt->dev);

	/* check if VOLUME_SWITCH is triggered */
	err = regmap_read_poll_timeout(mvc->regmap, TEGRA210_MVC_SWITCH,
			value, !(value & TEGRA210_MVC_VOLUME_SWITCH_MASK),
			10, 10000);
	if (err < 0)
		goto end;

	if (reg == TEGRA210_MVC_TARGET_VOL) {
		/* Volume control read from mixer ctl is with  */
		/* 100x scaling; for CURVE_POLY the reg range  */
		/* is 0-100 (linear, Q24) and for CURVE_LINEAR */
		/* it is -120dB to +40dB (Q8)                  */
		if (mvc->curve_type == CURVE_POLY) {
			val = ucontrol->value.integer.value[0];
			if (val > 10000)
				val = 10000;
			mvc->volume = ((val * (1<<8)) / 100) << 16;
		} else {
			val = ucontrol->value.integer.value[0] - 12000;
			mvc->volume = (val * (1<<8)) / 100;
		}
		err = regmap_write(mvc->regmap, reg, mvc->volume);
	} else {
		err = regmap_update_bits(mvc->regmap, reg,
				TEGRA210_MVC_MUTE_MASK,
				(ucontrol->value.integer.value[0] ?
				TEGRA210_MVC_MUTE_EN : 0));
	}
	err |= regmap_update_bits(mvc->regmap, TEGRA210_MVC_SWITCH,
			TEGRA210_MVC_VOLUME_SWITCH_MASK,
			TEGRA210_MVC_VOLUME_SWITCH_TRIGGER);

end:
	pm_runtime_put(cmpnt->dev);

	if (reg == TEGRA210_MVC_TARGET_VOL)
		err |= regmap_update_bits(mvc->regmap, TEGRA210_MVC_CTRL,
				TEGRA210_MVC_MUTE_MASK, 0);

	return err;
}

static int tegra210_mvc_get_curve_type(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmpnt = snd_soc_kcontrol_component(kcontrol);
	struct tegra210_mvc *mvc = snd_soc_component_get_drvdata(cmpnt);

	ucontrol->value.integer.value[0] = mvc->curve_type;

	return 0;
}

static int tegra210_mvc_put_curve_type(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmpnt = snd_soc_kcontrol_component(kcontrol);
	struct tegra210_mvc *mvc = snd_soc_component_get_drvdata(cmpnt);

	mvc->curve_type = ucontrol->value.integer.value[0];
	/* change volume to default init for new curve type */
	if (mvc->curve_type == CURVE_POLY)
		mvc->volume = TEGRA210_MVC_INIT_VOL_DEFAULT_POLY;
	else
		mvc->volume = TEGRA210_MVC_INIT_VOL_DEFAULT_LINEAR;

	return 0;
}

static int tegra210_mvc_get_audio_bits(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmpnt = snd_soc_kcontrol_component(kcontrol);
	struct tegra210_mvc *mvc = snd_soc_component_get_drvdata(cmpnt);

	if (mvc->audio_bits > 0)
		ucontrol->value.integer.value[0] = (mvc->audio_bits + 1) * 4;
	else
		ucontrol->value.integer.value[0] = 0;

	return 0;
}

static int tegra210_mvc_put_audio_bits(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmpnt = snd_soc_kcontrol_component(kcontrol);
	struct tegra210_mvc *mvc = snd_soc_component_get_drvdata(cmpnt);
	unsigned int val;

	val = ucontrol->value.integer.value[0];
	if ((val >= 8) && (val <= 32) && (val%4 == 0))
		mvc->audio_bits = val/4 - 1;
	else if (val == 0)
		mvc->audio_bits = 0;
	else
		return -EINVAL;

	return 0;
}
static int tegra210_mvc_get_format(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmpnt = snd_soc_kcontrol_component(kcontrol);
	struct tegra210_mvc *mvc = snd_soc_component_get_drvdata(cmpnt);

	/* get the format control flag */
	if (strstr(kcontrol->id.name, "input bit format"))
		ucontrol->value.integer.value[0] = mvc->format_in;
	else if (strstr(kcontrol->id.name, "Channels"))
		ucontrol->value.integer.value[0] = mvc->cif_channels;

	return 0;
}

static int tegra210_mvc_put_format(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *cmpnt = snd_soc_kcontrol_component(kcontrol);
	struct tegra210_mvc *mvc = snd_soc_component_get_drvdata(cmpnt);
	unsigned int value = ucontrol->value.integer.value[0];

	/* set the format control flag */
	if (strstr(kcontrol->id.name, "input bit format"))
		mvc->format_in = value;
	else if (strstr(kcontrol->id.name, "Channels"))
		mvc->cif_channels = value;

	return 0;
}

static const int tegra210_mvc_fmt_values[] = {
	0,
	TEGRA_ACIF_BITS_16,
	TEGRA_ACIF_BITS_32,
};

static int tegra210_mvc_set_audio_cif(struct tegra210_mvc *mvc,
				struct snd_pcm_hw_params *params,
				unsigned int reg)
{
	int channels, audio_bits;
	struct tegra_cif_conf cif_conf;

	memset(&cif_conf, 0, sizeof(struct tegra_cif_conf));

	channels = params_channels(params);
	if (mvc->cif_channels > 0)
		channels = mvc->cif_channels;

	if (channels > 8)
		return -EINVAL;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		audio_bits = TEGRA_ACIF_BITS_16;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		audio_bits = TEGRA_ACIF_BITS_32;
		break;
	default:
		return -EINVAL;
	}

	if (mvc->audio_bits > 0)
		audio_bits = mvc->audio_bits;

	cif_conf.audio_ch = channels;
	cif_conf.client_ch = channels;
	cif_conf.audio_bits = audio_bits;
	cif_conf.client_bits = audio_bits;

	/* Override input format to MVC, if set */
	if (mvc->format_in && (reg == TEGRA210_MVC_AXBAR_RX_CIF_CTRL))
		cif_conf.audio_bits = tegra210_mvc_fmt_values[mvc->format_in];

	tegra_set_cif(mvc->regmap, reg, &cif_conf);

	return 0;
}

static int tegra210_mvc_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct device *dev = dai->dev;
	struct tegra210_mvc *mvc = snd_soc_dai_get_drvdata(dai);
	int i, err, val;

	/* SW reset */
	regmap_write(mvc->regmap, TEGRA210_MVC_SOFT_RESET, 1);

	err = regmap_read_poll_timeout(mvc->regmap, TEGRA210_MVC_SOFT_RESET,
				       val, !val, 10, 10000);
	if (err < 0) {
		dev_err(dev, "SW reset failed, err = %d\n", err);
		return err;
	}

	/* set RX cif and TX cif */
	err = tegra210_mvc_set_audio_cif(mvc, params,
				TEGRA210_MVC_AXBAR_RX_CIF_CTRL);
	if (err) {
		dev_err(dev, "Can't set MVC RX CIF: %d\n", err);
		return err;
	}
	err = tegra210_mvc_set_audio_cif(mvc, params,
				TEGRA210_MVC_AXBAR_TX_CIF_CTRL);
	if (err) {
		dev_err(dev, "Can't set MVC TX CIF: %d\n", err);
		return err;
	}

	/* disable per_channel_cntrl_en */
	regmap_update_bits(mvc->regmap, TEGRA210_MVC_CTRL,
		TEGRA210_MVC_PER_CHAN_CTRL_EN_MASK,
		~(TEGRA210_MVC_PER_CHAN_CTRL_EN_MASK));

	/* change curve type */
	err = regmap_update_bits(mvc->regmap, TEGRA210_MVC_CTRL,
			TEGRA210_MVC_CURVE_TYPE_MASK,
			mvc->curve_type <<
			TEGRA210_MVC_CURVE_TYPE_SHIFT);

	/* init the default volume=1 for MVC */
	regmap_write(mvc->regmap, TEGRA210_MVC_INIT_VOL, mvc->volume);

	regmap_write(mvc->regmap, TEGRA210_MVC_TARGET_VOL, mvc->volume);

	/* program the poly coefficients */
	for (i = 0; i < 9; i++) {
		err = tegra210_mvc_write_ram(mvc, i, mvc->poly_coeff[i]);
		if (err < 0) {
			dev_err(dai->dev, "failed to write coefs, err = %d\n",
				err);
			return err;
		}
	}

	/* program poly_n1, poly_n2, duration */
	regmap_write(mvc->regmap, TEGRA210_MVC_POLY_N1, mvc->poly_n1);
	regmap_write(mvc->regmap, TEGRA210_MVC_POLY_N2, mvc->poly_n2);
	regmap_write(mvc->regmap, TEGRA210_MVC_DURATION, mvc->duration);

	/* program duration_inv */
	regmap_write(mvc->regmap, TEGRA210_MVC_DURATION_INV, mvc->duration_inv);

	/* trigger volume switch */
	err |= regmap_update_bits(mvc->regmap, TEGRA210_MVC_SWITCH,
			TEGRA210_MVC_VOLUME_SWITCH_MASK,
			TEGRA210_MVC_VOLUME_SWITCH_TRIGGER);
	return err;
}

static struct snd_soc_dai_ops tegra210_mvc_dai_ops = {
	.hw_params	= tegra210_mvc_hw_params,
};

static const unsigned int tegra210_mvc_curve_type_values[] = {
	CURVE_POLY,
	CURVE_LINEAR,
};

static const char * const tegra210_mvc_curve_type_text[] = {
	"Poly",
	"Linear",
};

static const struct soc_enum tegra210_mvc_curve_type_ctrl =
	SOC_ENUM_SINGLE_EXT(2, tegra210_mvc_curve_type_text);

static const char * const tegra210_mvc_format_text[] = {
	"None",
	"16",
	"32",
};

static const struct soc_enum tegra210_mvc_format_enum =
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0,
		ARRAY_SIZE(tegra210_mvc_format_text),
		tegra210_mvc_format_text);

static const struct snd_kcontrol_new tegra210_mvc_vol_ctrl[] = {
	SOC_SINGLE_EXT("Vol", TEGRA210_MVC_TARGET_VOL, 0, 16000, 0,
		tegra210_mvc_get_vol, tegra210_mvc_put_vol),
	SOC_SINGLE_EXT("Mute", TEGRA210_MVC_CTRL, 0, 1, 0,
		tegra210_mvc_get_vol, tegra210_mvc_put_vol),
	SOC_ENUM_EXT("Curve Type", tegra210_mvc_curve_type_ctrl,
		tegra210_mvc_get_curve_type, tegra210_mvc_put_curve_type),
	SOC_SINGLE_EXT("Bits", 0, 0, 32, 0,
		tegra210_mvc_get_audio_bits, tegra210_mvc_put_audio_bits),
	SOC_SINGLE_EXT("Channels", 0, 0, 8, 0,
		tegra210_mvc_get_format, tegra210_mvc_put_format),
	SOC_ENUM_EXT("input bit format", tegra210_mvc_format_enum,
		tegra210_mvc_get_format, tegra210_mvc_put_format),
};

static struct snd_soc_dai_driver tegra210_mvc_dais[] = {
	{
		.name = "MVC IN",
		.playback = {
			.stream_name = "MVC Receive",
			.channels_min = 1,
			.channels_max = 8,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
	},
	{
		.name = "MVC OUT",
		.capture = {
			.stream_name = "MVC Transmit",
			.channels_min = 1,
			.channels_max = 8,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S8 |
				SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S32_LE,
		},
		.ops = &tegra210_mvc_dai_ops,
	}
};

static const struct snd_soc_dapm_widget tegra210_mvc_widgets[] = {
	SND_SOC_DAPM_AIF_IN("MVC RX", NULL, 0, SND_SOC_NOPM,
				0, 0),
	SND_SOC_DAPM_AIF_OUT("MVC TX", NULL, 0, TEGRA210_MVC_ENABLE,
				TEGRA210_MVC_EN_SHIFT, 0),
};

static const struct snd_soc_dapm_route tegra210_mvc_routes[] = {
	{ "MVC RX",       NULL, "MVC Receive" },
	{ "MVC TX",       NULL, "MVC RX" },
	{ "MVC Transmit", NULL, "MVC TX" },
};

static struct snd_soc_component_driver tegra210_mvc_cmpnt = {
	.dapm_widgets = tegra210_mvc_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tegra210_mvc_widgets),
	.dapm_routes = tegra210_mvc_routes,
	.num_dapm_routes = ARRAY_SIZE(tegra210_mvc_routes),
	.controls = tegra210_mvc_vol_ctrl,
	.num_controls = ARRAY_SIZE(tegra210_mvc_vol_ctrl),
};

static bool tegra210_mvc_wr_rd_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA210_MVC_AXBAR_RX_STATUS:
	case TEGRA210_MVC_AXBAR_RX_INT_STATUS:
	case TEGRA210_MVC_AXBAR_RX_INT_MASK:
	case TEGRA210_MVC_AXBAR_RX_INT_SET:
	case TEGRA210_MVC_AXBAR_RX_INT_CLEAR:
	case TEGRA210_MVC_AXBAR_RX_CIF_CTRL:
	case TEGRA210_MVC_AXBAR_RX_CYA:
	case TEGRA210_MVC_AXBAR_RX_DBG:
	case TEGRA210_MVC_AXBAR_TX_STATUS:
	case TEGRA210_MVC_AXBAR_TX_INT_STATUS:
	case TEGRA210_MVC_AXBAR_TX_INT_MASK:
	case TEGRA210_MVC_AXBAR_TX_INT_SET:
	case TEGRA210_MVC_AXBAR_TX_INT_CLEAR:
	case TEGRA210_MVC_AXBAR_TX_CIF_CTRL:
	case TEGRA210_MVC_AXBAR_TX_CYA:
	case TEGRA210_MVC_AXBAR_TX_DBG:
	case TEGRA210_MVC_ENABLE:
	case TEGRA210_MVC_SOFT_RESET:
	case TEGRA210_MVC_CG:
	case TEGRA210_MVC_STATUS:
	case TEGRA210_MVC_INT_STATUS:
	case TEGRA210_MVC_CTRL:
	case TEGRA210_MVC_SWITCH:
	case TEGRA210_MVC_INIT_VOL:
	case TEGRA210_MVC_TARGET_VOL:
	case TEGRA210_MVC_DURATION:
	case TEGRA210_MVC_DURATION_INV:
	case TEGRA210_MVC_POLY_N1:
	case TEGRA210_MVC_POLY_N2:
	case TEGRA210_MVC_PEAK_CTRL:
	case TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL:
	case TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_DATA:
	case TEGRA210_MVC_PEAK_VALUE:
	case TEGRA210_MVC_CONFIG_ERR_TYPE:
	case TEGRA210_MVC_CYA:
	case TEGRA210_MVC_DBG:
		return true;
	default:
		return false;
	}
}

static bool tegra210_mvc_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TEGRA210_MVC_AXBAR_RX_STATUS:
	case TEGRA210_MVC_AXBAR_RX_INT_STATUS:
	case TEGRA210_MVC_AXBAR_RX_INT_SET:

	case TEGRA210_MVC_AXBAR_TX_STATUS:
	case TEGRA210_MVC_AXBAR_TX_INT_STATUS:
	case TEGRA210_MVC_AXBAR_TX_INT_SET:

	case TEGRA210_MVC_SOFT_RESET:
	case TEGRA210_MVC_STATUS:
	case TEGRA210_MVC_INT_STATUS:
	case TEGRA210_MVC_SWITCH:
	case TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_CTRL:
	case TEGRA210_MVC_AHUBRAMCTL_CONFIG_RAM_DATA:
	case TEGRA210_MVC_PEAK_VALUE:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config tegra210_mvc_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = TEGRA210_MVC_CYA,
	.writeable_reg = tegra210_mvc_wr_rd_reg,
	.readable_reg = tegra210_mvc_wr_rd_reg,
	.volatile_reg = tegra210_mvc_volatile_reg,
	.reg_defaults = tegra210_mvc_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(tegra210_mvc_reg_defaults),
	.cache_type = REGCACHE_FLAT,
};

static const struct of_device_id tegra210_mvc_of_match[] = {
	{ .compatible = "nvidia,tegra210-mvc" },
	{},
};
MODULE_DEVICE_TABLE(of, tegra210_mvc_of_match);

static int tegra210_mvc_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tegra210_mvc *mvc;
	void __iomem *regs;
	int err;

	mvc = devm_kzalloc(dev, sizeof(*mvc), GFP_KERNEL);
	if (!mvc)
		return -ENOMEM;

	dev_set_drvdata(dev, mvc);

	mvc->poly_n1 = 16;
	mvc->poly_n2 = 63;
	mvc->duration = 150;
	mvc->duration_inv = 14316558;
	mvc->poly_coeff[0] = 23738319;
	mvc->poly_coeff[1] = 659403;
	mvc->poly_coeff[2] = -3680;
	mvc->poly_coeff[3] = 15546680;
	mvc->poly_coeff[4] = 2530732;
	mvc->poly_coeff[5] = -120985;
	mvc->poly_coeff[6] = 12048422;
	mvc->poly_coeff[7] = 5527252;
	mvc->poly_coeff[8] = -785042;
	mvc->curve_type = CURVE_LINEAR;
	mvc->volume = TEGRA210_MVC_INIT_VOL_DEFAULT_LINEAR;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	mvc->regmap = devm_regmap_init_mmio(dev, regs,
					    &tegra210_mvc_regmap_config);
	if (IS_ERR(mvc->regmap)) {
		dev_err(dev, "regmap init failed\n");
		return PTR_ERR(mvc->regmap);
	}

	regcache_cache_only(mvc->regmap, true);

	err = devm_snd_soc_register_component(dev, &tegra210_mvc_cmpnt,
					      tegra210_mvc_dais,
					      ARRAY_SIZE(tegra210_mvc_dais));
	if (err) {
		dev_err(dev, "can't register MVC component, err: %d\n", err);
		return err;
	}

	pm_runtime_enable(dev);

	return 0;
}

static int tegra210_mvc_platform_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct dev_pm_ops tegra210_mvc_pm_ops = {
	SET_RUNTIME_PM_OPS(tegra210_mvc_runtime_suspend,
			   tegra210_mvc_runtime_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				     pm_runtime_force_resume)
};

static struct platform_driver tegra210_mvc_driver = {
	.driver = {
		.name = "tegra210-mvc",
		.of_match_table = tegra210_mvc_of_match,
		.pm = &tegra210_mvc_pm_ops,
	},
	.probe = tegra210_mvc_platform_probe,
	.remove = tegra210_mvc_platform_remove,
};
module_platform_driver(tegra210_mvc_driver)

MODULE_AUTHOR("Arun Shamanna Lakshmi <aruns@nvidia.com>");
MODULE_DESCRIPTION("Tegra210 MVC ASoC driver");
MODULE_LICENSE("GPL");
