/*
 * Copyright (c) 2019 Atmark Techno, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <init.h>
#include <drivers/gpio.h>

static int board_degu_evk_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	const struct device *gpio1 =
		device_get_binding(DT_LABEL(DT_NODELABEL(gpio1)));

	/*
	 * We must enable GPIO1_6 to read Vin voltage.
	 */
	gpio_pin_configure(gpio1, 6, GPIO_OUTPUT_HIGH);

	return 0;
}

SYS_INIT(board_degu_evk_init, PRE_KERNEL_1,
	 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
