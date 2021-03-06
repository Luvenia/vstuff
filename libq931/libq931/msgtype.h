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

#ifndef _LIBQ931_MSGTYPE_H
#define _LIBQ931_MSGTYPE_H

enum q931_message_type
{
	Q931_MT_ALERTING		= 0x01,
	Q931_MT_CALL_PROCEEDING		= 0x02,
	Q931_MT_CONNECT			= 0x07,
	Q931_MT_CONNECT_ACKNOWLEDGE	= 0x0f,
	Q931_MT_PROGRESS		= 0x03,
	Q931_MT_SETUP			= 0x05,
	Q931_MT_SETUP_ACKNOWLEDGE	= 0x0d,

/* Call Disestablishment Messages */
	Q931_MT_DISCONNECT		= 0x45,
	Q931_MT_RELEASE			= 0x4d,
	Q931_MT_RELEASE_COMPLETE	= 0x5a,
	Q931_MT_RESTART			= 0x46,
	Q931_MT_RESTART_ACKNOWLEDGE	= 0x4e,

/* Miscellaneous Messages */
	Q931_MT_STATUS			= 0x7d,
	Q931_MT_STATUS_ENQUIRY		= 0x75,
	Q931_MT_USER_INFORMATION	= 0x20,
	Q931_MT_SEGMENT			= 0x60,
	Q931_MT_CONGESTION_CONTROL	= 0x79,
	Q931_MT_INFORMATION		= 0x7b,
	Q931_MT_FACILITY		= 0x62,
	Q931_MT_REGISTER		= 0x64,
	Q931_MT_NOTIFY			= 0x6e,

/* Call Management Messages */
	Q931_MT_HOLD			= 0x24,
	Q931_MT_HOLD_ACKNOWLEDGE	= 0x28,
	Q931_MT_HOLD_REJECT		= 0x30,
	Q931_MT_RETRIEVE		= 0x31,
	Q931_MT_RETRIEVE_ACKNOWLEDGE	= 0x33,
	Q931_MT_RETRIEVE_REJECT		= 0x37,
	Q931_MT_RESUME			= 0x26,
	Q931_MT_RESUME_ACKNOWLEDGE	= 0x2e,
	Q931_MT_RESUME_REJECT		= 0x22,
	Q931_MT_SUSPEND			= 0x25,
	Q931_MT_SUSPEND_ACKNOWLEDGE	= 0x2d,
	Q931_MT_SUSPEND_REJECT		= 0x21,
};

struct q931_message_type_name
{
	enum q931_message_type id;
	char *name;
};

void q931_message_types_init();
const char *q931_message_type_to_text(int id);

#endif
