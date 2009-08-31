/*
 * Samsung N130 and NC120 Laptop driver
 *
 * Copyright (C) 2009 Greg Kroah-Hartman (gregkh@suse.de)
 * Copyright (C) 2009 Novell Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/dmi.h>
#include <linux/platform_device.h>

/*
 * This driver is needed because a number of Samsung laptops do not hook
 * their control settings through ACPI.  So we have to poke around in the
 * BIOS to do things like brightness values, and "special" key controls.
 */


/*
 * We have 0 - 8 as valid brightness levels.  The specs say that level 0 should
 * be reserved by the BIOS (which really doesn't make much sense), we tell
 * userspace that the value is 0 - 7 and then just tell the hardware 1 - 8
 */
#define MAX_BRIGHT	0x07

/* get model returns 4 characters that describe the model of the laptop */
#define SABI_GET_MODEL			0x04

/* Brightness is 0 - 8, as described above.  Value 0 is for the BIOS to use */
#define SABI_GET_BRIGHTNESS		0x10
#define SABI_SET_BRIGHTNESS		0x11

/* 0 is off, 1 is on, and 2 is a second user-defined key? */
#define SABI_GET_WIRELESS_BUTTON	0x12
#define SABI_SET_WIRELESS_BUTTON	0x13

/* Temperature is returned in degress Celsius from what I can guess. */
#define SABI_GET_CPU_TEMP		0x29

/* 0 is off, 1 is on.  Doesn't seem to work on a N130 for some reason */
#define SABI_GET_BACKLIGHT		0x2d
#define SABI_SET_BACKLIGHT		0x2e

/*
 * There is 3 different modes here:
 *   0 - off
 *   1 - on
 *   2 - max performance mode
 * off is "normal" mode.
 */
#define SABI_GET_ETIQUETTE_MODE		0x31
#define SABI_SET_ETIQUETTE_MODE		0x32

#define SABI_HEADER_PORT		0x00
#define SABI_HEADER_IFACEFUNC		0x02
#define SABI_HEADER_EN_MEM		0x03
#define SABI_HEADER_RE_MEM		0x04
#define SABI_HEADER_DATA_OFFSET		0x05
#define SABI_HEADER_DATA_SEGMENT	0x07

#define SABI_IFACE_MAIN			0x00
#define SABI_IFACE_SUB			0x02
#define SABI_IFACE_COMPLETE		0x04
#define SABI_IFACE_DATA			0x05

/* Structure to get data back to the calling function */
struct sabi_retval {
	u8 retval[20];
};

static void __iomem *sabi;
static void __iomem *sabi_iface;
static void __iomem *f0000_segment;
static struct backlight_device *backlight_device;
static struct mutex sabi_mutex;
static struct platform_device *sdev;

static int force;
module_param(force, bool, 0);
MODULE_PARM_DESC(force, "Disable the DMI check and forces the driver to be loaded");

static int debug;
module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug enabled or not");

static int sabi_get_command(u8 command, struct sabi_retval *sretval)
{
	int retval = 0;
	u16 port = readw(sabi + SABI_HEADER_PORT);

	mutex_lock(&sabi_mutex);

	/* enable memory to be able to write to it */
	outb(readb(sabi + SABI_HEADER_EN_MEM), port);

	/* write out the command */
	writew(0x5843, sabi_iface + SABI_IFACE_MAIN);
	writew(command, sabi_iface + SABI_IFACE_SUB);
	writeb(0, sabi_iface + SABI_IFACE_COMPLETE);
	outb(readb(sabi + SABI_HEADER_IFACEFUNC), port);

	/* sleep for a bit to let the command complete */
	msleep(100);

	/* write protect memory to make it safe */
	outb(readb(sabi + SABI_HEADER_RE_MEM), port);

	/* see if the command actually succeeded */
	if (readb(sabi_iface + SABI_IFACE_COMPLETE) == 0xaa &&
	    readb(sabi_iface + SABI_IFACE_DATA) != 0xff) {
		/*
		 * It did!
		 * Save off the data into a structure so the caller use it.
		 * Right now we only care about the first 4 bytes,
		 * I suppose there are commands that need more, but I don't
		 * know about them.
		 */
		sretval->retval[0] = readb(sabi_iface + SABI_IFACE_DATA);
		sretval->retval[1] = readb(sabi_iface + SABI_IFACE_DATA + 1);
		sretval->retval[2] = readb(sabi_iface + SABI_IFACE_DATA + 2);
		sretval->retval[3] = readb(sabi_iface + SABI_IFACE_DATA + 3);
		goto exit;
	}

	/* Something bad happened, so report it and error out */
	printk(KERN_WARNING "SABI command 0x%02x failed with completion flag 0x%02x and output 0x%02x\n",
		command, readb(sabi_iface + SABI_IFACE_COMPLETE),
		readb(sabi_iface + SABI_IFACE_DATA));
	retval = -EINVAL;
exit:
	mutex_unlock(&sabi_mutex);
	return retval;

}

