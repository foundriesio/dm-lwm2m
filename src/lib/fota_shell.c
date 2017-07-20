/*
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <init.h>
#include <misc/printk.h>
#include <shell/shell.h>
#include <zephyr.h>

#include "mcuboot.h"

#define THIS_SHELL_MODULE "fota"

#define FOTA_GET_HELP							\
	("<object> [arguments]:\n"					\
	 "Retrieve local state or fetch binaries from the network.\n"	\
	 "\n"								\
	 "Available objects:\n"						\
	 "acid - Get current and update hawkBit ACID values.\n")

static int fota_cmd_get_acid(int argc, char *argv[])
{
	struct hawkbit_dev_storage_acid acid;

	if (argc != 1) {
		printk("\"get acid\" takes no arguments.\n");
		return -1;
	}

	ARG_UNUSED(argv);

	hawkbit_device_acid_read(&acid);
	printk("ACID: current=%d, update=%d\n", acid.current, acid.update);
	return 0;
}

static int fota_cmd_get(int argc, char *argv[])
{
	/*
	 * Normalize to "get <what>".
	 *
	 * When run as "get <what>" after "select THIS_SHELL_MODULE",
	 * the shell module name is not part of argv. When run as
	 * "fota get <what>" from the base shell, it is.
	 */
	if (!strcmp(argv[0], "fota")) {
		argc--;
		argv++;
	}

	if (argc < 2) {
		return -1;
	}

	/* Skip the "get". */
	argc--;
	argv++;

	if (!strcmp(argv[0], "acid")) {
		return fota_cmd_get_acid(argc, argv);
	}

	printk("Unknown object %s\n", argv[0]);
	return -1;
}

static struct shell_cmd commands[] = {
	{ "get", fota_cmd_get, FOTA_GET_HELP },
	{ NULL, NULL, NULL}
};

static int fota_shell_init(struct device *dev)
{
	ARG_UNUSED(dev);
	SHELL_REGISTER(THIS_SHELL_MODULE, commands);
	return 0;
}

SYS_INIT(fota_shell_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
