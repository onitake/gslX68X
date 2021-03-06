/* -------------------------------------------------------------------------
 * Copyright (C) 2014-2015, Intel Corporation
 *
 * Derived from:
 *  gslX68X.c
 *  Copyright (C) 2010-2015, Shanghai Sileadinc Co.Ltd
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * ------------------------------------------------------------------------- */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/pm.h>
#include <linux/irq.h>

#define SILEAD_TS_NAME "silead_ts"

#define SILEAD_REG_RESET	0xE0
#define SILEAD_REG_DATA		0x80
#define SILEAD_REG_TOUCH_NR	0x80
#define SILEAD_REG_POWER	0xBC
#define SILEAD_REG_CLOCK	0xE4
#define SILEAD_REG_STATUS	0xB0
#define SILEAD_REG_ID		0xFC
#define SILEAD_REG_MEM_CHECK	0xB0

#define SILEAD_STATUS_OK	0x5A5A5A5A
#define SILEAD_TS_DATA_LEN	44
#define SILEAD_CLOCK		0x04

#define SILEAD_CMD_RESET	0x88
#define SILEAD_CMD_START	0x00

#define SILEAD_POINT_DATA_LEN	0x04
#define SILEAD_POINT_Y_OFF      0x00
#define SILEAD_POINT_Y_MSB_OFF	0x01
#define SILEAD_POINT_X_OFF	0x02
#define SILEAD_POINT_X_MSB_OFF	0x03
#define SILEAD_POINT_HSB_MASK	0x0F
#define SILEAD_TOUCH_ID_MASK	0xF0

#define SILEAD_DP_X_INVERT	"touchscreen-x-invert"
#define SILEAD_DP_Y_INVERT	"touchscreen-y-invert"
#define SILEAD_DP_XY_SWAP	"touchscreen-xy-swap"
#define SILEAD_DP_X_MAX		"touchscreen-size-x"
#define SILEAD_DP_Y_MAX		"touchscreen-size-y"
#define SILEAD_DP_MAX_FINGERS	"touchscreen-max-fingers"
#define SILEAD_DP_FW_NAME	"touchscreen-fw-name"
#define SILEAD_PWR_GPIO_NAME	"power"

#define SILEAD_CMD_SLEEP_MIN	10000
#define SILEAD_CMD_SLEEP_MAX	20000
#define SILEAD_POWER_SLEEP	20
#define SILEAD_STARTUP_SLEEP	30

#define SILEAD_MAX_FINGERS	10
#define SILEAD_MAX_X		4095
#define SILEAD_MAX_Y		4095

enum silead_ts_power {
	SILEAD_POWER_ON  = 1,
	SILEAD_POWER_OFF = 0
};

struct silead_ts_data {
	struct i2c_client *client;
	struct gpio_desc *gpio_power;
	struct input_dev *input;
	const char *custom_fw_name;
	char fw_name[I2C_NAME_SIZE];
	u16 x_max;
	u16 y_max;
	u8 max_fingers;
	bool x_invert;
	bool y_invert;
	bool xy_swap;
	u32 chip_id;
	struct input_mt_pos pos[SILEAD_MAX_FINGERS];
	int slots[SILEAD_MAX_FINGERS];
};

struct silead_fw_data {
	u32 offset;
	u32 val;
};

static int silead_ts_request_input_dev(struct silead_ts_data *data)
{
	struct device *dev = &data->client->dev;
	int error;

	data->input = devm_input_allocate_device(dev);
	if (!data->input) {
		dev_err(dev,
			"Failed to allocate input device\n");
		return -ENOMEM;
	}

	input_set_abs_params(data->input, ABS_MT_POSITION_X, 0,
			     data->x_max, 0, 0);
	input_set_abs_params(data->input, ABS_MT_POSITION_Y, 0,
			     data->y_max, 0, 0);

	input_mt_init_slots(data->input, data->max_fingers,
			    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED |
			    INPUT_MT_TRACK);

	data->input->name = SILEAD_TS_NAME;
	data->input->phys = "input/ts";
	data->input->id.bustype = BUS_I2C;

	error = input_register_device(data->input);
	if (error) {
		dev_err(dev, "Failed to register input device: %d\n", error);
		return error;
	}

	return 0;
}

