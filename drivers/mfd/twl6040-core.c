/*
 * MFD driver for TWL6040 audio device
 *
 * Authors:	Misael Lopez Cruz <misael.lopez@ti.com>
 *		Jorge Eduardo Candelaria <jorge.candelaria@ti.com>
 *		Peter Ujfalusi <peter.ujfalusi@ti.com>
 *
 * Copyright:	(C) 2011 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/i2c/twl.h>
#include <linux/mfd/core.h>
#include <linux/mfd/twl6040.h>

static struct platform_device *twl6040_dev;

int twl6040_reg_read(struct twl6040 *twl6040, unsigned int reg)
{
	int ret;
	u8 val = 0;

	mutex_lock(&twl6040->io_mutex);
	ret = twl_i2c_read_u8(TWL_MODULE_AUDIO_VOICE, &val, reg);
	if (ret < 0) {
		mutex_unlock(&twl6040->io_mutex);
		return ret;
	}
	mutex_unlock(&twl6040->io_mutex);

	return val;
}
EXPORT_SYMBOL(twl6040_reg_read);

int twl6040_reg_write(struct twl6040 *twl6040, unsigned int reg, u8 val)
{
	int ret;

	mutex_lock(&twl6040->io_mutex);
	ret = twl_i2c_write_u8(TWL_MODULE_AUDIO_VOICE, val, reg);
	mutex_unlock(&twl6040->io_mutex);

	return ret;
}
EXPORT_SYMBOL(twl6040_reg_write);

int twl6040_set_bits(struct twl6040 *twl6040, unsigned int reg, u8 mask)
{
	int ret;
	u8 val;

	mutex_lock(&twl6040->io_mutex);
	ret = twl_i2c_read_u8(TWL_MODULE_AUDIO_VOICE, &val, reg);
	if (ret)
		goto out;

	val |= mask;
	ret = twl_i2c_write_u8(TWL_MODULE_AUDIO_VOICE, val, reg);
out:
	mutex_unlock(&twl6040->io_mutex);
	return ret;
}
EXPORT_SYMBOL(twl6040_set_bits);

int twl6040_clear_bits(struct twl6040 *twl6040, unsigned int reg, u8 mask)
{
	int ret;
	u8 val;

	mutex_lock(&twl6040->io_mutex);
	ret = twl_i2c_read_u8(TWL_MODULE_AUDIO_VOICE, &val, reg);
	if (ret)
		goto out;

	val &= ~mask;
	ret = twl_i2c_write_u8(TWL_MODULE_AUDIO_VOICE, val, reg);
out:
	mutex_unlock(&twl6040->io_mutex);
	return ret;
}
EXPORT_SYMBOL(twl6040_clear_bits);

/* twl6040 codec manual power-up sequence */
static int twl6040_power_up(struct twl6040 *twl6040)
{
	u8 ldoctl, ncpctl, lppllctl;
	int ret;

	/* enable high-side LDO, reference system and internal oscillator */
	ldoctl = TWL6040_HSLDOENA | TWL6040_REFENA | TWL6040_OSCENA;
	ret = twl6040_reg_write(twl6040, TWL6040_REG_LDOCTL, ldoctl);
	if (ret)
		return ret;
	usleep_range(10000, 10500);

	/* enable negative charge pump */
	ncpctl = TWL6040_NCPENA;
	ret = twl6040_reg_write(twl6040, TWL6040_REG_NCPCTL, ncpctl);
	if (ret)
		goto ncp_err;
	usleep_range(1000, 1500);

	/* enable low-side LDO */
	ldoctl |= TWL6040_LSLDOENA;
	ret = twl6040_reg_write(twl6040, TWL6040_REG_LDOCTL, ldoctl);
	if (ret)
		goto lsldo_err;
	usleep_range(1000, 1500);

	/* enable low-power PLL */
	lppllctl = TWL6040_LPLLENA;
	ret = twl6040_reg_write(twl6040, TWL6040_REG_LPPLLCTL, lppllctl);
	if (ret)
		goto lppll_err;
	usleep_range(5000, 5500);

	/* disable internal oscillator */
	ldoctl &= ~TWL6040_OSCENA;
	ret = twl6040_reg_write(twl6040, TWL6040_REG_LDOCTL, ldoctl);
	if (ret)
		goto osc_err;

	return 0;

osc_err:
	lppllctl &= ~TWL6040_LPLLENA;
	twl6040_reg_write(twl6040, TWL6040_REG_LPPLLCTL, lppllctl);
lppll_err:
	ldoctl &= ~TWL6040_LSLDOENA;
	twl6040_reg_write(twl6040, TWL6040_REG_LDOCTL, ldoctl);
lsldo_err:
	ncpctl &= ~TWL6040_NCPENA;
	twl6040_reg_write(twl6040, TWL6040_REG_NCPCTL, ncpctl);
ncp_err:
	ldoctl &= ~(TWL6040_HSLDOENA | TWL6040_REFENA | TWL6040_OSCENA);
	twl6040_reg_write(twl6040, TWL6040_REG_LDOCTL, ldoctl);

	return ret;
}

