/*
 * imx-rt5625.c
 *
 * Copyright (C) 2012 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/fsl_devices.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/switch.h>
#include <linux/gpio.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/jack.h>
#include <mach/dma.h>
#include <mach/clock.h>
#include <mach/audmux.h>

#include "imx-ssi.h"
#include "../codecs/rt5625.h"

struct imx_priv {
	int sysclk;
	
	int hp_irq;
	int hp_status;
	
	struct switch_dev sdev;
	
	struct clk *codec_mclk;
	struct platform_device *pdev;
};

static struct snd_soc_card snd_soc_card_imx;
static struct snd_soc_codec *gcodec;
static struct imx_priv card_priv;
static struct snd_soc_jack imx_hp_jack;
static struct snd_soc_jack_gpio imx_hp_jack_gpio = {
	.name = "headphone detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
	.invert = 0,
};

extern int rt5625_headset_detect(struct snd_soc_codec *codec);
static int hp_jack_status_check(void);

static int imx_hifi_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;


	if (!codec_dai->active)
		clk_enable(card_priv.codec_mclk);

	return 0;
}

static void imx_hifi_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;


	if (!codec_dai->active)
		clk_disable(card_priv.codec_mclk);

	return;
}

static int imx_hifi_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	unsigned int channels = params_channels(params);
	int ret = 0;
	unsigned int pll_out;
	u32 dai_format;


	dai_format = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS;
	
	pll_out = params_rate(params) * 512;

	/* set cpu DAI configuration */

	/* set i.MX active slot mask */
	snd_soc_dai_set_tdm_slot(cpu_dai,
				 channels == 1 ? 0xfffffffe : 0xfffffffc,
				 channels == 1 ? 0xfffffffe : 0xfffffffc,
				 2, 32);

	ret = snd_soc_dai_set_fmt(cpu_dai, dai_format);
	if (ret < 0) return ret;


	/* set the SSI system clock as input (unused) */
	snd_soc_dai_set_sysclk(cpu_dai, IMX_SSP_SYS_CLK, 0, SND_SOC_CLOCK_IN);	

	snd_soc_dai_set_clkdiv(cpu_dai, IMX_SSI_TX_DIV_2, 0);
	snd_soc_dai_set_clkdiv(cpu_dai, IMX_SSI_TX_DIV_PSR, 0);
	snd_soc_dai_set_clkdiv(cpu_dai, IMX_SSI_TX_DIV_PM, 3);


	/* set codec DAI configuration */
	dai_format = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS;
	ret = snd_soc_dai_set_fmt(codec_dai, dai_format);
	if (ret < 0)
		return ret;
	
	ret =  snd_soc_dai_set_pll(codec_dai, RT5625_PLL1_FROM_MCLK, 0, card_priv.sysclk, pll_out);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, 0, pll_out, 0);

	return 0;
}

/* imx card dapm widgets */
static const struct snd_soc_dapm_widget imx_dapm_widgets[] = {
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Ext Speaker", NULL),
};

/* imx machine connections to the codec pins */
static const struct snd_soc_dapm_route audio_map[] = {
	/* headphone connected to HPL, HPR */
	{"Headphone Jack", NULL, "HPL Out PGA"},
	{"Headphone Jack", NULL, "HPR Out PGA"},

	/* ext speaker connected to SPKL, SPKR */
	{"Ext Speaker", NULL, "SPKL Out PGA"},
	{"Ext Speaker", NULL, "SPKR Out PGA"},

	/* ----input ------------------- */
	/* Mic Jack --> MIC_IN (with automatic bias) */
	{"Mic2 Boost", NULL, "Headset Mic"},
	{"Mic1 Boost", NULL, "Main Mic"},

};

static ssize_t show_headphone(struct device_driver *dev, char *buf)
{
	struct imx_priv *priv = &card_priv;
	struct platform_device *pdev = priv->pdev;
	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;

	if (plat->hp_gpio == -1)
		return 0;

	/* determine whether hp is plugged in */
	priv->hp_status = gpio_get_value(plat->hp_gpio);

	if (priv->hp_status != plat->hp_active_low)
		strcpy(buf, "headphone\n");
	else
		strcpy(buf, "speaker\n");

	hp_jack_status_check();

	return strlen(buf);
}

static DRIVER_ATTR(headphone, S_IRUGO | S_IWUSR, show_headphone, NULL);

static void imx_resume_event(struct work_struct *work)
{
	struct imx_priv *priv = &card_priv;
	struct platform_device *pdev = priv->pdev;
	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;
	struct snd_soc_jack *jack;
	int enable;
	int report;

	if (gpio_is_valid(plat->hp_gpio)) {
		jack = imx_hp_jack_gpio.jack;

		enable = gpio_get_value_cansleep(imx_hp_jack_gpio.gpio);
		if (imx_hp_jack_gpio.invert)
			enable = !enable;

		if (enable)
			report = imx_hp_jack_gpio.report;
		else
			report = 0;

		snd_soc_jack_report(jack, report, imx_hp_jack_gpio.report);
	}

}

