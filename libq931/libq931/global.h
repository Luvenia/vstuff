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

#ifndef _LIBQ931_GLOBAL_H
#define _LIBQ931_GLOBAL_H

#include <libq931/intf.h>
#include <libq931/chanset.h>

enum q931_global_state
{
	Q931_GLOBAL_STATE_NULL,
	Q931_GLOBAL_STATE_RESTART_REQUEST,
	Q931_GLOBAL_STATE_RESTART
};

struct q931_interface;
struct q931_global_call
{
	struct q931_interface *intf;

	enum q931_global_state state;

	struct q931_timer T316;
	struct q931_timer T317;
	int T317_expired;

	int restart_retransmit_count;
	int restart_responded;
	int restart_acknowledged;
	struct q931_ie_restart_indicator *restart_indicator;

	struct q931_chanset restart_reqd_chans;
	struct q931_chanset restart_acked_chans;

	struct q931_dlc *restart_dlc;
};


#ifdef Q931_PRIVATE

#define report_gc(cr, lvl, format, arg...)		\
	q931_report((lvl), "Global Call: " format,	\
			## arg)

#define q931_global_start_timer(cr, timer)		\
	do {						\
		q931_start_timer_delta(			\
			&(cr)->timer,			\
			(cr)->intf->timer);		\
		report_intf((cr)->intf, LOG_DEBUG,	\
			"%s:%d Timer %s started\n",	\
			__FILE__,__LINE__,		\
			#timer);			\
	} while(0)

#define q931_global_stop_timer(cr, timer)		\
	do {						\
		q931_stop_timer(&(cr)->timer);		\
		report_intf((cr)->intf, LOG_DEBUG,	\
			"%s:%d Timer %s stopped\n",	\
			__FILE__,__LINE__,		\
			#timer);			\
	} while(0)

#define q931_global_timer_running(cr, timer)		\
		q931_timer_pending(&(cr)->timer)


void q931_dispatch_global_message(
	struct q931_global_call *gc,
	struct q931_message *msg);

void q931_global_restart_confirm(
	struct q931_global_call *gc,
	struct q931_dlc *dlc,
	struct q931_channel *chan);

void q931_management_restart_request(
	struct q931_global_call *gc,
	struct q931_dlc *dlc,
	struct q931_chanset *chanset,
	const struct q931_ies *user_ies);

void q931_global_init(
	struct q931_global_call *gc,
	struct q931_interface *intf);
#endif
#endif