/* twl6040 manual power-down sequence */
static void twl6040_power_down(struct twl6040 *twl6040)
{
	u8 ncpctl, ldoctl, lppllctl;

	ncpctl = twl6040_reg_read(twl6040, TWL6040_REG_NCPCTL);
	ldoctl = twl6040_reg_read(twl6040, TWL6040_REG_LDOCTL);
	lppllctl = twl6040_reg_read(twl6040, TWL6040_REG_LPPLLCTL);

	/* enable internal oscillator */
	ldoctl |= TWL6040_OSCENA;
	twl6040_reg_write(twl6040, TWL6040_REG_LDOCTL, ldoctl);
	usleep_range(1000, 1500);

	/* disable low-power PLL */
	lppllctl &= ~TWL6040_LPLLENA;
	twl6040_reg_write(twl6040, TWL6040_REG_LPPLLCTL, lppllctl);

	/* disable low-side LDO */
	ldoctl &= ~TWL6040_LSLDOENA;
	twl6040_reg_write(twl6040, TWL6040_REG_LDOCTL, ldoctl);

	/* disable negative charge pump */
	ncpctl &= ~TWL6040_NCPENA;
	twl6040_reg_write(twl6040, TWL6040_REG_NCPCTL, ncpctl);

	/* disable high-side LDO, reference system and internal oscillator */
	ldoctl &= ~(TWL6040_HSLDOENA | TWL6040_REFENA | TWL6040_OSCENA);
	twl6040_reg_write(twl6040, TWL6040_REG_LDOCTL, ldoctl);
}

static irqreturn_t twl6040_naudint_handler(int irq, void *data)
{
	struct twl6040 *twl6040 = data;
	u8 intid, status;

	intid = twl6040_reg_read(twl6040, TWL6040_REG_INTID);

	if (intid & TWL6040_READYINT)
		complete(&twl6040->ready);

	if (intid & TWL6040_THINT) {
		status = twl6040_reg_read(twl6040, TWL6040_REG_STATUS);
		if (status & TWL6040_TSHUTDET) {
			dev_warn(&twl6040_dev->dev,
				 "Thermal shutdown, powering-off");
			twl6040_power(twl6040, 0);
		} else {
			dev_warn(&twl6040_dev->dev,
				 "Leaving thermal shutdown, powering-on");
			twl6040_power(twl6040, 1);
		}
	}

	return IRQ_HANDLED;
}

static int twl6040_power_up_completion(struct twl6040 *twl6040,
				       int naudint)
{
	int time_left;
	u8 intid;

	time_left = wait_for_completion_timeout(&twl6040->ready,
						msecs_to_jiffies(144));
	if (!time_left) {
		intid = twl6040_reg_read(twl6040, TWL6040_REG_INTID);
		if (!(intid & TWL6040_READYINT)) {
			dev_err(&twl6040_dev->dev,
				"timeout waiting for READYINT\n");
			return -ETIMEDOUT;
		}
	}

	return 0;
}