static int sabi_set_command(u8 command, u8 data)
{
	int retval = 0;
	u16 port = readw(sabi + SABI_HEADER_PORT);

	mutex_lock(&sabi_mutex);

	/* enable memory to be able to write to it */
	outb(readb(sabi + SABI_HEADER_EN_MEM), port);

	/* write out the command */
	writew(0x5843, sabi_iface + SABI_IFACE_MAIN);
	writew(command, sabi_iface + SABI_IFACE_SUB);
	writeb(0, sabi_iface + SABI_IFACE_COMPLETE);
	writeb(data, sabi_iface + SABI_IFACE_DATA);
	outb(readb(sabi + SABI_HEADER_IFACEFUNC), port);

	/* sleep for a bit to let the command complete */
	msleep(100);

	/* write protect memory to make it safe */
	outb(readb(sabi + SABI_HEADER_RE_MEM), port);

	/* see if the command actually succeeded */
	if (readb(sabi_iface + SABI_IFACE_COMPLETE) == 0xaa &&
	    readb(sabi_iface + SABI_IFACE_DATA) != 0xff) {
		/* it did! */
		goto exit;
	}

	/* Something bad happened, so report it and error out */
	printk(KERN_WARNING "SABI command 0x%02x failed with completion flag 0x%02x and output 0x%02x\n",
		command, readb(sabi_iface + SABI_IFACE_COMPLETE),
		readb(sabi_iface + SABI_IFACE_DATA));
	retval = -EINVAL;
exit:
	mutex_unlock(&sabi_mutex);
	return retval;
}

static u8 read_brightness(void)
{
	struct sabi_retval sretval;
	int user_brightness = 0;
	int retval;

	retval = sabi_get_command(SABI_GET_BACKLIGHT, &sretval);
	if (!retval)
		user_brightness = sretval.retval[0];
		if (user_brightness != 0)
			--user_brightness;
	return user_brightness;
}

static void set_brightness(u8 user_brightness)
{
	sabi_set_command(SABI_SET_BRIGHTNESS, user_brightness + 1);
}

static int get_brightness(struct backlight_device *bd)
{
	return bd->props.brightness;
}

static int update_status(struct backlight_device *bd)
{
	set_brightness(bd->props.brightness);
	return 0;
}

static struct backlight_ops backlight_ops = {
	.get_brightness	= get_brightness,
	.update_status	= update_status,
};

static int __init dmi_check_cb(const struct dmi_system_id *id)
{
	printk(KERN_INFO KBUILD_MODNAME ": found laptop model '%s'\n",
		id->ident);
	return 0;
}

static struct dmi_system_id __initdata samsung_dmi_table[] = {
	{
		.ident = "N120",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
			DMI_MATCH(DMI_PRODUCT_NAME, "N120"),
			DMI_MATCH(DMI_BOARD_NAME, "N120"),
		},
		.callback = dmi_check_cb,
	},
	{
		.ident = "N130",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
			DMI_MATCH(DMI_PRODUCT_NAME, "N130"),
			DMI_MATCH(DMI_BOARD_NAME, "N130"),
		},
		.callback = dmi_check_cb,
	},
	{
		.ident = "NP-Q45",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
			DMI_MATCH(DMI_PRODUCT_NAME, "SQ45S70S"),
			DMI_MATCH(DMI_BOARD_NAME, "SQ45S70S"),
		},
		.callback = dmi_check_cb,
	},
	{
		.ident = "R510/P510",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
			DMI_MATCH(DMI_PRODUCT_NAME, "R510/P510"),
			DMI_MATCH(DMI_BOARD_NAME, "R510/P510"),
		},
		.callback = dmi_check_cb,
	},
	{ },
};
MODULE_DEVICE_TABLE(dmi, samsung_dmi_table);

