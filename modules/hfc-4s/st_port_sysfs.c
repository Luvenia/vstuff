/*
 * Cologne Chip's HFC-4S and HFC-8S vISDN driver
 *
 * Copyright (C) 2004-2005 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#include <linux/kernel.h>

#include <kernel_config.h>

#include "st_port.h"
#include "st_port_inline.h"
#include "st_port_sysfs.h"
#include "card.h"

static ssize_t hfc_show_role(
	struct visdn_port *visdn_port,
	struct visdn_port_attribute *attr,
	char *buf)
{
	struct hfc_st_port *port = to_st_port(visdn_port);

	return snprintf(buf, PAGE_SIZE, "%s\n",
		port->nt_mode? "NT" : "TE");
}

static ssize_t hfc_store_role(
	struct visdn_port *visdn_port,
	struct visdn_port_attribute *attr,
	const char *buf,
	size_t count)
{
	struct hfc_st_port *port = to_st_port(visdn_port);
	struct hfc_card *card = port->card;

	if (count < 2)
		return count;

	hfc_card_lock(card);

	hfc_st_port_select(port);

	if (!strncmp(buf, "NT", 2) && !port->nt_mode) {
		port->nt_mode = TRUE;
		port->clock_delay = HFC_DEF_NT_CLK_DLY;
		port->sampling_comp = HFC_DEF_NT_SAMPL_COMP;
	} else if (!strncmp(buf, "TE", 2) && port->nt_mode) {
		port->nt_mode = FALSE;
		port->clock_delay = HFC_DEF_TE_CLK_DLY;
		port->sampling_comp = HFC_DEF_TE_SAMPL_COMP;
	}

	hfc_st_port_update_st_ctrl_0(port);
	hfc_st_port_update_st_clk_dly(port);

	hfc_card_unlock(card);

	return count;
}

static VISDN_PORT_ATTR(role, S_IRUGO | S_IWUSR,
		hfc_show_role,
		hfc_store_role);

//----------------------------------------------------------------------------

static ssize_t hfc_show_l1_state(
	struct visdn_port *visdn_port,
	struct visdn_port_attribute *attr,
	char *buf)
{
	struct hfc_st_port *port = to_st_port(visdn_port);

	return snprintf(buf, PAGE_SIZE, "%c%d\n",
		port->nt_mode?'G':'F',
		port->l1_state);
}

static ssize_t hfc_store_l1_state(
	struct visdn_port *visdn_port,
	struct visdn_port_attribute *attr,
	const char *buf,
	size_t count)
{
	struct hfc_st_port *port = to_st_port(visdn_port);
	struct hfc_card *card = port->card;
	int err;

	hfc_card_lock(card);

	hfc_st_port_select(port);

	if (count >= 8 && !strncmp(buf, "activate", 8)) {
		hfc_outb(card, hfc_A_ST_WR_STA,
			hfc_A_ST_WR_STA_V_ST_ACT_ACTIVATION|
			hfc_A_ST_WR_STA_V_SET_G2_G3);
	} else if (count >= 10 && !strncmp(buf, "deactivate", 10)) {
		hfc_outb(card, hfc_A_ST_WR_STA,
			hfc_A_ST_WR_STA_V_ST_ACT_DEACTIVATION|
			hfc_A_ST_WR_STA_V_SET_G2_G3);
	} else {
		int state;
		if (sscanf(buf, "%d", &state) < 1) {
			err = -EINVAL;
			goto err_invalid_scanf;
		}

		if (state < 0 ||
		    (port->nt_mode && state > 7) ||
		    (!port->nt_mode && state > 3)) {
			err = -EINVAL;
			goto err_invalid_state;
		}

		hfc_outb(card, hfc_A_ST_WR_STA,
			hfc_A_ST_WR_STA_V_ST_SET_STA(state));
	}

	hfc_card_unlock(card);

	return count;

err_invalid_scanf:
err_invalid_state:

	hfc_card_unlock(card);

	return err;
}

static VISDN_PORT_ATTR(l1_state, S_IRUGO | S_IWUSR,
		hfc_show_l1_state,
		hfc_store_l1_state);

//----------------------------------------------------------------------------

static ssize_t hfc_show_st_clock_delay(
	struct visdn_port *visdn_port,
	struct visdn_port_attribute *attr,
	char *buf)
{
	struct hfc_st_port *port = to_st_port(visdn_port);

	return snprintf(buf, PAGE_SIZE, "%02x\n", port->clock_delay);
}

static ssize_t hfc_store_st_clock_delay(
	struct visdn_port *visdn_port,
	struct visdn_port_attribute *attr,
	const char *buf,
	size_t count)
{
	struct hfc_st_port *port = to_st_port(visdn_port);
	struct hfc_card *card = port->card;

	unsigned int value;
	if (sscanf(buf, "%02x", &value) < 1)
		return -EINVAL;

	if (value > 0x0f)
		return -EINVAL;

	hfc_card_lock(card);
	port->clock_delay = value;
	hfc_st_port_update_st_clk_dly(port);
	hfc_card_unlock(card);

	return count;
}

static VISDN_PORT_ATTR(st_clock_delay, S_IRUGO | S_IWUSR,
		hfc_show_st_clock_delay,
		hfc_store_st_clock_delay);

//----------------------------------------------------------------------------
static ssize_t hfc_show_st_sampling_comp(
	struct visdn_port *visdn_port,
	struct visdn_port_attribute *attr,
	char *buf)
{
	struct hfc_st_port *port = to_st_port(visdn_port);

	return snprintf(buf, PAGE_SIZE, "%02x\n", port->sampling_comp);
}

static ssize_t hfc_store_st_sampling_comp(
	struct visdn_port *visdn_port,
	struct visdn_port_attribute *attr,
	const char *buf,
	size_t count)
{
	struct hfc_st_port *port = to_st_port(visdn_port);
	struct hfc_card *card = port->card;

	unsigned int value;
	if (sscanf(buf, "%u", &value) < 1)
		return -EINVAL;

	if (value > 0x7)
		return -EINVAL;

	hfc_card_lock(card);
	port->sampling_comp = value;
	hfc_st_port_update_st_clk_dly(port);
	hfc_card_unlock(card);

	return count;
}

static VISDN_PORT_ATTR(st_sampling_comp, S_IRUGO | S_IWUSR,
		hfc_show_st_sampling_comp,
		hfc_store_st_sampling_comp);

int hfc_st_port_sysfs_create_files(
        struct hfc_st_port *port)
{
	int err;

	err = visdn_port_create_file(
		&port->visdn_port,
		&visdn_port_attr_role);
	if (err < 0)
		goto err_create_file_role;

	err = visdn_port_create_file(
		&port->visdn_port,
		&visdn_port_attr_l1_state);
	if (err < 0)
		goto err_create_file_l1_state;

	err = visdn_port_create_file(
		&port->visdn_port,
		&visdn_port_attr_st_clock_delay);
	if (err < 0)
		goto err_create_file_st_clock_delay;

	err = visdn_port_create_file(
		&port->visdn_port,
		&visdn_port_attr_st_sampling_comp);
	if (err < 0)
		goto err_create_file_st_sampling_comp;

	return 0;

	visdn_port_remove_file(
		&port->visdn_port,
		&visdn_port_attr_role);
err_create_file_role:
	visdn_port_remove_file(
		&port->visdn_port,
		&visdn_port_attr_st_sampling_comp);
err_create_file_st_sampling_comp:
	visdn_port_remove_file(
		&port->visdn_port,
		&visdn_port_attr_st_clock_delay);
err_create_file_st_clock_delay:
	visdn_port_remove_file(
		&port->visdn_port,
		&visdn_port_attr_l1_state);
err_create_file_l1_state:

	return err;
}

void hfc_st_port_sysfs_delete_files(
        struct hfc_st_port *port)
{
	visdn_port_remove_file(
		&port->visdn_port,
		&visdn_port_attr_role);
	visdn_port_remove_file(
		&port->visdn_port,
		&visdn_port_attr_st_sampling_comp);
	visdn_port_remove_file(
		&port->visdn_port,
		&visdn_port_attr_st_clock_delay);
	visdn_port_remove_file(
		&port->visdn_port,
		&visdn_port_attr_l1_state);
}