static int hp_jack_status_check(void)
{
	struct imx_priv *priv = &card_priv;
	struct platform_device *pdev = priv->pdev;
	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;
	char *envp[3];
	char *buf;
	int  ret = 0;

	if (gpio_is_valid(plat->hp_gpio)) {
		int state=0;
		priv->hp_status = gpio_get_value(plat->hp_gpio);
		//only auto connect while in non android os
		#ifndef CONFIG_ANDROID
		/* if headphone is inserted, disable speaker */
		if (priv->hp_status != plat->hp_active_low)
			snd_soc_dapm_nc_pin(&gcodec->dapm, "Ext Speaker");
		else
			snd_soc_dapm_enable_pin(&gcodec->dapm, "Ext Speaker");

		snd_soc_dapm_sync(&gcodec->dapm);
		#endif

		buf = kmalloc(32, GFP_ATOMIC);
		if (!buf) {
			pr_err("%s kmalloc failed\n", __func__);
			return -ENOMEM;
		}

		if (priv->hp_status != plat->hp_active_low) {
			/*
			 *	state meaning
			 *	0: no headset plug in
			 *	1: headset with microphone plugged
			 *	2: headset without microphone plugged
			 *    if
			*/
			state = 2;
			if(rt5625_headset_detect(gcodec))
				state=1;
			switch_set_state(&priv->sdev, state);
			snprintf(buf, 32, "STATE=%d", state);
			ret = imx_hp_jack_gpio.report;
		} else {
			switch_set_state(&priv->sdev, state);
			snprintf(buf, 32, "STATE=%d", state);
		}
		if(plat->headphone_switch)
			plat->headphone_switch(state);

		envp[0] = "NAME=headphone";
		envp[1] = buf;
		envp[2] = NULL;
		kobject_uevent_env(&pdev->dev.kobj, KOBJ_CHANGE, envp);
		kfree(buf);
	}

	return ret;
}

static DECLARE_DELAYED_WORK(resume_hp_event, imx_resume_event);

static int imx_hifi_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct imx_priv *priv = &card_priv;
	struct platform_device *pdev = priv->pdev;
	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;

	if (SNDRV_PCM_TRIGGER_RESUME == cmd) {
		if (gpio_is_valid(plat->hp_gpio) || gpio_is_valid(plat->mic_gpio))
			schedule_delayed_work(&resume_hp_event,
				msecs_to_jiffies(200));
	}

	return 0;
}

static int imx_rt5625_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct imx_priv *priv = &card_priv;
	struct platform_device *pdev = priv->pdev;
	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;
	int ret;


	gcodec = codec;

	/* Add imx specific widgets */
	snd_soc_dapm_new_controls(&codec->dapm, imx_dapm_widgets,
				  ARRAY_SIZE(imx_dapm_widgets));

	/* Set up imx specific audio path audio_map */
	snd_soc_dapm_add_routes(&codec->dapm, audio_map, ARRAY_SIZE(audio_map));

	snd_soc_dapm_sync(&codec->dapm);

	if (gpio_is_valid(plat->hp_gpio)) {
		imx_hp_jack_gpio.gpio = plat->hp_gpio;
		imx_hp_jack_gpio.jack_status_check = hp_jack_status_check;

		ret = snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
				&imx_hp_jack);
		if(ret) goto error;
		ret = snd_soc_jack_add_gpios(&imx_hp_jack,
					1, &imx_hp_jack_gpio);
		if(ret) goto error;

		ret = driver_create_file(pdev->dev.driver,
							&driver_attr_headphone);
		if (ret < 0) {
			ret = -EINVAL;
			return ret;
		}

		priv->hp_status = gpio_get_value(plat->hp_gpio);
		
	}

	snd_soc_dapm_nc_pin(&codec->dapm,"HPL");
	snd_soc_dapm_nc_pin(&codec->dapm,"HPR");
	snd_soc_dapm_nc_pin(&codec->dapm,"SPKL");
	snd_soc_dapm_nc_pin(&codec->dapm,"SPKR");
	snd_soc_dapm_enable_pin(&codec->dapm, "Ext Speaker");
	snd_soc_dapm_enable_pin(&codec->dapm, "Headphone Jack");
	snd_soc_dapm_sync(&codec->dapm);
	
error:	
	return 0;
}

static struct snd_soc_ops imx_hifi_ops = {
	.startup = imx_hifi_startup,
	.shutdown = imx_hifi_shutdown,
	.hw_params = imx_hifi_hw_params,
	.trigger = imx_hifi_trigger,
};