int twl6040_power(struct twl6040 *twl6040, int on)
{
	int audpwron = twl6040->audpwron;
	int naudint = twl6040->irq;
	int ret = 0;

	mutex_lock(&twl6040->mutex);

	if (on) {
		/* already powered-up */
		if (twl6040->power_count++)
			goto out;

		if (gpio_is_valid(audpwron)) {
			/* use AUDPWRON line */
			gpio_set_value(audpwron, 1);
			/* wait for power-up completion */
			ret = twl6040_power_up_completion(twl6040, naudint);
			if (ret) {
				dev_err(&twl6040_dev->dev,
					"automatic power-down failed\n");
				twl6040->power_count = 0;
				goto out;
			}
		} else {
			/* use manual power-up sequence */
			ret = twl6040_power_up(twl6040);
			if (ret) {
				dev_err(&twl6040_dev->dev,
					"manual power-up failed\n");
				twl6040->power_count = 0;
				goto out;
			}
		}
		twl6040->pll = TWL6040_LPPLL_ID;
		twl6040->sysclk = 19200000;
	} else {
		/* already powered-down */
		if (!twl6040->power_count) {
			dev_err(&twl6040_dev->dev,
				"device is already powered-off\n");
			ret = -EPERM;
			goto out;
		}

		if (--twl6040->power_count)
			goto out;

		if (gpio_is_valid(audpwron)) {
			/* use AUDPWRON line */
			gpio_set_value(audpwron, 0);

			/* power-down sequence latency */
			usleep_range(500, 700);
		} else {
			/* use manual power-down sequence */
			twl6040_power_down(twl6040);
		}
		twl6040->pll = TWL6040_NOPLL_ID;
		twl6040->sysclk = 0;
	}

out:
	mutex_unlock(&twl6040->mutex);
	return ret;
}
EXPORT_SYMBOL(twl6040_power);

int twl6040_is_powered(struct twl6040 *twl6040)
{
	return twl6040->power_count;
}
EXPORT_SYMBOL(twl6040_is_powered);