static void silead_ts_report_touch(struct silead_ts_data *data, u16 x, u16 y,
				   u8 id)
{
	if (data->x_invert)
		x = data->x_max - x;

	if (data->y_invert)
		y = data->y_max - y;

	if (data->xy_swap)
		swap(x, y);

	input_mt_slot(data->input, id);
	input_mt_report_slot_state(data->input, MT_TOOL_FINGER, true);
	input_report_abs(data->input, ABS_MT_POSITION_X, x);
	input_report_abs(data->input, ABS_MT_POSITION_Y, y);
}

static void silead_ts_set_power(struct i2c_client *client,
				enum silead_ts_power state)
{
	struct silead_ts_data *data = i2c_get_clientdata(client);

	if (data->gpio_power) {
		gpiod_set_value_cansleep(data->gpio_power, state);
		msleep(SILEAD_POWER_SLEEP);
	}
}

static void silead_ts_read_data(struct i2c_client *client)
{
	struct silead_ts_data *data = i2c_get_clientdata(client);
	struct device *dev = &client->dev;
	u8 buf[SILEAD_TS_DATA_LEN];
	int x, y, id, touch_nr, error, i, offset, index;

	error = i2c_smbus_read_i2c_block_data(client, SILEAD_REG_DATA,
					    SILEAD_TS_DATA_LEN, buf);
	if (error < 0) {
		dev_err(dev, "Data read error %d\n", error);
		return;
	}

	touch_nr = buf[0];

	if (touch_nr < 0)
		return;

	dev_dbg(dev, "Touch number: %d\n", touch_nr);

	for (i = 1; i <= touch_nr; i++) {
		offset = i * SILEAD_POINT_DATA_LEN;

		/* Bits 4-7 are the touch id */
		id = (buf[offset + SILEAD_POINT_X_MSB_OFF] &
		      SILEAD_TOUCH_ID_MASK) >> 4;

		/* Bits 0-3 are MSB of X */
		buf[offset + SILEAD_POINT_X_MSB_OFF] =
					buf[offset + SILEAD_POINT_X_MSB_OFF] &
					SILEAD_POINT_HSB_MASK;

		/* Bits 0-3 are MSB of Y */
		buf[offset + SILEAD_POINT_Y_MSB_OFF] =
					buf[offset + SILEAD_POINT_Y_MSB_OFF] &
					SILEAD_POINT_HSB_MASK;

		y = le16_to_cpup((__le16 *)(buf + offset + SILEAD_POINT_Y_OFF));
		x = le16_to_cpup((__le16 *)(buf + offset + SILEAD_POINT_X_OFF));

		index = i - 1;
		data->pos[index].x = x;
		data->pos[index].y = y;

		input_mt_assign_slots(data->input, data->slots, data->pos,
				      index, 0);
		silead_ts_report_touch(data, x, y, data->slots[index]);

		dev_dbg(dev, "x=%d y=%d hw_id=%d sw_id=%d\n", x, y, id,
			data->slots[index]);
	}

	input_sync(data->input);
}