static struct snd_soc_dai_link imx_dai[] = {
	{
		.name = "HiFi",
		.stream_name = "HiFi",
		.codec_dai_name	= "rt5625_hifi",
		.codec_name	= "rt5625.0-001f",
		.cpu_dai_name	= "imx-ssi.1",
		.platform_name	= "imx-pcm-audio.1",
		.init		= imx_rt5625_init,
		.ops		= &imx_hifi_ops,
	},
};

static struct snd_soc_card snd_soc_card_imx = {
	.name		= "rt5625-audio",
	.dai_link	= imx_dai,
	.num_links	= ARRAY_SIZE(imx_dai),
};

/*
 * slave is internal port (1/2/7),master is external port(3/4/5/6)
 * In our scenario ,rt5625 is slave device (SND_SOC_DAIFMT_CBS_CFS),
 * We should set external port TFS and TCLK from internal port
 * our mapping ssi.1<-->SSI2<-->SSI3
*/
static int imx_audmux_config(int slave, int master)
{
	unsigned int ptcr, pdcr;
	slave = slave - 1;
	master = master - 1;
	ptcr = MXC_AUDMUX_V2_PTCR_SYN |
		MXC_AUDMUX_V2_PTCR_TFSDIR |
		MXC_AUDMUX_V2_PTCR_TFSEL(slave) |
		MXC_AUDMUX_V2_PTCR_TCLKDIR |
		MXC_AUDMUX_V2_PTCR_TCSEL(slave);
	pdcr = MXC_AUDMUX_V2_PDCR_RXDSEL(slave)|MXC_AUDMUX_V2_PDCR_TXRXEN;
	mxc_audmux_v2_configure_port(master, ptcr, pdcr);

	ptcr = MXC_AUDMUX_V2_PTCR_SYN;
	pdcr = MXC_AUDMUX_V2_PDCR_RXDSEL(master);
	mxc_audmux_v2_configure_port(slave, ptcr, pdcr);

	return 0;
}

/*
 * This function will register the snd_soc_pcm_link drivers.
 */
static int __devinit imx_rt5625_probe(struct platform_device *pdev)
{
	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;	
	struct imx_priv *priv = &card_priv;
	int ret = 0;

	card_priv.pdev = pdev;
	
	card_priv.codec_mclk = clk_get(NULL, "clko2_clk");
	if (IS_ERR(card_priv.codec_mclk)) {
		printk(KERN_ERR "can't get CLKO2 clock.\n");
		return PTR_ERR(card_priv.codec_mclk);
	}

	imx_audmux_config(plat->src_port, plat->ext_port);

	if (plat->init && plat->init()) {
		ret = -EINVAL;
		return ret;
	}

	card_priv.sysclk = plat->sysclk;

	
	priv->sdev.name = "h2w";
	ret = switch_dev_register(&priv->sdev);
	if (ret < 0) {
		ret = -EINVAL;
		return ret;
	}

	if (gpio_is_valid(plat->hp_gpio)) {
		priv->hp_status = gpio_get_value(plat->hp_gpio);
		if (priv->hp_status != plat->hp_active_low)
			switch_set_state(&priv->sdev, 2);
		else
			switch_set_state(&priv->sdev, 0);
	}
	
	return ret;
}

static int __devexit imx_rt5625_remove(struct platform_device *pdev)
{
	struct mxc_audio_platform_data *plat = pdev->dev.platform_data;

	if (plat->finit)
		plat->finit();

	return 0;
}

static struct platform_driver imx_rt5625_driver = {
	.probe = imx_rt5625_probe,
	.remove = imx_rt5625_remove,
	.driver = {
		   .name = "imx-rt5625",
		   .owner = THIS_MODULE,
		   },
};

static struct platform_device *imx_snd_device;

static int __init imx_asoc_init(void)
{
	int ret;

	ret = platform_driver_register(&imx_rt5625_driver);
	if (ret < 0)
		goto exit;

	imx_snd_device = platform_device_alloc("soc-audio", 8);
	if (!imx_snd_device)
		goto err_device_alloc;

	platform_set_drvdata(imx_snd_device, &snd_soc_card_imx);

	ret = platform_device_add(imx_snd_device);

	if (0 == ret)
		goto exit;

	platform_device_put(imx_snd_device);

err_device_alloc:
	platform_driver_unregister(&imx_rt5625_driver);
exit:
	return ret;
}

static void __exit imx_asoc_exit(void)
{
	platform_driver_unregister(&imx_rt5625_driver);
	platform_device_unregister(imx_snd_device);
}

module_init(imx_asoc_init);
module_exit(imx_asoc_exit);

/* Module information */
MODULE_DESCRIPTION("ALSA SoC imx rt5625");
MODULE_LICENSE("GPL");
