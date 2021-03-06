/*
 * vISDN DSSS-1/q.931 signalling library
 *
 * Copyright (C) 2004-2005 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <stdio.h>

#define Q931_PRIVATE

#include <libq931/lib.h>
#include <libq931/ie_call_identity.h>

static const struct q931_ie_class *my_class;

void q931_ie_call_identity_register(
	const struct q931_ie_class *ie_class)
{
	my_class = ie_class;
}

struct q931_ie_call_identity *q931_ie_call_identity_alloc(void)
{
	struct q931_ie_call_identity *ie;
	ie = malloc(sizeof(*ie));
	assert(ie);

	memset(ie, 0x00, sizeof(*ie));

	ie->ie.refcnt = 1;
	ie->ie.cls = my_class;

	return ie;
}

struct q931_ie *q931_ie_call_identity_alloc_abstract(void)
{
	return &q931_ie_call_identity_alloc()->ie;
}

int q931_ie_call_identity_read_from_buf(
	struct q931_ie *abstract_ie,
	void *buf,
	int len,
	void (*report_func)(int level, const char *format, ...),
	struct q931_interface *intf)
{
	assert(abstract_ie->cls == my_class);

	struct q931_ie_call_identity *ie =
		container_of(abstract_ie,
			struct q931_ie_call_identity, ie);

	if (len < 1) {
		report_ie(abstract_ie, LOG_ERR, "IE len < 1\n");
		return FALSE;
	}

	if (len > 10) {
		// Be reeeeally sure the IE is not > 10 octets
		report_ie(abstract_ie, LOG_ERR, "IE len > 10\n");
		return FALSE;
	}

	memcpy(ie->data, buf, len);
	ie->data_len = len;

	return TRUE;
}

int q931_ie_call_identity_write_to_buf(
	const struct q931_ie *abstract_ie,
	void *buf,
	int max_size)
{
	int len = 0;
	struct q931_ie_call_identity *ie =
		container_of(abstract_ie, struct q931_ie_call_identity, ie);

	memcpy(buf + len, ie->data, ie->data_len);
	len += ie->data_len;

	return len;
}

void q931_ie_call_identity_dump(
	const struct q931_ie *abstract_ie,
	void (*report_func)(int level, const char *format, ...),
	const char *prefix)
{
	struct q931_ie_call_identity *ie =
		container_of(abstract_ie, struct q931_ie_call_identity, ie);

	char sane_str[10];
	char hex_str[20];
	int i;
	for(i=0; i<sizeof(sane_str) && i<ie->data_len; i++) {
		sane_str[i] = isprint(ie->data[i]) ? ie->data[i] : '.';
		snprintf(hex_str + (i*2), sizeof(hex_str)-(i*2),
				"%02x ", ie->data[i]);
	}
	sane_str[i] = '\0';
	hex_str[i*2] = '\0';

	report_ie_dump(abstract_ie, "%sIdentity = %s (%s)\n", prefix,
		sane_str, hex_str);
}