static int silead_ts_init(struct i2c_client *client)
{
	struct silead_ts_data *data = i2c_get_clientdata(client);
	int error;

	error = i2c_smbus_write_byte_data(client, SILEAD_REG_RESET,
					SILEAD_CMD_RESET);
	if (error)
		goto i2c_write_err;
	usleep_range(SILEAD_CMD_SLEEP_MIN, SILEAD_CMD_SLEEP_MAX);

	error = i2c_smbus_write_byte_data(client, SILEAD_REG_TOUCH_NR,
					data->max_fingers);
	if (error)
		goto i2c_write_err;
	usleep_range(SILEAD_CMD_SLEEP_MIN, SILEAD_CMD_SLEEP_MAX);

	error = i2c_smbus_write_byte_data(client, SILEAD_REG_CLOCK,
					  SILEAD_CLOCK);
	if (error)
		goto i2c_write_err;
	usleep_range(SILEAD_CMD_SLEEP_MIN, SILEAD_CMD_SLEEP_MAX);

	error = i2c_smbus_write_byte_data(client, SILEAD_REG_RESET,
					SILEAD_CMD_START);
	if (error)
		goto i2c_write_err;
	usleep_range(SILEAD_CMD_SLEEP_MIN, SILEAD_CMD_SLEEP_MAX);

	return 0;

i2c_write_err:
	dev_err(&client->dev, "Registers clear error %d\n", error);
	return error;
}

static int silead_ts_reset(struct i2c_client *client)
{
	int error;

	error = i2c_smbus_write_byte_data(client, SILEAD_REG_RESET,
					SILEAD_CMD_RESET);
	if (error)
		goto i2c_write_err;
	usleep_range(SILEAD_CMD_SLEEP_MIN, SILEAD_CMD_SLEEP_MAX);

	error = i2c_smbus_write_byte_data(client, SILEAD_REG_CLOCK,
					  SILEAD_CLOCK);
	if (error)
		goto i2c_write_err;
	usleep_range(SILEAD_CMD_SLEEP_MIN, SILEAD_CMD_SLEEP_MAX);

	error = i2c_smbus_write_byte_data(client, SILEAD_REG_POWER,
					SILEAD_CMD_START);
	if (error)
		goto i2c_write_err;
	usleep_range(SILEAD_CMD_SLEEP_MIN, SILEAD_CMD_SLEEP_MAX);

	return 0;

i2c_write_err:
	dev_err(&client->dev, "Chip reset error %d\n", error);
	return error;
}

static int silead_ts_startup(struct i2c_client *client)
{
	int error;

	error = i2c_smbus_write_byte_data(client, SILEAD_REG_RESET, 0x00);
	if (error) {
		dev_err(&client->dev, "Startup error %d\n", error);
		return error;
	}
	msleep(SILEAD_STARTUP_SLEEP);

	return 0;
}

static int silead_ts_load_fw(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct silead_ts_data *data = i2c_get_clientdata(client);
	unsigned int fw_size, i;
	const struct firmware *fw;
	struct silead_fw_data *fw_data;
	int error;

	dev_dbg(dev, "Firmware file name: %s", data->fw_name);

	if (data->custom_fw_name)
		error = request_firmware(&fw, data->custom_fw_name, dev);
	else
		error = request_firmware(&fw, data->fw_name, dev);

	if (error) {
		dev_err(dev, "Firmware request error %d\n", error);
		return error;
	}

	fw_size = fw->size / sizeof(*fw_data);
	fw_data = (struct silead_fw_data *)fw->data;

	for (i = 0; i < fw_size; i++) {
		error = i2c_smbus_write_i2c_block_data(client,
						       fw_data[i].offset,
						       4,
						       (u8 *)&fw_data[i].val);
		if (error) {
			dev_err(dev, "Firmware load error %d\n", error);
			goto release_fw_err;
		}
	}

	release_firmware(fw);
	return 0;

release_fw_err:
	release_firmware(fw);
	return error;
}

static u32 silead_ts_get_status(struct i2c_client *client)
{
	int error;
	u32 status;

	error = i2c_smbus_read_i2c_block_data(client, SILEAD_REG_STATUS, 4,
					    (u8 *)&status);
	if (error < 0) {
		dev_err(&client->dev, "Status read error %d\n", error);
		return error;
	}

	return le32_to_cpu(status);
}