int twl6040_set_pll(struct twl6040 *twl6040, enum twl6040_pll_id id,
		    unsigned int freq_in, unsigned int freq_out)
{
	u8 hppllctl, lppllctl;
	int ret = 0;

	mutex_lock(&twl6040->mutex);

	hppllctl = twl6040_reg_read(twl6040, TWL6040_REG_HPPLLCTL);
	lppllctl = twl6040_reg_read(twl6040, TWL6040_REG_LPPLLCTL);

	switch (id) {
	case TWL6040_LPPLL_ID:
		/* low-power PLL divider */
		switch (freq_out) {
		case 17640000:
			lppllctl |= TWL6040_LPLLFIN;
			break;
		case 19200000:
			lppllctl &= ~TWL6040_LPLLFIN;
			break;
		default:
			dev_err(&twl6040_dev->dev,
				"freq_out %d not supported\n", freq_out);
			ret = -EINVAL;
			goto pll_out;
		}
		twl6040_reg_write(twl6040, TWL6040_REG_LPPLLCTL, lppllctl);

		switch (freq_in) {
		case 32768:
			lppllctl |= TWL6040_LPLLENA;
			twl6040_reg_write(twl6040, TWL6040_REG_LPPLLCTL,
					  lppllctl);
			mdelay(5);
			lppllctl &= ~TWL6040_HPLLSEL;
			twl6040_reg_write(twl6040, TWL6040_REG_LPPLLCTL,
					  lppllctl);
			hppllctl &= ~TWL6040_HPLLENA;
			twl6040_reg_write(twl6040, TWL6040_REG_HPPLLCTL,
					  hppllctl);
			break;
		default:
			dev_err(&twl6040_dev->dev,
				"freq_in %d not supported\n", freq_in);
			ret = -EINVAL;
			goto pll_out;
		}

		twl6040->pll = TWL6040_LPPLL_ID;
		break;
	case TWL6040_HPPLL_ID:
		/* high-performance PLL can provide only 19.2 MHz */
		if (freq_out != 19200000) {
			dev_err(&twl6040_dev->dev,
				"freq_out %d not supported\n", freq_out);
			ret = -EINVAL;
			goto pll_out;
		}

		hppllctl &= ~TWL6040_MCLK_MSK;

		switch (freq_in) {
		case 12000000:
			/* PLL enabled, active mode */
			hppllctl |= TWL6040_MCLK_12000KHZ |
				    TWL6040_HPLLENA;
			break;
		case 19200000:
			/*
			 * PLL disabled
			 * (enable PLL if MCLK jitter quality
			 *  doesn't meet specification)
			 */
			hppllctl |= TWL6040_MCLK_19200KHZ;
			break;
		case 26000000:
			/* PLL enabled, active mode */
			hppllctl |= TWL6040_MCLK_26000KHZ |
				    TWL6040_HPLLENA;
			break;
		case 38400000:
			/* PLL enabled, active mode */
			hppllctl |= TWL6040_MCLK_38400KHZ |
				    TWL6040_HPLLENA;
			break;
		default:
			dev_err(&twl6040_dev->dev,
				"freq_in %d not supported\n", freq_in);
			ret = -EINVAL;
			goto pll_out;
		}

		/* enable clock slicer to ensure input waveform is square */
		hppllctl |= TWL6040_HPLLSQRENA;

		twl6040_reg_write(twl6040, TWL6040_REG_HPPLLCTL, hppllctl);
		usleep_range(500, 700);
		lppllctl |= TWL6040_HPLLSEL;
		twl6040_reg_write(twl6040, TWL6040_REG_LPPLLCTL, lppllctl);
		lppllctl &= ~TWL6040_LPLLENA;
		twl6040_reg_write(twl6040, TWL6040_REG_LPPLLCTL, lppllctl);

		twl6040->pll = TWL6040_HPPLL_ID;
		break;
	default:
		dev_err(&twl6040_dev->dev, "unknown pll id %d\n", id);
		ret = -EINVAL;
		goto pll_out;
	}

	twl6040->sysclk = freq_out;

pll_out:
	mutex_unlock(&twl6040->mutex);
	return ret;
}
EXPORT_SYMBOL(twl6040_set_pll);

enum twl6040_pll_id twl6040_get_pll(struct twl6040 *twl6040)
{
	return twl6040->pll;
}
EXPORT_SYMBOL(twl6040_get_pll);

unsigned int twl6040_get_sysclk(struct twl6040 *twl6040)
{
	return twl6040->sysclk;
}
EXPORT_SYMBOL(twl6040_get_sysclk);