static int __init samsung_init(void)
{
	struct sabi_retval sretval;
	const char *testStr = "SwSmi@";
	void __iomem *memcheck;
	unsigned int ifaceP;
	int pStr;
	int loca;
	int retval;

	mutex_init(&sabi_mutex);

	if (!force && !dmi_check_system(samsung_dmi_table))
		return -ENODEV;

	f0000_segment = ioremap(0xf0000, 0xffff);
	if (!f0000_segment) {
		printk(KERN_ERR "Can't map the segment at 0xf0000\n");
		return -EINVAL;
	}

	/* Try to find the signature "SwSmi@" in memory to find the header */
	pStr = 0;
	memcheck = f0000_segment;
	for (loca = 0; loca < 0xffff; loca++) {
		char temp = readb(memcheck + loca);

		if (temp == testStr[pStr]) {
			if (pStr == 5)
				break;
			++pStr;
		} else {
			pStr = 0;
		}
	}
	if (loca == 0xffff) {
		printk(KERN_INFO "This computer does not support SABI\n");
		goto error_no_signature;
		}

	/* point to the SMI port Number */
	loca += 1;
	sabi = (memcheck + loca);

	/* Get a pointer to the SABI Interface */
	ifaceP = (readw(sabi + SABI_HEADER_DATA_SEGMENT) & 0x0ffff) << 4;
	ifaceP += readw(sabi + SABI_HEADER_DATA_OFFSET) & 0x0ffff;
	sabi_iface = ioremap(ifaceP, 16);
	if (!sabi_iface) {
		printk(KERN_ERR "Can't remap %x\n", ifaceP);
		goto exit;
	}

	if (debug) {
		retval = sabi_get_command(SABI_GET_MODEL, &sretval);
		printk(KERN_INFO "Model Name %c%c%c%c\n",
			sretval.retval[0],
			sretval.retval[1],
			sretval.retval[2],
			sretval.retval[3]);

		retval = sabi_get_command(SABI_GET_BACKLIGHT, &sretval);
		printk(KERN_INFO "backlight = 0x%02x\n", sretval.retval[0]);

		retval = sabi_get_command(SABI_GET_WIRELESS_BUTTON, &sretval);
		printk(KERN_INFO "wireless button = 0x%02x\n",
			sretval.retval[0]);

		retval = sabi_get_command(SABI_GET_BRIGHTNESS, &sretval);
		printk(KERN_INFO "brightness = 0x%02x\n", sretval.retval[0]);

		retval = sabi_get_command(SABI_GET_ETIQUETTE_MODE, &sretval);
		printk(KERN_INFO "etiquette mode = 0x%02x\n",
			sretval.retval[0]);

		retval = sabi_get_command(SABI_GET_CPU_TEMP, &sretval);
		printk(KERN_INFO "cpu temp = 0x%02x\n", sretval.retval[0]);
	}

	/* knock up a platform device to hang stuff off of */
	sdev = platform_device_register_simple("samsung", -1, NULL, 0);
	if (IS_ERR(sdev))
		goto error_no_platform;

	/* create a backlight device to talk to this one */
	backlight_device = backlight_device_register("samsung", &sdev->dev,
						     NULL, &backlight_ops);
	if (IS_ERR(backlight_device))
		goto error_no_backlight;

	backlight_device->props.max_brightness = MAX_BRIGHT;
	backlight_device->props.brightness = read_brightness();
	backlight_device->props.power = FB_BLANK_UNBLANK;
	backlight_update_status(backlight_device);

exit:
	return 0;

error_no_backlight:
	platform_device_unregister(sdev);

error_no_platform:
	iounmap(sabi_iface);

error_no_signature:
	iounmap(f0000_segment);
	return -EINVAL;
}

static void __exit samsung_exit(void)
{
	backlight_device_unregister(backlight_device);
	iounmap(sabi_iface);
	iounmap(f0000_segment);
	platform_device_unregister(sdev);
}

module_init(samsung_init);
module_exit(samsung_exit);

MODULE_AUTHOR("Greg Kroah-Hartman <gregkh@suse.de>");
MODULE_DESCRIPTION("Samsung Backlight driver");
MODULE_LICENSE("GPL");