static int silead_ts_get_id(struct i2c_client *client)
{
	struct silead_ts_data *data = i2c_get_clientdata(client);
	int error;

	error = i2c_smbus_read_i2c_block_data(client, SILEAD_REG_ID, 4,
					    (u8 *)&data->chip_id);

	data->chip_id = le32_to_cpu(data->chip_id);

	if (error < 0) {
		dev_err(&client->dev, "Chip ID read error %d\n", error);
		return error;
	}

	return 0;
}

static int silead_ts_setup(struct i2c_client *client)
{
	struct silead_ts_data *data = i2c_get_clientdata(client);
	struct device *dev = &client->dev;
	int error;
	u32 status;

	silead_ts_set_power(client, SILEAD_POWER_OFF);
	silead_ts_set_power(client, SILEAD_POWER_ON);

	error = silead_ts_get_id(client);
	if (error)
		return error;
	dev_dbg(dev, "Chip ID: 0x%8X", data->chip_id);

	error = silead_ts_init(client);
	if (error)
		return error;

	error = silead_ts_reset(client);
	if (error)
		return error;

	error = silead_ts_load_fw(client);
	if (error)
		return error;

	error = silead_ts_startup(client);
	if (error)
		return error;

	status = silead_ts_get_status(client);
	if (status != SILEAD_STATUS_OK) {
		dev_err(dev, "Initialization error, status: 0x%X\n", status);
		return -ENODEV;
	}

	return 0;
}

static irqreturn_t silead_ts_threaded_irq_handler(int irq, void *id)
{
	struct silead_ts_data *data = id;
	struct i2c_client *client = data->client;

	silead_ts_read_data(client);

	return IRQ_HANDLED;
}

static int silead_ts_read_props(struct i2c_client *client)
{
	struct silead_ts_data *data = i2c_get_clientdata(client);
	struct device *dev = &client->dev;
	int error;

	error = device_property_read_u16(dev, SILEAD_DP_X_MAX, &data->x_max);
	if (error) {
		dev_dbg(dev, "Resolution X read error %d\n", error);
		data->x_max = SILEAD_MAX_X;
	}

	error = device_property_read_u16(dev, SILEAD_DP_Y_MAX, &data->y_max);
	if (error) {
		dev_dbg(dev, "Resolution Y read error %d\n", error);
		data->y_max = SILEAD_MAX_Y;
	}

	error = device_property_read_u8(dev, SILEAD_DP_MAX_FINGERS,
				      &data->max_fingers);
	if (error) {
		dev_dbg(dev, "Max fingers read error %d\n", error);
		data->max_fingers = SILEAD_MAX_FINGERS;
	}

	error = device_property_read_string(dev, SILEAD_DP_FW_NAME,
					  &data->custom_fw_name);
	if (error)
		dev_dbg(dev, "Firmware file name read error. Using default.");

	data->x_invert = device_property_read_bool(dev, SILEAD_DP_X_INVERT);
	data->y_invert = device_property_read_bool(dev, SILEAD_DP_Y_INVERT);
	data->xy_swap = device_property_read_bool(dev, SILEAD_DP_XY_SWAP);

	dev_dbg(dev, "x_max = %d, y_max = %d, max_fingers = %d, x_invert = %d, y_invert = %d, xy_swap = %d",
		data->x_max, data->y_max, data->max_fingers, data->x_invert,
		data->y_invert, data->xy_swap);

	return 0;
}

#ifdef CONFIG_ACPI
static int silead_ts_set_default_fw_name(struct silead_ts_data *data,
					 const struct i2c_device_id *id)
{
	const struct acpi_device_id *acpi_id;
	struct device *dev = &data->client->dev;
	int i;

	if (ACPI_HANDLE(dev)) {
		acpi_id = acpi_match_device(dev->driver->acpi_match_table, dev);
		if (!acpi_id)
			return -ENODEV;

		snprintf(data->fw_name, sizeof(data->fw_name), "%s.fw", acpi_id->id);

		for (i = 0; i < strlen(data->fw_name); i++)
			data->fw_name[i] = tolower(data->fw_name[i]);
	} else {
		snprintf(data->fw_name, sizeof(data->fw_name), "%s.fw", id->name);
	}

	return 0;
}
#else
static int silead_ts_set_default_fw_name(struct silead_ts_data *data,
					 const struct i2c_device_id *id)
{
	snprintf(data->fw_name, sizeof(data->fw_name), "%s.fw", id->name);
	return 0;
}
#endif