static int __devinit twl6040_probe(struct platform_device *pdev)
{
	struct twl4030_audio_data *pdata = pdev->dev.platform_data;
	struct twl6040 *twl6040;
	struct mfd_cell *cell = NULL;
	int ret, children = 0;

	if (!pdata) {
		dev_err(&pdev->dev, "Platform data is missing\n");
		return -EINVAL;
	}

	twl6040 = kzalloc(sizeof(struct twl6040), GFP_KERNEL);
	if (!twl6040)
		return -ENOMEM;

	platform_set_drvdata(pdev, twl6040);

	twl6040_dev = pdev;
	twl6040->dev = &pdev->dev;
	twl6040->audpwron = pdata->audpwron_gpio;
	twl6040->irq = pdata->naudint_irq;
	twl6040->irq_base = pdata->irq_base;

	mutex_init(&twl6040->mutex);
	mutex_init(&twl6040->io_mutex);
	init_completion(&twl6040->ready);

	twl6040->rev = twl6040_reg_read(twl6040, TWL6040_REG_ASICREV);

	if (gpio_is_valid(twl6040->audpwron)) {
		ret = gpio_request(twl6040->audpwron, "audpwron");
		if (ret)
			goto gpio1_err;

		ret = gpio_direction_output(twl6040->audpwron, 0);
		if (ret)
			goto gpio2_err;
	}

	/* ERRATA: Automatic power-up is not possible in ES1.0 */
	if (twl6040_get_rev(twl6040) == TWL6040_REV_ES1_0)
		twl6040->audpwron = -EINVAL;

	if (twl6040->irq) {
		/* codec interrupt */
		ret = twl6040_irq_init(twl6040);
		if (ret)
			goto gpio2_err;

		ret = twl6040_request_irq(twl6040, TWL6040_IRQ_READY,
					  twl6040_naudint_handler, 0,
					  "twl6040_irq_ready", twl6040);
		if (ret) {
			dev_err(twl6040->dev, "READY IRQ request failed: %d\n",
				ret);
			goto irq_err;
		}
	}

	/* dual-access registers controlled by I2C only */
	twl6040_set_bits(twl6040, TWL6040_REG_ACCCTL, TWL6040_I2CSEL);

	if (pdata->codec) {
		cell = &twl6040->cells[children];
		cell->name = "twl6040-codec";
		cell->platform_data = pdata->codec;
		cell->pdata_size = sizeof(*pdata->codec);
		children++;
	}

	if (pdata->vibra) {
		cell = &twl6040->cells[children];
		cell->name = "twl6040-vibra";
		cell->platform_data = pdata->vibra;
		cell->pdata_size = sizeof(*pdata->vibra);
		children++;
	}

	if (children) {
		ret = mfd_add_devices(&pdev->dev, pdev->id, twl6040->cells,
				      children, NULL, 0);
		if (ret)
			goto mfd_err;
	} else {
		dev_err(&pdev->dev, "No platform data found for children\n");
		ret = -ENODEV;
		goto mfd_err;
	}

	return 0;

mfd_err:
	if (twl6040->irq)
		twl6040_free_irq(twl6040, TWL6040_IRQ_READY, twl6040);
irq_err:
	if (twl6040->irq)
		twl6040_irq_exit(twl6040);
gpio2_err:
	if (gpio_is_valid(twl6040->audpwron))
		gpio_free(twl6040->audpwron);
gpio1_err:
	platform_set_drvdata(pdev, NULL);
	kfree(twl6040);
	twl6040_dev = NULL;
	return ret;
}

static int __devexit twl6040_remove(struct platform_device *pdev)
{
	struct twl6040 *twl6040 = platform_get_drvdata(pdev);

	if (twl6040_is_powered(twl6040))
		twl6040_power(twl6040, 0);

	if (gpio_is_valid(twl6040->audpwron))
		gpio_free(twl6040->audpwron);

	twl6040_free_irq(twl6040, TWL6040_IRQ_READY, twl6040);

	if (twl6040->irq)
		twl6040_irq_exit(twl6040);

	mfd_remove_devices(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	kfree(twl6040);
	twl6040_dev = NULL;

	return 0;
}

static struct platform_driver twl6040_driver = {
	.probe		= twl6040_probe,
	.remove		= __devexit_p(twl6040_remove),
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "twl6040",
	},
};

static int __devinit twl6040_init(void)
{
	return platform_driver_register(&twl6040_driver);
}
module_init(twl6040_init);

static void __devexit twl6040_exit(void)
{
	platform_driver_unregister(&twl6040_driver);
}

module_exit(twl6040_exit);

MODULE_DESCRIPTION("TWL6040 MFD");
MODULE_AUTHOR("Misael Lopez Cruz <misael.lopez@ti.com>");
MODULE_AUTHOR("Jorge Eduardo Candelaria <jorge.candelaria@ti.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:twl6040");