static int silead_ts_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct silead_ts_data *data;
	struct device *dev = &client->dev;
	int error;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_I2C |
				     I2C_FUNC_SMBUS_READ_I2C_BLOCK |
				     I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)) {
		dev_err(dev, "I2C functionality check failed\n");
		return -ENXIO;
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	i2c_set_clientdata(client, data);
	data->client = client;

	error = silead_ts_set_default_fw_name(data, id);
	if (error)
		return error;

	/* If the IRQ is not filled by DT or ACPI subsytem
	 * we can't continue without it */
	if (client->irq <= 0)
		return -ENODEV;

	/* Power GPIO pin */
	data->gpio_power = devm_gpiod_get_index(dev, SILEAD_PWR_GPIO_NAME,
						0, GPIOD_OUT_LOW);
	if (IS_ERR(data->gpio_power)) {
		dev_dbg(dev, "Shutdown GPIO request failed\n");
		data->gpio_power = NULL;
	}

	error = silead_ts_read_props(client);
	if (error)
		return error;

	error = silead_ts_setup(client);
	if (error)
		return error;

	error = silead_ts_request_input_dev(data);
	if (error)
		return error;

	error = devm_request_threaded_irq(dev, client->irq, NULL,
					silead_ts_threaded_irq_handler,
					IRQF_ONESHOT | IRQ_TYPE_EDGE_RISING,
					client->name, data);
	if (error) {
		dev_err(dev, "IRQ request failed %d\n", error);
		return error;
	}

	dev_dbg(dev, "Probing succeded\n");
	return 0;
}

static int __maybe_unused silead_ts_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	silead_ts_set_power(client, SILEAD_POWER_OFF);
	return 0;
}

static int __maybe_unused silead_ts_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	int error, status;

	silead_ts_set_power(client, SILEAD_POWER_ON);

	error = silead_ts_reset(client);
	if (error)
		return error;

	error = silead_ts_startup(client);
	if (error)
		return error;

	status = silead_ts_get_status(client);
	if (status != SILEAD_STATUS_OK) {
		dev_err(dev, "Resume error, status: 0x%X\n", status);
		return -ENODEV;
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(silead_ts_pm, silead_ts_suspend, silead_ts_resume);

static const struct i2c_device_id silead_ts_id[] = {
	{ "gsl1680", 0 },
	{ "gsl1688", 0 },
	{ "gsl3670", 0 },
	{ "gsl3675", 0 },
	{ "gsl3692", 0 },
	{ "mssl1680", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, silead_ts_id);

#ifdef CONFIG_ACPI
static const struct acpi_device_id silead_ts_acpi_match[] = {
	{ "GSL1680", 0 },
	{ "GSL1688", 0 },
	{ "GSL3670", 0 },
	{ "GSL3675", 0 },
	{ "GSL3692", 0 },
	{ "MSSL1680", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, silead_ts_acpi_match);
#endif

static struct i2c_driver silead_ts_driver = {
	.probe = silead_ts_probe,
	.id_table = silead_ts_id,
	.driver = {
		.name = SILEAD_TS_NAME,
		.owner = THIS_MODULE,
		.acpi_match_table = ACPI_PTR(silead_ts_acpi_match),
		.pm = &silead_ts_pm,
	},
};
module_i2c_driver(silead_ts_driver);

MODULE_AUTHOR("Robert Dolca <robert.dolca@intel.com>");
MODULE_DESCRIPTION("Silead I2C touchscreen driver");
MODULE_LICENSE("GPL");
