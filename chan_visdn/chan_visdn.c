/*
 * vISDN channel driver for Asterisk
 *
 * Copyright (C) 2004-2005 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <signal.h>
#include <ctype.h>

#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <net/if_arp.h>

#include <linux/rtc.h>

#include <asterisk/lock.h>
#include <asterisk/channel.h>
#include <asterisk/config.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/pbx.h>
#include <asterisk/options.h>
#include <asterisk/utils.h>
#include <asterisk/callerid.h>
#include <asterisk/indications.h>
#include <asterisk/cli.h>
#include <asterisk/musiconhold.h>

#include "../config.h"

#ifdef HAVE_ASTERISK_VERSION_H
#include <asterisk/version.h>
#endif

#ifndef ASTERISK_VERSION_NUM
#include <asterisk/channel_pvt.h>
#define _bridge bridge
#define caller_id_number callerid
#define AST_BRIDGE_COMPLETE 0
#define AST_BRIDGE_FAILED -1
#define AST_BRIDGE_FAILED_NOWARN -2
#else
#define caller_id_number cid.cid_num
#endif

#include <linux/lapd.h>
#include <linux/visdn/netdev.h>
#include <linux/visdn/streamport.h>
#include <linux/visdn/cxc.h>

#include <libq931/q931.h>

#include "chan_visdn.h"

#include "../config.h"

#define VISDN_DESCRIPTION "VISDN Channel For Asterisk"
#define VISDN_CHAN_TYPE "VISDN"
#define VISDN_CONFIG_FILE "visdn.conf"


#define assert(cond)							\
	do {								\
		if (!(cond)) {						\
			ast_log(LOG_ERROR,				\
				"assertion (" #cond ") failed\n");	\
			abort();					\
		}							\
	} while(0)

AST_MUTEX_DEFINE_STATIC(usecnt_lock);

static pthread_t visdn_q931_thread = AST_PTHREADT_NULL;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

enum poll_info_type
{
	POLL_INFO_TYPE_INTERFACE,
	POLL_INFO_TYPE_DLC,
	POLL_INFO_TYPE_NETLINK,
	POLL_INFO_TYPE_CCB_Q931,
	POLL_INFO_TYPE_Q931_CCB,
};

struct poll_info
{
	enum poll_info_type type;
	union
	{
		struct q931_interface *interface;
		struct q931_dlc *dlc;
	};
};

enum visdn_type_of_number
{
	VISDN_TYPE_OF_NUMBER_UNKNOWN,
	VISDN_TYPE_OF_NUMBER_INTERNATIONAL,
	VISDN_TYPE_OF_NUMBER_NATIONAL,
	VISDN_TYPE_OF_NUMBER_NETWORK_SPECIFIC,
	VISDN_TYPE_OF_NUMBER_SUBSCRIBER,
	VISDN_TYPE_OF_NUMBER_ABBREVIATED,
};

struct visdn_interface
{
	struct list_head ifs_node;

	char name[IFNAMSIZ];

	int configured;
	int open_pending;

	enum q931_interface_network_role network_role;
	enum visdn_type_of_number type_of_number;
	enum visdn_type_of_number local_type_of_number;
	int tones_option;
	char context[AST_MAX_EXTENSION];
	char default_inbound_caller_id[128];
	int force_inbound_caller_id;
	int overlap_sending;
	int overlap_receiving;
	char national_prefix[10];
	char international_prefix[10];
	int dlc_autorelease_time;

	int T301;
	int T302;
	int T303;
	int T304;
	int T305;
	int T306;
	int T307;
	int T308;
	int T309;
	int T310;
	int T312;
	int T313;
	int T314;
	int T316;
	int T317;
	int T318;
	int T319;
	int T320;
	int T321;
	int T322;

	struct list_head suspended_calls;

	struct q931_interface *q931_intf;

	char remote_port[PATH_MAX];
};

struct visdn_state
{
	ast_mutex_t lock;

	struct q931_lib *libq931;

	int have_to_exit;

	struct list_head ccb_q931_queue;
	ast_mutex_t ccb_q931_queue_lock;
	int ccb_q931_queue_pipe_read;
	int ccb_q931_queue_pipe_write;

	struct list_head q931_ccb_queue;
	ast_mutex_t q931_ccb_queue_lock;
	int q931_ccb_queue_pipe_read;
	int q931_ccb_queue_pipe_write;

	struct list_head ifs;

	struct pollfd polls[100];
	struct poll_info poll_infos[100];
	int npolls;

	int open_pending;
	int open_pending_nextcheck;

	int usecnt;
	int netlink_socket;

	int debug;
	int debug_q931;
	int debug_q921;

	struct visdn_interface default_intf;
} visdn = {
	.usecnt = 0,
#ifdef DEBUG_DEFAULTS
	.debug = TRUE,
	.debug_q921 = FALSE,
	.debug_q931 = TRUE,
#else
	.debug = FALSE,
	.debug_q921 = FALSE,
	.debug_q931 = FALSE,
#endif

	.default_intf = {
		.network_role = Q931_INTF_NET_PRIVATE,
		.type_of_number = VISDN_TYPE_OF_NUMBER_UNKNOWN,
		.local_type_of_number = VISDN_TYPE_OF_NUMBER_UNKNOWN,
		.tones_option = TRUE,
		.context = "visdn",
		.default_inbound_caller_id = "",
		.force_inbound_caller_id = FALSE,
		.overlap_sending = TRUE,
		.overlap_receiving = FALSE,
		.national_prefix = "0",
		.international_prefix = "00",
		.dlc_autorelease_time = 10,
		.T307 = 180,
	}
};

#ifdef DEBUG_CODE
#define visdn_debug(format, arg...)			\
	if (visdn.debug)				\
		ast_log(LOG_NOTICE,			\
			format,				\
			## arg)
#else
#define visdn_debug(format, arg...)		\
	do {} while(0);
#endif

static void visdn_set_socket_debug(int on)
{
	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {
		if (!intf->q931_intf)
			continue;

		if (intf->q931_intf->role == LAPD_ROLE_NT) {
			setsockopt(intf->q931_intf->master_socket,
				SOL_SOCKET, SO_DEBUG,
				&on, sizeof(on));
		} else {
			setsockopt(intf->q931_intf->dlc.socket,
				SOL_SOCKET, SO_DEBUG,
				&on, sizeof(on));
		}

		struct q931_dlc *dlc;
		list_for_each_entry(dlc, &intf->q931_intf->dlcs, intf_node) {
			setsockopt(dlc->socket,
				SOL_SOCKET, SO_DEBUG,
				&on, sizeof(on));
		}
	}

}

static int do_debug_visdn_generic(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);
	visdn.debug = TRUE;
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN debugging enabled\n");

	return 0;
}

static int do_no_debug_visdn_generic(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);
	visdn.debug = FALSE;
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN debugging disabled\n");

	return 0;
}

static int do_debug_visdn_q921(int fd, int argc, char *argv[])
{
	// Enable debugging on new DLCs FIXME TODO

	ast_mutex_lock(&visdn.lock);
	visdn.debug_q921 = TRUE;
	visdn_set_socket_debug(1);
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN q.921 debugging enabled\n");

	return 0;
}

static int do_no_debug_visdn_q921(int fd, int argc, char *argv[])
{
	// Disable debugging on new DLCs FIXME TODO

	ast_mutex_lock(&visdn.lock);
	visdn.debug_q921 = FALSE;
	visdn_set_socket_debug(0);
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN q.921 debugging disabled\n");

	return 0;
}

static int do_debug_visdn_q931(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);
	visdn.debug_q931 = TRUE;
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN q.931 debugging enabled\n");

	return 0;
}

static int do_no_debug_visdn_q931(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);
	visdn.debug_q931 = FALSE;
	ast_mutex_unlock(&visdn.lock);

	ast_cli(fd, "vISDN q.931 debugging disabled\n");

	return 0;
}

static const char *visdn_interface_network_role_to_string(
	enum q931_interface_network_role value)
{
	switch(value) {
	case Q931_INTF_NET_USER:
		return "User";
	case Q931_INTF_NET_PRIVATE:
		return "Private Network";
	case Q931_INTF_NET_LOCAL:
		return "Local Network";
	case Q931_INTF_NET_TRANSIT:
		return "Transit Network";
	case Q931_INTF_NET_INTERNATIONAL:
		return "International Network";
	}

	return "*UNKNOWN*";
}

static const char *visdn_type_of_number_to_string(
	enum visdn_type_of_number type_of_number)
{
	switch(type_of_number) {
	case VISDN_TYPE_OF_NUMBER_UNKNOWN:
		return "unknown";
	case VISDN_TYPE_OF_NUMBER_INTERNATIONAL:
		return "international";
	case VISDN_TYPE_OF_NUMBER_NATIONAL:
		return "national";
	case VISDN_TYPE_OF_NUMBER_NETWORK_SPECIFIC:
		return "network specific";
	case VISDN_TYPE_OF_NUMBER_SUBSCRIBER:
		return "subscriber";
	case VISDN_TYPE_OF_NUMBER_ABBREVIATED:
		return "private";
	}

	return "*UNKNOWN*";
}

static int do_show_visdn_interfaces(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);

	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {

		ast_cli(fd, "\n------ Interface %s ---------\n", intf->name);

		ast_cli(fd, "Role                      : %s\n",
				intf->q931_intf ?
					(intf->q931_intf->role == LAPD_ROLE_NT ?
						"NT" : "TE") :
					"UNUSED!");

		ast_cli(fd,
			"Network role              : %s\n"
			"Type of number            : %s\n"
			"Local type of number      : %s\n"
			"Tones option              : %s\n"
			"Context                   : %s\n"
			"Default inbound caller ID : %s\n"
			"Force inbound caller ID   : %s\n"
			"Overlap Sending           : %s\n"
			"Overlap Receiving         : %s\n"
			"National prefix           : %s\n"
			"International prefix      : %s\n"
			"Autorelease time          : %d\n",
			visdn_interface_network_role_to_string(
				intf->network_role),
			visdn_type_of_number_to_string(
				intf->type_of_number),
			visdn_type_of_number_to_string(
				intf->local_type_of_number),
			intf->tones_option ? "Yes" : "No",
			intf->context,
			intf->default_inbound_caller_id,
			intf->force_inbound_caller_id ? "Yes" : "No",
			intf->overlap_sending ? "Yes" : "No",
			intf->overlap_receiving ? "Yes" : "No",
			intf->national_prefix,
			intf->international_prefix,
			intf->dlc_autorelease_time);

		if (intf->q931_intf) {
			if (intf->q931_intf->role == LAPD_ROLE_NT) {
				ast_cli(fd, "DLCs                      : ");

				struct q931_dlc *dlc;
				list_for_each_entry(dlc, &intf->q931_intf->dlcs,
						intf_node) {
					ast_cli(fd, "%d ", dlc->tei);

				}

				ast_cli(fd, "\n");

#define TIMER_CONFIG(timer)					\
	ast_cli(fd, #timer ": %lld%s\n",			\
		intf->q931_intf->timer / 1000000LL,		\
		intf->timer ? " (Non-default)" : "");


				TIMER_CONFIG(T301);
				TIMER_CONFIG(T301);
				TIMER_CONFIG(T302);
				TIMER_CONFIG(T303);
				TIMER_CONFIG(T304);
				TIMER_CONFIG(T305);
				TIMER_CONFIG(T306);
				ast_cli(fd, "T307: %d\n", intf->T307);
				TIMER_CONFIG(T308);
				TIMER_CONFIG(T309);
				TIMER_CONFIG(T310);
				TIMER_CONFIG(T312);
				TIMER_CONFIG(T314);
				TIMER_CONFIG(T316);
				TIMER_CONFIG(T317);
				TIMER_CONFIG(T320);
				TIMER_CONFIG(T321);
				TIMER_CONFIG(T322);
			} else {
				TIMER_CONFIG(T301);
				TIMER_CONFIG(T302);
				TIMER_CONFIG(T303);
				TIMER_CONFIG(T304);
				TIMER_CONFIG(T305);
				TIMER_CONFIG(T306);
				ast_cli(fd, "T307: %d\n", intf->T307);
				TIMER_CONFIG(T308);
				TIMER_CONFIG(T309);
				TIMER_CONFIG(T310);
				TIMER_CONFIG(T312);
				TIMER_CONFIG(T313);
				TIMER_CONFIG(T314);
				TIMER_CONFIG(T316);
				TIMER_CONFIG(T317);
				TIMER_CONFIG(T318);
				TIMER_CONFIG(T319);
				TIMER_CONFIG(T320);
				TIMER_CONFIG(T321);
				TIMER_CONFIG(T322);
			}

		} else {
			ast_cli(fd,
				"T301: %d\n"
				"T302: %d\n"
				"T303: %d\n"
				"T304: %d\n"
				"T305: %d\n"
				"T306: %d\n"
				"T307: %d\n"
				"T308: %d\n"
				"T309: %d\n"
				"T310: %d\n"
				"T312: %d\n"
				"T313: %d\n"
				"T314: %d\n"
				"T316: %d\n"
				"T317: %d\n"
				"T318: %d\n"
				"T319: %d\n"
				"T320: %d\n"
				"T321: %d\n"
				"T322: %d\n",
				intf->T301,
				intf->T302,
				intf->T303,
				intf->T304,
				intf->T305,
				intf->T306,
				intf->T307,
				intf->T308,
				intf->T309,
				intf->T310,
				intf->T312,
				intf->T313,
				intf->T314,
				intf->T316,
				intf->T317,
				intf->T318,
				intf->T319,
				intf->T320,
				intf->T321,
				intf->T322);
		}

		ast_cli(fd, "Parked calls:\n");
		struct visdn_suspended_call *suspended_call;
		list_for_each_entry(suspended_call, &intf->suspended_calls, node) {

			char sane_str[10];
			char hex_str[20];
			int i;
			for(i=0;
			    i<sizeof(sane_str) &&
				i<suspended_call->call_identity_len;
			    i++) {
				sane_str[i] =
					 isprint(suspended_call->call_identity[i]) ?
						suspended_call->call_identity[i] : '.';

				snprintf(hex_str + (i*2),
					sizeof(hex_str)-(i*2),
					"%02x ",
					suspended_call->call_identity[i]);
			}
			sane_str[i] = '\0';
			hex_str[i*2] = '\0';

			ast_cli(fd, "    %s (%s)\n",
				sane_str,
				hex_str);
		}
	}

	ast_mutex_unlock(&visdn.lock);

	return 0;
}

static enum visdn_type_of_number
		visdn_string_to_type_of_number(const char *str)
{
	 enum visdn_type_of_number type_of_number =
				VISDN_TYPE_OF_NUMBER_UNKNOWN;

	if (!strcasecmp(str, "unknown"))
		type_of_number = VISDN_TYPE_OF_NUMBER_UNKNOWN;
	else if (!strcasecmp(str, "international"))
		type_of_number = VISDN_TYPE_OF_NUMBER_INTERNATIONAL;
	else if (!strcasecmp(str, "national"))
		type_of_number = VISDN_TYPE_OF_NUMBER_NATIONAL;
	else if (!strcasecmp(str, "network_specific"))
		type_of_number = VISDN_TYPE_OF_NUMBER_NETWORK_SPECIFIC;
	else if (!strcasecmp(str, "subscriber"))
		type_of_number = VISDN_TYPE_OF_NUMBER_SUBSCRIBER;
	else if (!strcasecmp(str, "abbreviated"))
		type_of_number = VISDN_TYPE_OF_NUMBER_ABBREVIATED;
	else {
		ast_log(LOG_ERROR,
			"Unknown type_of_number '%s'\n",
			str);
	}

	return type_of_number;
}

static enum q931_interface_network_role
	visdn_string_to_network_role(const char *str)
{
	enum q931_interface_network_role role = 0;

	if (!strcasecmp(str, "user"))
		role = Q931_INTF_NET_USER;
	else if (!strcasecmp(str, "private"))
		role = Q931_INTF_NET_PRIVATE;
	else if (!strcasecmp(str, "local"))
		role = Q931_INTF_NET_LOCAL;
	else if (!strcasecmp(str, "transit"))
		role = Q931_INTF_NET_TRANSIT;
	else if (!strcasecmp(str, "international"))
		role = Q931_INTF_NET_INTERNATIONAL;
	else {
		ast_log(LOG_ERROR,
			"Unknown network_role '%s'\n",
			str);
	}

	return role;
}

static int visdn_intf_from_var(
	struct visdn_interface *intf,
	struct ast_variable *var)
{
	if (!strcasecmp(var->name, "network_role")) {
		intf->network_role =
			visdn_string_to_network_role(var->value);
	} else if (!strcasecmp(var->name, "type_of_number")) {
		intf->type_of_number =
			visdn_string_to_type_of_number(var->value);
	} else if (!strcasecmp(var->name, "local_type_of_number")) {
		intf->local_type_of_number =
			visdn_string_to_type_of_number(var->value);
	} else if (!strcasecmp(var->name, "tones_option")) {
		intf->tones_option = ast_true(var->value);
	} else if (!strcasecmp(var->name, "context")) {
		strncpy(intf->context, var->value,
			sizeof(intf->context));
	} else if (!strcasecmp(var->name, "default_inbound_caller_id")) {
		strncpy(intf->default_inbound_caller_id, var->value,
			sizeof(intf->default_inbound_caller_id));
	} else if (!strcasecmp(var->name, "force_inbound_caller_id")) {
		intf->force_inbound_caller_id = ast_true(var->value);
	} else if (!strcasecmp(var->name, "overlap_sending")) {
		intf->overlap_sending = ast_true(var->value);
	} else if (!strcasecmp(var->name, "overlap_receiving")) {
		intf->overlap_receiving = ast_true(var->value);
	} else if (!strcasecmp(var->name, "national_prefix")) {
		strncpy(intf->national_prefix, var->value,
			sizeof(intf->national_prefix));
	} else if (!strcasecmp(var->name, "international_prefix")) {
		strncpy(intf->international_prefix, var->value,
			sizeof(intf->international_prefix));
	} else if (!strcasecmp(var->name, "autorelease_dlc")) {
		intf->dlc_autorelease_time = atoi(var->value);
	} else if (!strcasecmp(var->name, "t301")) {
		intf->T301 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t302")) {
		intf->T302 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t303")) {
		intf->T303 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t304")) {
		intf->T304 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t305")) {
		intf->T305 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t306")) {
		intf->T306 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t307")) {
		intf->T307 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t308")) {
		intf->T308 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t309")) {
		intf->T309 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t310")) {
		intf->T310 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t312")) {
		intf->T312 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t313")) {
		intf->T313 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t314")) {
		intf->T314 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t316")) {
		intf->T316 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t317")) {
		intf->T317 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t318")) {
		intf->T318 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t319")) {
		intf->T319 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t320")) {
		intf->T320 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t321")) {
		intf->T321 = atoi(var->value);
	} else if (!strcasecmp(var->name, "t322")) {
		intf->T322 = atoi(var->value);
	} else {
		return -1;
	}

	return 0;
}

static void visdn_copy_interface_config(
	struct visdn_interface *dst,
	const struct visdn_interface *src)
{
	dst->network_role = src->network_role;
	dst->type_of_number = src->type_of_number;
	dst->local_type_of_number = src->local_type_of_number;
	dst->tones_option = src->tones_option;
	strncpy(dst->context, src->context,
		sizeof(dst->context));
	strncpy(dst->default_inbound_caller_id, src->default_inbound_caller_id,
		sizeof(dst->default_inbound_caller_id));
	dst->force_inbound_caller_id = src->force_inbound_caller_id;
	dst->overlap_sending = src->overlap_sending;
	dst->overlap_receiving = src->overlap_receiving;
	strncpy(dst->national_prefix, src->national_prefix,
		sizeof(dst->national_prefix));
	strncpy(dst->international_prefix, src->international_prefix,
		sizeof(dst->international_prefix));
	dst->dlc_autorelease_time = src->dlc_autorelease_time;
	dst->T301 = src->T301;
	dst->T302 = src->T302;
	dst->T303 = src->T303;
	dst->T304 = src->T304;
	dst->T305 = src->T305;
	dst->T306 = src->T306;
	dst->T307 = src->T307;
	dst->T308 = src->T308;
	dst->T309 = src->T309;
	dst->T310 = src->T310;
	dst->T312 = src->T312;
	dst->T313 = src->T313;
	dst->T314 = src->T314;
	dst->T316 = src->T316;
	dst->T317 = src->T317;
	dst->T318 = src->T318;
	dst->T319 = src->T319;
	dst->T320 = src->T320;
	dst->T321 = src->T321;
	dst->T322 = src->T322;
}

static void visdn_reload_config(void)
{
	struct ast_config *cfg;
	cfg = ast_config_load(VISDN_CONFIG_FILE);
	if (!cfg) {
		ast_log(LOG_WARNING,
			"Unable to load config %s, VISDN disabled\n",
			VISDN_CONFIG_FILE);

		return;
	}

	ast_mutex_lock(&visdn.lock);

	struct ast_variable *var;
	var = ast_variable_browse(cfg, "global");
	while (var) {
		if (visdn_intf_from_var(&visdn.default_intf, var) < 0) {
			ast_log(LOG_WARNING,
				"Unknown configuration variable %s\n",
				var->name);
		}

		var = var->next;
	}

	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {
		intf->configured = FALSE;
	}

	const char *cat;
	for (cat = ast_category_browse(cfg, NULL); cat;
	     cat = ast_category_browse(cfg, (char *)cat)) {

		if (!strcasecmp(cat, "general") ||
		    !strcasecmp(cat, "global"))
			continue;

		int found = FALSE;
		list_for_each_entry(intf, &visdn.ifs, ifs_node) {
			if (!strcasecmp(intf->name, cat)) {
				found = TRUE;
				break;
			}
		}

		if (!found) {
			intf = malloc(sizeof(*intf));

			INIT_LIST_HEAD(&intf->suspended_calls);
			intf->q931_intf = NULL;
			intf->open_pending = FALSE;
			strncpy(intf->name, cat, sizeof(intf->name));
			visdn_copy_interface_config(intf, &visdn.default_intf);

			list_add_tail(&intf->ifs_node, &visdn.ifs);
		}

		intf->configured = TRUE;

		var = ast_variable_browse(cfg, (char *)cat);
		while (var) {
			if (visdn_intf_from_var(intf, var) < 0) {
				ast_log(LOG_WARNING,
					"Unknown configuration variable %s\n",
					var->name);
			}

			var = var->next;
		}
	}

	ast_mutex_unlock(&visdn.lock);

	ast_config_destroy(cfg);
}

static int do_visdn_reload(int fd, int argc, char *argv[])
{
	visdn_reload_config();

	return 0;
}

static int do_show_visdn_channels(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);

	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {

		if (!intf->q931_intf)
			continue;

		ast_cli(fd, "Interface: %s\n", intf->name);

		int j;
		for (j=0; j<intf->q931_intf->n_channels; j++) {
			ast_cli(fd, "  B%d: %s\n",
				intf->q931_intf->channels[j].id + 1,
				q931_channel_state_to_text(
					intf->q931_intf->channels[j].state));
		}
	}

	ast_mutex_unlock(&visdn.lock);

	return 0;
}

static inline struct ast_channel *callpvt_to_astchan(
	struct q931_call *call)
{
	return (struct ast_channel *)call->pvt;
}

static int visdn_cli_print_call_list(
	int fd,
	struct q931_interface *filter_intf)
{
	int first_call;

	ast_mutex_lock(&visdn.lock);

	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {

		if (!intf->q931_intf)
			continue;

		struct q931_call *call;
		first_call = TRUE;

		list_for_each_entry(call, &intf->q931_intf->calls, calls_node) {

			if (!filter_intf || call->intf == filter_intf) {

				if (first_call) {
					ast_cli(fd, "Interface: %s\n", intf->q931_intf->name);
					ast_cli(fd, "  Ref#    Caller       State\n");
					first_call = FALSE;
				}

				struct ast_channel *ast_chan =
						callpvt_to_astchan(call);

				struct visdn_chan *visdn_chan = NULL;
				if (ast_chan)
					visdn_chan = to_visdn_chan(ast_chan);

				ast_cli(fd, "  %c %5ld %-12s %s\n",
					(call->direction ==
						Q931_CALL_DIRECTION_INBOUND)
							? 'I' : 'O',
					call->call_reference,
					visdn_chan ?
						visdn_chan->calling_number :
						"",
					q931_call_state_to_text(call->state));

/*				ast_cli(fd, "  %c %5ld %-12s %-12s %s\n",
					(call->direction == Q931_CALL_DIRECTION_INBOUND)
						? 'I' : 'O',
					call->call_reference,
					call->calling_number,
					call->called_number,
					q931_call_state_to_text(call->state));
*/
			}
		}
	}

	ast_mutex_unlock(&visdn.lock);

	return RESULT_SUCCESS;
}

static void visdn_cli_print_call(int fd, struct q931_call *call)
{
	ast_cli(fd, "--------- Call %ld %s (%d refs)\n",
		call->call_reference,
		call->direction == Q931_CALL_DIRECTION_INBOUND ?
			"inbound" : "outbound",
		call->refcnt);

	ast_cli(fd, "Interface       : %s\n", call->intf->name);

	if (call->dlc)
		ast_cli(fd, "DLC (TEI)       : %d\n", call->dlc->tei);

	ast_cli(fd, "State           : %s\n",
		q931_call_state_to_text(call->state));

//	ast_cli(fd, "Calling Number  : %s\n", call->calling_number);
//	ast_cli(fd, "Called Number   : %s\n", call->called_number);

//	ast_cli(fd, "Sending complete: %s\n",
//		call->sending_complete ? "Yes" : "No");

	ast_cli(fd, "Broadcast seutp : %s\n",
		call->broadcast_setup ? "Yes" : "No");

	ast_cli(fd, "Tones option    : %s\n",
		call->tones_option ? "Yes" : "No");

	ast_cli(fd, "Active timers   : ");

	if (call->T301.pending) ast_cli(fd, "T301 ");
	if (call->T302.pending) ast_cli(fd, "T302 ");
	if (call->T303.pending) ast_cli(fd, "T303 ");
	if (call->T304.pending) ast_cli(fd, "T304 ");
	if (call->T305.pending) ast_cli(fd, "T305 ");
	if (call->T306.pending) ast_cli(fd, "T306 ");
	if (call->T308.pending) ast_cli(fd, "T308 ");
	if (call->T309.pending) ast_cli(fd, "T309 ");
	if (call->T310.pending) ast_cli(fd, "T310 ");
	if (call->T312.pending) ast_cli(fd, "T312 ");
	if (call->T313.pending) ast_cli(fd, "T313 ");
	if (call->T314.pending) ast_cli(fd, "T314 ");
	if (call->T316.pending) ast_cli(fd, "T316 ");
	if (call->T318.pending) ast_cli(fd, "T318 ");
	if (call->T319.pending) ast_cli(fd, "T319 ");
	if (call->T320.pending) ast_cli(fd, "T320 ");
	if (call->T321.pending) ast_cli(fd, "T321 ");
	if (call->T322.pending) ast_cli(fd, "T322 ");

	ast_cli(fd, "\n");

	ast_cli(fd, "CES:\n");
	struct q931_ces *ces;
	list_for_each_entry(ces, &call->ces, node) {
		ast_cli(fd, "%d %s %s ",
			ces->dlc->tei,
			q931_ces_state_to_text(ces->state),
			(ces == call->selected_ces ? "presel" :
			  (ces == call->preselected_ces ? "sel" : "")));

		if (ces->T304.pending) ast_cli(fd, "T304 ");
		if (ces->T308.pending) ast_cli(fd, "T308 ");
		if (ces->T322.pending) ast_cli(fd, "T322 ");

		ast_cli(fd, "\n");
	}

}

static int do_show_visdn_calls(int fd, int argc, char *argv[])
{
	ast_mutex_lock(&visdn.lock);

	if (argc == 3) {
		visdn_cli_print_call_list(fd, NULL);
	} else if (argc == 4) {
		char *callpos = strchr(argv[3], '/');
		if (callpos) {
			*callpos = '\0';
			callpos++;
		}

		struct visdn_interface *filter_intf = NULL;
		struct visdn_interface *intf;
		list_for_each_entry(intf, &visdn.ifs, ifs_node) {
			if (intf->q931_intf &&
			    !strcasecmp(intf->name, argv[3])) {
				filter_intf = intf;
				break;
			}
		}

		if (!filter_intf) {
			ast_cli(fd, "Interface not found\n");
			goto err_intf_not_found;
		}

		if (!callpos) {
			visdn_cli_print_call_list(fd, filter_intf->q931_intf);
		} else {
			struct q931_call *call;

			if (callpos[0] == 'i' || callpos[0] == 'I') {
				call = q931_get_call_by_reference(
							filter_intf->q931_intf,
					Q931_CALL_DIRECTION_INBOUND,
					atoi(callpos + 1));
			} else if (callpos[0] == 'o' || callpos[0] == 'O') {
				call = q931_get_call_by_reference(
							filter_intf->q931_intf,
					Q931_CALL_DIRECTION_OUTBOUND,
					atoi(callpos + 1));
			} else {
				ast_cli(fd, "Invalid call reference\n");
				goto err_unknown_direction;
			}

			if (!call) {
				ast_cli(fd, "Call %s not found\n", callpos);
				goto err_call_not_found;
			}

			visdn_cli_print_call(fd, call);

			q931_call_put(call);
		}
	}

err_call_not_found:
err_unknown_direction:
err_intf_not_found:

	ast_mutex_unlock(&visdn.lock);

	return RESULT_SUCCESS;
}

static char debug_visdn_generic_help[] =
	"Usage: debug visdn generic\n"
	"	Debug generic vISDN events\n";

static struct ast_cli_entry debug_visdn_generic =
{
	{ "debug", "visdn", "generic", NULL },
	do_debug_visdn_generic,
	"Enables generic vISDN debugging",
	debug_visdn_generic_help,
	NULL
};

static struct ast_cli_entry no_debug_visdn_generic =
{
	{ "no", "debug", "visdn", "generic", NULL },
	do_no_debug_visdn_generic,
	"Disables generic vISDN debugging",
	NULL,
	NULL
};

static char debug_visdn_q921_help[] =
	"Usage: debug visdn q921 [interface]\n"
	"	Traces q921 traffic\n";

static struct ast_cli_entry debug_visdn_q921 =
{
	{ "debug", "visdn", "q921", NULL },
	do_debug_visdn_q921,
	"Enables q.921 tracing",
	debug_visdn_q921_help,
	NULL
};

static struct ast_cli_entry no_debug_visdn_q921 =
{
	{ "no", "debug", "visdn", "q921", NULL },
	do_no_debug_visdn_q921,
	"Disables q.921 tracing",
	NULL,
	NULL
};

static char debug_visdn_q931_help[] =
	"Usage: debug visdn q931 [interface]\n"
	"	Traces q931 traffic\n";

static struct ast_cli_entry debug_visdn_q931 =
{
	{ "debug", "visdn", "q931", NULL },
	do_debug_visdn_q931,
	"Enables q.931 tracing",
	debug_visdn_q931_help,
	NULL
};

static struct ast_cli_entry no_debug_visdn_q931 =
{
	{ "no", "debug", "visdn", "q931", NULL },
	do_no_debug_visdn_q931,
	"Disables q.931 tracing",
	NULL,
	NULL
};

static char show_visdn_interfaces_help[] =
	"Usage: visdn show interfaces\n"
	"	Displays informations on vISDN interfaces\n";

static struct ast_cli_entry show_visdn_interfaces =
{
	{ "show", "visdn", "interfaces", NULL },
	do_show_visdn_interfaces,
	"Displays vISDN interface information",
	show_visdn_interfaces_help,
	NULL
};

static char visdn_visdn_reload_help[] =
	"Usage: visdn reload\n"
	"	Reloads vISDN config\n";

static struct ast_cli_entry visdn_reload =
{
	{ "visdn", "reload", NULL },
	do_visdn_reload,
	"Reloads vISDN configuration",
	visdn_visdn_reload_help,
	NULL
};

static char visdn_show_visdn_channels_help[] =
	"Usage: visdn show channels\n"
	"	Displays informations on vISDN channels\n";

static struct ast_cli_entry show_visdn_channels =
{
	{ "show", "visdn", "channels", NULL },
	do_show_visdn_channels,
	"Displays vISDN channel information",
	visdn_show_visdn_channels_help,
	NULL
};

static char show_visdn_calls_help[] =
	"Usage: show visdn calls\n"
	"	Lists vISDN calls\n";

static struct ast_cli_entry show_visdn_calls =
{
	{ "show", "visdn", "calls", NULL },
	do_show_visdn_calls,
	"Lists vISDN calls",
	show_visdn_calls_help,
	NULL
};

static enum q931_ie_called_party_number_type_of_number
	visdn_type_of_number_to_cdpn(enum visdn_type_of_number type_of_number)
{
	switch(type_of_number) {
	case VISDN_TYPE_OF_NUMBER_UNKNOWN:
		return Q931_IE_CDPN_TON_UNKNOWN;
	case VISDN_TYPE_OF_NUMBER_INTERNATIONAL:
		return Q931_IE_CDPN_TON_INTERNATIONAL;
	case VISDN_TYPE_OF_NUMBER_NATIONAL:
		return Q931_IE_CDPN_TON_NATIONAL;
	case VISDN_TYPE_OF_NUMBER_NETWORK_SPECIFIC:
		return Q931_IE_CDPN_TON_NETWORK_SPECIFIC;
	case VISDN_TYPE_OF_NUMBER_SUBSCRIBER:
		return Q931_IE_CDPN_TON_SUBSCRIBER;
	case VISDN_TYPE_OF_NUMBER_ABBREVIATED:
		return Q931_IE_CDPN_TON_ABBREVIATED;
	}

	assert(0);
}

static enum q931_ie_calling_party_number_type_of_number
	visdn_type_of_number_to_cgpn(enum visdn_type_of_number type_of_number)
{
	switch(type_of_number) {
	case VISDN_TYPE_OF_NUMBER_UNKNOWN:
		return Q931_IE_CGPN_TON_UNKNOWN;
	case VISDN_TYPE_OF_NUMBER_INTERNATIONAL:
		return Q931_IE_CDPN_TON_INTERNATIONAL;
	case VISDN_TYPE_OF_NUMBER_NATIONAL:
		return Q931_IE_CGPN_TON_NATIONAL;
	case VISDN_TYPE_OF_NUMBER_NETWORK_SPECIFIC:
		return Q931_IE_CGPN_TON_NETWORK_SPECIFIC;
	case VISDN_TYPE_OF_NUMBER_SUBSCRIBER:
		return Q931_IE_CGPN_TON_SUBSCRIBER;
	case VISDN_TYPE_OF_NUMBER_ABBREVIATED:
		return Q931_IE_CGPN_TON_ABBREVIATED;
	}

	return 0;
}

void q931_send_primitive(
	struct q931_call *call,
	enum q931_primitive primitive,
	struct q931_ies *ies)
{
	struct q931_ccb_message *msg;
	msg = malloc(sizeof(*msg));
	if (!msg)
		return;

	msg->call = call;
	msg->primitive = primitive;

	q931_ies_init(&msg->ies);

	if (ies)
		q931_ies_copy(&msg->ies, ies);

	ast_mutex_lock(&visdn.ccb_q931_queue_lock);
	list_add_tail(&msg->node, &visdn.ccb_q931_queue);
	ast_mutex_unlock(&visdn.ccb_q931_queue_lock);

	if (write(visdn.ccb_q931_queue_pipe_write, " ", 1) < 0) {
		ast_log(LOG_WARNING,
			"Cannot write on ccb_q931_pipe_write\n");
	}
}

void visdn_queue_primitive(
	struct q931_call *call,
	enum q931_primitive primitive,
	const struct q931_ies *ies,
	unsigned long par1,
	unsigned long par2)
{
	struct q931_ccb_message *msg;
	msg = malloc(sizeof(*msg));
	if (!msg)
		return;

	msg->call = call;
	msg->primitive = primitive;
	msg->par1 = par1;
	msg->par2 = par2;

	q931_ies_init(&msg->ies);

	if (ies)
		q931_ies_copy(&msg->ies, ies);

	ast_mutex_lock(&visdn.q931_ccb_queue_lock);
	list_add_tail(&msg->node, &visdn.q931_ccb_queue);
	ast_mutex_unlock(&visdn.q931_ccb_queue_lock);

	if (write(visdn.q931_ccb_queue_pipe_write, " ", 1) < 0) {
		ast_log(LOG_WARNING,
			"Cannot write on q931_ccb_pipe_write\n");
	}
}


static int visdn_call(
	struct ast_channel *ast_chan,
	char *orig_dest,
	int timeout)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	int err;
	char dest[256];

	strncpy(dest, orig_dest, sizeof(dest));

	// Parse destination and obtain interface name + number
	const char *intf_name;
	const char *number;
	char *stringp = dest;

	intf_name = strsep(&stringp, "/");
	if (!intf_name) {
		ast_log(LOG_WARNING,
			"Invalid destination '%s' format (interface/number)\n",
			dest);

		err = -1;
		goto err_invalid_destination;
	}

	number = strsep(&stringp, "/");
	if (!number) {
		ast_log(LOG_WARNING,
			"Invalid destination '%s' format (interface/number)\n",
			dest);

		err = -1;
		goto err_invalid_format;
	}

	enum q931_ie_bearer_capability_information_transfer_capability bc_itc =
		Q931_IE_BC_ITC_SPEECH;
	enum q931_ie_bearer_capability_user_information_layer_1_protocol bc_l1p =
		Q931_IE_BC_UIL1P_G711_ALAW;

	visdn_chan->is_voice = TRUE;

	const char *options = strsep(&stringp, "/");
	if (options) {
		if (strchr(options, 'D')) {
			bc_itc = Q931_IE_BC_ITC_UNRESTRICTED_DIGITAL;
			bc_l1p = Q931_IE_BC_UIL1P_UNUSED;
			visdn_chan->is_voice = FALSE;
		}
	}

	int found = FALSE;
	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {
		if (intf->q931_intf &&
		    !strcmp(intf->name, intf_name)) {
			found = TRUE;
			break;
		}
	}

	if (!found) {
		ast_log(LOG_WARNING, "Interface %s not found\n", intf_name);
		err = -1;
		goto err_intf_not_found;
	}

	if (!intf->q931_intf) {
		ast_log(LOG_WARNING, "Interface %s not present\n", intf_name);
		err = -1;
		goto err_intf_not_found;
	}

	struct q931_call *q931_call;
	q931_call = q931_call_alloc_out(intf->q931_intf);
	if (!q931_call) {
		ast_log(LOG_WARNING, "Cannot allocate outbound call\n");
		err = -1;
		goto err_call_alloc;
	}

	if ((ast_chan->_state != AST_STATE_DOWN) &&
	    (ast_chan->_state != AST_STATE_RESERVED)) {
		ast_log(LOG_WARNING,
			"visdn_call called on %s,"
			" neither down nor reserved\n",
			ast_chan->name);

		err = -1;
		goto err_channel_not_down;
	}

	if (option_debug)
		ast_log(LOG_DEBUG,
			"Calling %s on %s\n",
			dest, ast_chan->name);

	q931_call->pvt = ast_chan;

	visdn_chan->q931_call = q931_call_get(q931_call);

	char newname[40];
	snprintf(newname, sizeof(newname), "VISDN/%s/%c%ld",
		q931_call->intf->name,
		q931_call->direction == Q931_CALL_DIRECTION_INBOUND ? 'I' : 'O',
		q931_call->call_reference);

	ast_change_name(ast_chan, newname);

	ast_setstate(ast_chan, AST_STATE_DIALING);

	struct q931_ies ies = Q931_IES_INIT;

	struct q931_ie_bearer_capability *bc =
		q931_ie_bearer_capability_alloc();
	bc->coding_standard = Q931_IE_BC_CS_CCITT;
	bc->information_transfer_capability = bc_itc;
	bc->transfer_mode = Q931_IE_BC_TM_CIRCUIT;
	bc->information_transfer_rate = Q931_IE_BC_ITR_64;
	bc->user_information_layer_1_protocol = bc_l1p;
	bc->user_information_layer_2_protocol = Q931_IE_BC_UIL2P_UNUSED;
	bc->user_information_layer_3_protocol = Q931_IE_BC_UIL3P_UNUSED;
	q931_ies_add_put(&ies, &bc->ie);

	struct q931_ie_called_party_number *cdpn =
		q931_ie_called_party_number_alloc();
	cdpn->type_of_number =
		visdn_type_of_number_to_cdpn(intf->type_of_number);
	cdpn->numbering_plan_identificator = Q931_IE_CDPN_NPI_ISDN_TELEPHONY;
	snprintf(cdpn->number, sizeof(cdpn->number), "%s", number);
	q931_ies_add_put(&ies, &cdpn->ie);

	if (intf->q931_intf->role == LAPD_ROLE_NT &&
	    !intf->overlap_receiving) {
		struct q931_ie_sending_complete *sc =
			q931_ie_sending_complete_alloc();

		q931_ies_add_put(&ies, &sc->ie);
	}

	if (ast_chan->caller_id_number && strlen(ast_chan->caller_id_number)) {

		struct q931_ie_calling_party_number *cgpn =
			q931_ie_calling_party_number_alloc();

		cgpn->type_of_number =
			visdn_type_of_number_to_cgpn(
					intf->local_type_of_number);
		cgpn->numbering_plan_identificator =
			Q931_IE_CGPN_NPI_ISDN_TELEPHONY;
		cgpn->presentation_indicator =
			Q931_IE_CGPN_PI_PRESENTATION_ALLOWED;
		cgpn->screening_indicator =
			Q931_IE_CGPN_SI_USER_PROVIDED_VERIFIED_AND_PASSED;

#ifndef ASTERISK_VERSION_NUM
		char callerid[255];
		char *name, *number;
		strncpy(callerid, ast_chan->callerid, sizeof(callerid));
		ast_callerid_parse(callerid, &name, &number);
		if (number) {
			strncpy(cgpn->number, number, sizeof(cgpn->number));

			q931_ies_add_put(&ies, &cgpn->ie);
		} else {
			ast_log(LOG_WARNING,
				"Unable to parse '%s'"
				" into CallerID name & number\n",
				callerid);
		}
#else
		strncpy(cgpn->number, ast_chan->cid.cid_num,
			sizeof(cgpn->number));

		q931_ies_add_put(&ies, &cgpn->ie);
#endif
	}

	struct q931_ie_high_layer_compatibility *hlc =
		q931_ie_high_layer_compatibility_alloc();

	hlc->coding_standard = Q931_IE_HLC_CS_CCITT;
	hlc->interpretation = Q931_IE_HLC_P_FIRST;
	hlc->presentation_method = Q931_IE_HLC_PM_HIGH_LAYER_PROTOCOL_PROFILE;
	hlc->characteristics_identification = Q931_IE_HLC_CI_TELEPHONY;

	q931_send_primitive(q931_call, Q931_CCB_SETUP_REQUEST, &ies);

	return 0;

err_channel_not_down:
	q931_call_release_reference(q931_call);
	q931_call_put(q931_call);
err_call_alloc:
err_intf_not_found:
err_invalid_format:
err_invalid_destination:

	return err;
}

static int visdn_answer(struct ast_channel *ast_chan)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	visdn_debug("visdn_answer\n");

	ast_indicate(ast_chan, -1);

	if (!visdn_chan) {
		ast_log(LOG_ERROR, "NO VISDN_CHAN!!\n");
		return -1;
	}

	if (visdn_chan->q931_call->state == U6_CALL_PRESENT ||
	    visdn_chan->q931_call->state == U7_CALL_RECEIVED ||
	    visdn_chan->q931_call->state == U9_INCOMING_CALL_PROCEEDING ||
	    visdn_chan->q931_call->state == U25_OVERLAP_RECEIVING ||
	    visdn_chan->q931_call->state == N2_OVERLAP_SENDING ||
	    visdn_chan->q931_call->state == N3_OUTGOING_CALL_PROCEEDING ||
	    visdn_chan->q931_call->state == N4_CALL_DELIVERED) {
		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_SETUP_RESPONSE, NULL);
	}

	return 0;
}

#ifndef ASTERISK_VERSION_NUM
static int visdn_bridge(
	struct ast_channel *c0,
	struct ast_channel *c1,
	int flags, struct ast_frame **fo,
	struct ast_channel **rc)
#else
static int visdn_bridge(
	struct ast_channel *c0,
	struct ast_channel *c1,
	int flags, struct ast_frame **fo,
	struct ast_channel **rc,
	int timeoutms)
#endif
{
	return AST_BRIDGE_FAILED_NOWARN;

#if 0
	/* if need DTMF, cant native bridge (at least not yet...) */
	if (flags & (AST_BRIDGE_DTMF_CHANNEL_0 | AST_BRIDGE_DTMF_CHANNEL_1))
		return AST_BRIDGE_FAILED;

	struct visdn_chan *visdn_chan1 = to_visdn_chan(c0);
	struct visdn_chan *visdn_chan2 = to_visdn_chan(c1);

	char path[100], dest1[100], dest2[100];

	snprintf(path, sizeof(path),
		"/sys/class/net/%s/visdn_channel/connected/../B%d",
		visdn_chan1->q931_call->intf->name,
		visdn_chan1->q931_call->channel->id+1);

	memset(dest1, 0, sizeof(dest1));
	if (readlink(path, dest1, sizeof(dest1) - 1) < 0) {
		ast_log(LOG_ERROR, "readlink(%s): %s\n", path, strerror(errno));
		return AST_BRIDGE_FAILED;
	}

	char *chanid1 = strrchr(dest1, '/');
	if (!chanid1 || !strlen(chanid1 + 1)) {
		ast_log(LOG_ERROR,
			"Invalid chanid found in symlink %s\n",
			dest1);
		return AST_BRIDGE_FAILED;
	}

	chanid1++;

	snprintf(path, sizeof(path),
		"/sys/class/net/%s/visdn_channel/connected/../B%d",
		visdn_chan2->q931_call->intf->name,
		visdn_chan2->q931_call->channel->id+1);

	memset(dest2, 0, sizeof(dest2));
	if (readlink(path, dest2, sizeof(dest2) - 1) < 0) {
		ast_log(LOG_ERROR, "readlink(%s): %s\n", path, strerror(errno));
		return AST_BRIDGE_FAILED;
	}

	char *chanid2 = strrchr(dest2, '/');
	if (!chanid2 || !strlen(chanid2 + 1)) {
		ast_log(LOG_ERROR,
			"Invalid chanid found in symlink %s\n",
			dest2);
		return AST_BRIDGE_FAILED;
	}

	chanid2++;

	visdn_debug("Connecting chan %s to chan %s\n", chanid1, chanid2);

	int fd = open("/sys/visdn_tdm/internal-cxc/connect", O_WRONLY);
	if (fd < 0) {
		ast_log(LOG_ERROR,
			"Cannot open /sys/visdn_tdm/internal-cxc/connect: %s\n",
			strerror(errno));
		return AST_BRIDGE_FAILED;
	}

	if (ioctl(visdn_chan1->channel_fd,
			VISDN_IOC_DISCONNECT, NULL) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_IOC_DISCONNECT): %s\n",
			strerror(errno));
	}

	close(visdn_chan1->channel_fd);
	visdn_chan1->channel_fd = -1;

	if (ioctl(visdn_chan2->channel_fd,
			VISDN_IOC_DISCONNECT, NULL) < 0) {
		ast_log(LOG_ERROR,
			"ioctl(VISDN_IOC_DISCONNECT): %s\n",
			strerror(errno));
	}

	close(visdn_chan2->channel_fd);
	visdn_chan2->channel_fd = -1;

	char command[256];
	snprintf(command, sizeof(command),
		"%s\n%s\n",
		chanid1,
		chanid2);

	if (write(fd, command, strlen(command)) < 0) {
		ast_log(LOG_ERROR,
			"Cannot write to /sys/visdn_tdm/internal-cxc/connect: %s\n",
			strerror(errno));

		close(fd);

		return AST_BRIDGE_FAILED;
	}

	close(fd);

	struct ast_channel *cs[2];
	cs[0] = c0;
	cs[1] = c1;

	struct ast_channel *who = NULL;
	for (;;) {
		int to = -1;
		who = ast_waitfor_n(cs, 2, &to);
		if (!who) {
			ast_log(LOG_DEBUG, "Ooh, empty read...\n");
			continue;
		}

		struct ast_frame *f;
		f = ast_read(who);
		if (!f)
			break;

		if (f->frametype == AST_FRAME_DTMF) {
			if (((who == c0) && (flags & AST_BRIDGE_DTMF_CHANNEL_0)) ||
			    ((who == c1) && (flags & AST_BRIDGE_DTMF_CHANNEL_1))) {

				*fo = f;
				*rc = who;

				// Disconnect channels
				return AST_BRIDGE_COMPLETE;
			}

			if (who == c0)
				ast_write(c1, f);
			else
				ast_write(c0, f);
		}

		ast_frfree(f);

		// Braindead anyone?
		struct ast_channel *t;
		t = cs[0];
		cs[0] = cs[1];
		cs[1] = t;
	}

	// Really braindead
	*fo = NULL;
	*rc = who;

#endif

	return AST_BRIDGE_COMPLETE;
}

struct ast_frame *visdn_exception(struct ast_channel *ast_chan)
{
	ast_log(LOG_WARNING, "visdn_exception\n");

	return NULL;
}

/* We are called with chan->lock'ed */
static int visdn_indicate(struct ast_channel *ast_chan, int condition)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	if (!visdn_chan) {
		ast_log(LOG_ERROR, "NO VISDN_CHAN!!\n");
		return 1;
	}

	visdn_debug("visdn_indicate %d\n", condition);

	switch(condition) {
	case AST_CONTROL_RING:
	case AST_CONTROL_TAKEOFFHOOK:
	case AST_CONTROL_FLASH:
	case AST_CONTROL_WINK:
	case AST_CONTROL_OPTION:
	case AST_CONTROL_RADIO_KEY:
	case AST_CONTROL_RADIO_UNKEY:
		return 1;
	break;

	case -1:
		ast_playtones_stop(ast_chan);

		return 0;
	break;

	case AST_CONTROL_OFFHOOK: {
		const struct tone_zone_sound *tone;
		tone = ast_get_indication_tone(ast_chan->zone, "dial");
		if (tone)
			ast_playtones_start(ast_chan, 0, tone->data, 1);

		return 0;
	}
	break;

	case AST_CONTROL_HANGUP: {
		const struct tone_zone_sound *tone;
		tone = ast_get_indication_tone(ast_chan->zone, "congestion");
		if (tone)
			ast_playtones_start(ast_chan, 0, tone->data, 1);

		return 0;
	}
	break;

	case AST_CONTROL_RINGING: {
		struct q931_ies ies = Q931_IES_INIT;

		struct q931_ie_progress_indicator *pi = NULL;

		if (ast_chan->_bridge &&
		   strcmp(ast_chan->_bridge->type, VISDN_CHAN_TYPE)) {

			visdn_debug("Channel is not VISDN, sending"
					" progress indicator\n");

			pi = q931_ie_progress_indicator_alloc();
			pi->coding_standard = Q931_IE_PI_CS_CCITT;
			pi->location = q931_ie_progress_indicator_location(
						visdn_chan->q931_call);
			pi->progress_description =
				Q931_IE_PI_PD_CALL_NOT_END_TO_END;
			q931_ies_add_put(&ies, &pi->ie);
		}

		pi = q931_ie_progress_indicator_alloc();
		pi->coding_standard = Q931_IE_PI_CS_CCITT;
		pi->location = q931_ie_progress_indicator_location(
					visdn_chan->q931_call);
		pi->progress_description = Q931_IE_PI_PD_IN_BAND_INFORMATION;
		q931_ies_add_put(&ies, &pi->ie);

		if (visdn_chan->q931_call->state == N1_CALL_INITIATED)
			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_PROCEEDING_REQUEST, NULL);

		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_ALERTING_REQUEST, &ies);

		const struct tone_zone_sound *tone;
		tone = ast_get_indication_tone(ast_chan->zone, "ring");
		if (tone)
			ast_playtones_start(ast_chan, 0, tone->data, 1);

		return 0;
	}
	break;

	case AST_CONTROL_ANSWER:
		ast_playtones_stop(ast_chan);

		return 0;
	break;

	case AST_CONTROL_BUSY: {
		struct q931_ies ies = Q931_IES_INIT;

		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_call(
							visdn_chan->q931_call);
		cause->value = Q931_IE_C_CV_USER_BUSY;
		q931_ies_add_put(&ies, &cause->ie);

		q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_DISCONNECT_REQUEST, &ies);

		const struct tone_zone_sound *tone;
		tone = ast_get_indication_tone(ast_chan->zone, "busy");
		if (tone)
			ast_playtones_start(ast_chan, 0, tone->data, 1);

		return 0;
	}
	break;

	case AST_CONTROL_CONGESTION: {
		struct q931_ies ies = Q931_IES_INIT;

		struct q931_ie_cause *cause = q931_ie_cause_alloc();
		cause->coding_standard = Q931_IE_C_CS_CCITT;
		cause->location = q931_ie_cause_location_call(
							visdn_chan->q931_call);
		cause->value = Q931_IE_C_CV_DESTINATION_OUT_OF_ORDER;
		q931_ies_add_put(&ies, &cause->ie);

		q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_DISCONNECT_REQUEST, &ies);

		const struct tone_zone_sound *tone;
		tone = ast_get_indication_tone(ast_chan->zone, "busy");
		if (tone)
			ast_playtones_start(ast_chan, 0, tone->data, 1);

		return 0;
	}
	break;

	case AST_CONTROL_PROGRESS: {
		struct q931_ies ies = Q931_IES_INIT;

		struct q931_ie_progress_indicator *pi =
			q931_ie_progress_indicator_alloc();
		pi->coding_standard = Q931_IE_PI_CS_CCITT;
		pi->location = q931_ie_progress_indicator_location(
					visdn_chan->q931_call);

		if (ast_chan->_bridge &&
		   strcmp(ast_chan->_bridge->type, VISDN_CHAN_TYPE)) {
			pi->progress_description =
				Q931_IE_PI_PD_CALL_NOT_END_TO_END; // FIXME
		} else if (visdn_chan->is_voice) {
			pi->progress_description =
				Q931_IE_PI_PD_IN_BAND_INFORMATION;
		}

		q931_ies_add_put(&ies, &pi->ie);

		q931_send_primitive(visdn_chan->q931_call, Q931_CCB_PROGRESS_REQUEST, &ies);

		return 0;
	}
	break;

	case AST_CONTROL_PROCEEDING:
		q931_send_primitive(visdn_chan->q931_call, Q931_CCB_PROCEEDING_REQUEST, NULL);

		return 0;
	break;
	}

	return 1;
}

static int visdn_fixup(
	struct ast_channel *oldchan,
	struct ast_channel *newchan)
{
	struct visdn_chan *chan = to_visdn_chan(newchan);

	if (chan->ast_chan != oldchan) {
		ast_log(LOG_WARNING, "old channel wasn't %p but was %p\n",
				oldchan, chan->ast_chan);
		return -1;
	}

	chan->ast_chan = newchan;

	return 0;
}

static int visdn_setoption(
	struct ast_channel *ast_chan,
	int option,
	void *data,
	int datalen)
{
	ast_log(LOG_ERROR, "%s\n", __FUNCTION__);

	return -1;
}

#ifndef ASTERISK_VERSION_NUM
static int visdn_transfer(
	struct ast_channel *ast,
	char *dest)
#else
static int visdn_transfer(
	struct ast_channel *ast,
	const char *dest)
#endif
{
	ast_log(LOG_ERROR, "%s\n", __FUNCTION__);

	return -1;
}

static int visdn_send_digit(struct ast_channel *ast_chan, char digit)
{
	ast_log(LOG_NOTICE, "%s %c\n", __FUNCTION__, digit);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	struct q931_call *q931_call = visdn_chan->q931_call;
	struct visdn_interface *intf = q931_call->intf->pvt;

	if (visdn_chan->may_send_digits) {
		visdn_debug("Not ready to send digits, queuing\n");

		struct q931_ies ies = Q931_IES_INIT;

		struct q931_ie_called_party_number *cdpn =
			q931_ie_called_party_number_alloc();
		cdpn->type_of_number = visdn_type_of_number_to_cdpn(
							intf->type_of_number);
		cdpn->numbering_plan_identificator =
			Q931_IE_CDPN_NPI_ISDN_TELEPHONY;

		cdpn->number[0] = digit;
		cdpn->number[1] = '\0';
		q931_ies_add_put(&ies, &cdpn->ie);

		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_INFO_REQUEST, &ies);
	} else {
		visdn_chan->queued_digits[
			strlen(visdn_chan->queued_digits)] = digit;
	}

	return 1;
}

#ifndef ASTERISK_VERSION_NUM
static int visdn_sendtext(struct ast_channel *ast, char *text)
#else
static int visdn_sendtext(struct ast_channel *ast, const char *text)
#endif
{
	ast_log(LOG_WARNING, "%s\n", __FUNCTION__);

	return -1;
}

static void visdn_destroy(struct visdn_chan *visdn_chan)
{
	free(visdn_chan);
}

static struct visdn_chan *visdn_alloc()
{
	struct visdn_chan *visdn_chan;

	visdn_chan = malloc(sizeof(*visdn_chan));
	if (!visdn_chan)
		return NULL;

	memset(visdn_chan, 0, sizeof(*visdn_chan));

	visdn_chan->channel_fd = -1;

	return visdn_chan;
}

static int visdn_hangup(struct ast_channel *ast_chan)
{
	visdn_debug("visdn_hangup %s\n", ast_chan->name);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	struct q931_call *q931_call = visdn_chan->q931_call;

	ast_mutex_lock(&visdn.lock);
	if (q931_call) {
		q931_call->pvt = NULL;

		if (
		    q931_call->state != N0_NULL_STATE &&
		    q931_call->state != N1_CALL_INITIATED &&
		    q931_call->state != N11_DISCONNECT_REQUEST &&
		    q931_call->state != N12_DISCONNECT_INDICATION &&
		    q931_call->state != N15_SUSPEND_REQUEST &&
		    q931_call->state != N17_RESUME_REQUEST &&
		    q931_call->state != N19_RELEASE_REQUEST &&
		    q931_call->state != N22_CALL_ABORT &&
		    q931_call->state != U0_NULL_STATE &&
		    q931_call->state != U6_CALL_PRESENT &&
		    q931_call->state != U11_DISCONNECT_REQUEST &&
		    q931_call->state != U12_DISCONNECT_INDICATION &&
		    q931_call->state != U15_SUSPEND_REQUEST &&
		    q931_call->state != U17_RESUME_REQUEST &&
		    q931_call->state != U19_RELEASE_REQUEST) {

			struct q931_ies ies = Q931_IES_INIT;

			struct q931_ie_cause *cause = q931_ie_cause_alloc();
			cause->coding_standard = Q931_IE_C_CS_CCITT;
			cause->location = q931_ie_cause_location_call(
							q931_call);
			cause->value = Q931_IE_C_CV_NORMAL_CALL_CLEARING;
			q931_ies_add_put(&ies, &cause->ie);

			q931_send_primitive(q931_call,
				Q931_CCB_DISCONNECT_REQUEST, &ies);
		}

		q931_call_put(q931_call);
	}

	ast_mutex_unlock(&visdn.lock);

	ast_mutex_lock(&ast_chan->lock);

	visdn_chan->q931_call = NULL;

	if (visdn_chan->suspended_call) {
		// We are responsible for the channel
		q931_channel_release(visdn_chan->suspended_call->q931_chan);

		list_del(&visdn_chan->suspended_call->node);
		free(visdn_chan->suspended_call);
		visdn_chan->suspended_call = NULL;
	}

	close(ast_chan->fds[0]);

	if (visdn_chan) {
		if (visdn_chan->channel_fd >= 0) {
			// Disconnect the softport since we cannot rely on
			// libq931 (see above)
			if (ioctl(visdn_chan->channel_fd,
					VISDN_IOC_DISCONNECT_PATH, NULL) < 0) {
				ast_log(LOG_ERROR,
					"ioctl(VISDN_IOC_DISCONNECT): %s\n",
					strerror(errno));
			}

			if (close(visdn_chan->channel_fd) < 0) {
				ast_log(LOG_ERROR,
					"close(visdn_chan->channel_fd): %s\n",
					strerror(errno));
			}

			visdn_chan->channel_fd = -1;
		}

		visdn_destroy(visdn_chan);

#ifndef ASTERISK_VERSION_NUM
		ast_chan->pvt->pvt = NULL;
#else
		ast_chan->tech_pvt = NULL;
#endif
	}

	ast_setstate(ast_chan, AST_STATE_DOWN);

	ast_mutex_unlock(&ast_chan->lock);

	visdn_debug("visdn_hangup complete\n");

	return 0;
}

static struct ast_frame *visdn_read(struct ast_channel *ast_chan)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	static struct ast_frame f;
	char buf[512];

	read(ast_chan->fds[0], buf, 1);

	f.src = VISDN_CHAN_TYPE;
	f.mallocd = 0;
	f.delivery.tv_sec = 0;
	f.delivery.tv_usec = 0;

	if (visdn_chan->channel_fd < 0) {
		f.frametype = AST_FRAME_NULL;
		f.subclass = 0;
		f.samples = 0;
		f.datalen = 0;
		f.data = NULL;
		f.offset = 0;

		return &f;
	}

	int nread = read(visdn_chan->channel_fd, buf, sizeof(buf));
	if (nread < 0) {
		ast_log(LOG_WARNING, "read error: %s\n", strerror(errno));
		return &f;
	}

#if 0
struct timeval tv;
gettimeofday(&tv, NULL);
unsigned long long t = tv.tv_sec * 1000000ULL + tv.tv_usec;
ast_verbose(VERBOSE_PREFIX_3 "R %.3f %d\n",
	t/1000000.0,
	visdn_chan->channel_fd);
#endif

	f.frametype = AST_FRAME_VOICE;
	f.subclass = AST_FORMAT_ALAW;
	f.samples = nread;
	f.datalen = nread;
	f.data = buf;
	f.offset = 0;

	return &f;
}

static int visdn_write(
	struct ast_channel *ast_chan,
	struct ast_frame *frame)
{
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	if (frame->frametype != AST_FRAME_VOICE) {
		ast_log(LOG_WARNING,
			"Don't know what to do with frame type '%d'\n",
			frame->frametype);

		return 0;
	}

	if (frame->subclass != AST_FORMAT_ALAW) {
		ast_log(LOG_WARNING,
			"Cannot handle frames in %d format\n",
			frame->subclass);
		return 0;
	}

	if (visdn_chan->channel_fd < 0) {
//		ast_log(LOG_WARNING,
//			"Attempting to write on unconnected channel\n");
		return 0;
	}

#if 0
ast_verbose(VERBOSE_PREFIX_3 "W %d %02x%02x%02x%02x%02x%02x%02x%02x %d\n", visdn_chan->channel_fd,
	*(__u8 *)(frame->data + 0),
	*(__u8 *)(frame->data + 1),
	*(__u8 *)(frame->data + 2),
	*(__u8 *)(frame->data + 3),
	*(__u8 *)(frame->data + 4),
	*(__u8 *)(frame->data + 5),
	*(__u8 *)(frame->data + 6),
	*(__u8 *)(frame->data + 7),
	frame->datalen);
#endif

	write(visdn_chan->channel_fd, frame->data, frame->datalen);

	return 0;
}

static const struct ast_channel_tech visdn_tech;
static struct ast_channel *visdn_new(
	struct visdn_chan *visdn_chan,
	int state)
{
	struct ast_channel *ast_chan;
	ast_chan = ast_channel_alloc(1);
	if (!ast_chan) {
		ast_log(LOG_WARNING, "Unable to allocate channel\n");
		return NULL;
	}

	ast_chan->fds[0] = open("/dev/visdn/timer", O_RDONLY);
	if (ast_chan->fds[0] < 0) {
		ast_log(LOG_ERROR, "Unable to open timer: %s\n",
			strerror(errno));
		return NULL;
	}

	if (state == AST_STATE_RING)
		ast_chan->rings = 1;

	visdn_chan->ast_chan = ast_chan;

	ast_chan->adsicpe = AST_ADSI_UNAVAILABLE;

//	ast_chan->language[0] = '\0';
//	ast_set_flag(ast_chan, AST_FLAG_DIGITAL);

	ast_chan->nativeformats = AST_FORMAT_ALAW;
	ast_chan->readformat = AST_FORMAT_ALAW;
	ast_chan->writeformat = AST_FORMAT_ALAW;

#ifndef ASTERISK_VERSION_NUM
	ast_chan->pvt->rawreadformat = AST_FORMAT_ALAW;
	ast_chan->pvt->rawwriteformat = AST_FORMAT_ALAW;

	ast_chan->type = VISDN_CHAN_TYPE;

	ast_chan->pvt->call = visdn_call;
	ast_chan->pvt->hangup = visdn_hangup;
	ast_chan->pvt->answer = visdn_answer;
	ast_chan->pvt->read = visdn_read;
	ast_chan->pvt->write = visdn_write;
	ast_chan->pvt->bridge = visdn_bridge;
	ast_chan->pvt->exception = visdn_exception;
	ast_chan->pvt->indicate = visdn_indicate;
	ast_chan->pvt->fixup = visdn_fixup;
	ast_chan->pvt->setoption = visdn_setoption;
	ast_chan->pvt->send_text = visdn_sendtext;
	ast_chan->pvt->transfer = visdn_transfer;
	ast_chan->pvt->send_digit = visdn_send_digit;

	ast_chan->pvt->pvt = visdn_chan;
#else
	ast_chan->rawreadformat = AST_FORMAT_ALAW;
	ast_chan->rawwriteformat = AST_FORMAT_ALAW;

	ast_chan->tech = &visdn_tech;

	ast_chan->tech_pvt = visdn_chan;
#endif

	ast_setstate(ast_chan, state);

	return ast_chan;
}

#ifndef ASTERISK_VERSION_NUM
static struct ast_channel *visdn_request(
	char *type, int format, void *data)
#else
static struct ast_channel *visdn_request(
	const char *type, int format, void *data, int *cause)
#endif
{
	struct visdn_chan *visdn_chan;
	char *dest = NULL;

	if (!(format & AST_FORMAT_ALAW)) {
		ast_log(LOG_NOTICE,
			"Asked to get a channel of unsupported format '%d'\n",
			format);
		return NULL;
	}

	if (data) {
		dest = ast_strdupa((char *)data);
	} else {
		ast_log(LOG_WARNING, "Channel requested with no data\n");
		return NULL;
	}

	visdn_chan = visdn_alloc();
	if (!visdn_chan) {
		ast_log(LOG_ERROR, "Cannot allocate visdn_chan\n");
		return NULL;
	}

	struct ast_channel *ast_chan;
	ast_chan = visdn_new(visdn_chan, AST_STATE_DOWN);

	snprintf(ast_chan->name, sizeof(ast_chan->name), "VISDN/null");

	ast_mutex_lock(&usecnt_lock);
	visdn.usecnt++;
	ast_mutex_unlock(&usecnt_lock);
	ast_update_use_count();

	return ast_chan;
}

#ifdef ASTERISK_VERSION_NUM
static const struct ast_channel_tech visdn_tech = {
	.type		= VISDN_CHAN_TYPE,
	.description	= VISDN_DESCRIPTION,
	.capabilities	= AST_FORMAT_ALAW,
	.requester	= visdn_request,
	.call		= visdn_call,
	.hangup		= visdn_hangup,
	.answer		= visdn_answer,
	.read		= visdn_read,
	.write		= visdn_write,
	.indicate	= visdn_indicate,
	.transfer	= visdn_transfer,
	.fixup		= visdn_fixup,
	.send_digit	= visdn_send_digit,
	.bridge		= visdn_bridge,
	.send_text	= visdn_sendtext,
	.setoption	= visdn_setoption,
};
#endif

// Must be called with visdn.lock acquired
static void refresh_polls_list()
{
	visdn.npolls = 0;

	visdn.polls[visdn.npolls].fd = visdn.q931_ccb_queue_pipe_read;
	visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
	visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_Q931_CCB;
	visdn.poll_infos[visdn.npolls].interface = NULL;
	(visdn.npolls)++;

	visdn.polls[visdn.npolls].fd = visdn.ccb_q931_queue_pipe_read;
	visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
	visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_CCB_Q931;
	visdn.poll_infos[visdn.npolls].interface = NULL;
	(visdn.npolls)++;

	visdn.polls[visdn.npolls].fd = visdn.netlink_socket;
	visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
	visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_NETLINK;
	visdn.poll_infos[visdn.npolls].interface = NULL;
	(visdn.npolls)++;

	visdn.open_pending = FALSE;

	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {
		if (intf->open_pending)
			visdn.open_pending = TRUE;
			visdn.open_pending_nextcheck = 0;

		if (!intf->q931_intf)
			continue;

		if (intf->q931_intf->role == LAPD_ROLE_NT) {
			visdn.polls[visdn.npolls].fd = intf->q931_intf->master_socket;
			visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
			visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_INTERFACE;
			visdn.poll_infos[visdn.npolls].interface = intf->q931_intf;
			(visdn.npolls)++;
		} else {
			visdn.polls[visdn.npolls].fd = intf->q931_intf->dlc.socket;
			visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
			visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_DLC;
			visdn.poll_infos[visdn.npolls].dlc = &intf->q931_intf->dlc;
			(visdn.npolls)++;
		}

		struct q931_dlc *dlc;
		list_for_each_entry(dlc,  &intf->q931_intf->dlcs, intf_node) {
			visdn.polls[visdn.npolls].fd = dlc->socket;
			visdn.polls[visdn.npolls].events = POLLIN | POLLERR;
			visdn.poll_infos[visdn.npolls].type = POLL_INFO_TYPE_DLC;
			visdn.poll_infos[visdn.npolls].dlc = dlc;
			(visdn.npolls)++;
		}
	}
}

// Must be called with visdn.lock acquired
static void visdn_accept(
	struct q931_interface *intf,
	int accept_socket)
{
	struct q931_dlc *newdlc;

	newdlc = q931_accept(intf, accept_socket);
	if (!newdlc)
		return;

	visdn_debug("New DLC (TEI=%d) accepted on interface %s\n",
			newdlc->tei,
			intf->name);

	refresh_polls_list();
}

static int visdn_open_interface(
	struct visdn_interface *intf)
{
	assert(!intf->q931_intf);

	intf->open_pending = TRUE;

	intf->q931_intf = q931_open_interface(visdn.libq931, intf->name, 0);
	if (!intf->q931_intf) {
		ast_log(LOG_WARNING,
			"Cannot open interface %s, skipping\n",
			intf->name);

		return -1;
	}

	intf->q931_intf->pvt = intf;
	intf->q931_intf->network_role = intf->network_role;
	intf->q931_intf->dlc_autorelease_time = intf->dlc_autorelease_time;

	if (intf->T301) intf->q931_intf->T301 = intf->T301 * 1000000LL;
	if (intf->T302) intf->q931_intf->T302 = intf->T302 * 1000000LL;
	if (intf->T303) intf->q931_intf->T303 = intf->T303 * 1000000LL;
	if (intf->T304) intf->q931_intf->T304 = intf->T304 * 1000000LL;
	if (intf->T305) intf->q931_intf->T305 = intf->T305 * 1000000LL;
	if (intf->T306) intf->q931_intf->T306 = intf->T306 * 1000000LL;
	if (intf->T308) intf->q931_intf->T308 = intf->T308 * 1000000LL;
	if (intf->T309) intf->q931_intf->T309 = intf->T309 * 1000000LL;
	if (intf->T310) intf->q931_intf->T310 = intf->T310 * 1000000LL;
	if (intf->T312) intf->q931_intf->T312 = intf->T312 * 1000000LL;
	if (intf->T313) intf->q931_intf->T313 = intf->T313 * 1000000LL;
	if (intf->T314) intf->q931_intf->T314 = intf->T314 * 1000000LL;
	if (intf->T316) intf->q931_intf->T316 = intf->T316 * 1000000LL;
	if (intf->T317) intf->q931_intf->T317 = intf->T317 * 1000000LL;
	if (intf->T318) intf->q931_intf->T318 = intf->T318 * 1000000LL;
	if (intf->T319) intf->q931_intf->T319 = intf->T319 * 1000000LL;
	if (intf->T320) intf->q931_intf->T320 = intf->T320 * 1000000LL;
	if (intf->T321) intf->q931_intf->T321 = intf->T321 * 1000000LL;
	if (intf->T322) intf->q931_intf->T322 = intf->T322 * 1000000LL;

	if (intf->q931_intf->role == LAPD_ROLE_NT) {
		if (listen(intf->q931_intf->master_socket, 100) < 0) {
			ast_log(LOG_ERROR,
				"cannot listen on master socket: %s\n",
				strerror(errno));

			return -1;
		}
	}

	char path[PATH_MAX];
	char goodpath[PATH_MAX];
	snprintf(path, sizeof(path),
		"/sys/class/net/%s/visdn_channel/leg_a/",
		intf->name);

	struct stat st;
	for(;;) {
		if (stat(path, &st) < 0) {
			if (errno == ENOENT)
				break;

			ast_log(LOG_ERROR,
				"cannot stat(%s): %s\n",
				path,
				strerror(errno));

			return -1;
		}

		strcpy(goodpath, path);

		strncat(path, "connected/other_leg/", sizeof(path));
	}

	strncat(goodpath, "../..", sizeof(goodpath));

	if (realpath(goodpath, intf->remote_port) < 0) {
		ast_log(LOG_ERROR,
			"cannot find realpath(%s): %s\n",
			goodpath,
			strerror(errno));
	}

	intf->open_pending = FALSE;

	return 0;
}

// Must be called with visdn.lock acquired
static void visdn_add_interface(const char *name)
{
	int found = FALSE;
	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {
		if (!strcasecmp(intf->name, name)) {
			found = TRUE;
			break;
		}
	}

	if (!found) {
		intf = malloc(sizeof(*intf));

		INIT_LIST_HEAD(&intf->suspended_calls);
		intf->q931_intf = NULL;
		intf->configured = FALSE;
		intf->open_pending = FALSE;
		strncpy(intf->name, name, sizeof(intf->name));
		visdn_copy_interface_config(intf, &visdn.default_intf);

		list_add_tail(&intf->ifs_node, &visdn.ifs);
	}

	if (!intf->q931_intf) {
		visdn_debug("Opening interface %s\n", name);

		visdn_open_interface(intf);
		refresh_polls_list();
	}
}

// Must be called with visdn.lock acquired
static void visdn_rem_interface(const char *name)
{
	struct visdn_interface *intf;
	list_for_each_entry(intf, &visdn.ifs, ifs_node) {
		if (intf->q931_intf &&
		    !strcmp(intf->name, name)) {
			q931_close_interface(intf->q931_intf);

			intf->q931_intf = NULL;

			refresh_polls_list();

			break;
		}
	}
}

#define MAX_PAYLOAD 1024

// Must be called with visdn.lock acquired
static void visdn_netlink_receive()
{
	struct sockaddr_nl tonl;
	tonl.nl_family = AF_NETLINK;
	tonl.nl_pid = 0;
	tonl.nl_groups = 0;

	struct msghdr skmsg;
	struct sockaddr_nl dest_addr;
	struct cmsghdr cmsg;
	struct iovec iov;

	__u8 data[NLMSG_SPACE(MAX_PAYLOAD)];

	struct nlmsghdr *hdr = (struct nlmsghdr *)data;

	iov.iov_base = data;
	iov.iov_len = sizeof(data);

	skmsg.msg_name = &dest_addr;
	skmsg.msg_namelen = sizeof(dest_addr);
	skmsg.msg_iov = &iov;
	skmsg.msg_iovlen = 1;
	skmsg.msg_control = &cmsg;
	skmsg.msg_controllen = sizeof(cmsg);
	skmsg.msg_flags = 0;

	if(recvmsg(visdn.netlink_socket, &skmsg, 0) < 0) {
		ast_log(LOG_WARNING, "recvmsg: %s\n", strerror(errno));
		return;
	}

	// Implement multipart messages FIXME FIXME TODO

	if (hdr->nlmsg_type == RTM_NEWLINK) {
		struct ifinfomsg *ifi = NLMSG_DATA(hdr);

		if (ifi->ifi_type == ARPHRD_LAPD) {

			char ifname[IFNAMSIZ] = "";
			int len = hdr->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifinfomsg));

			struct rtattr *rtattr;
			for (rtattr = IFLA_RTA(ifi);
			     RTA_OK(rtattr, len);
			     rtattr = RTA_NEXT(rtattr, len)) {

				if (rtattr->rta_type == IFLA_IFNAME) {
					strncpy(ifname,
						RTA_DATA(rtattr),
						sizeof(ifname));
				}
			}

			if (ifi->ifi_flags & IFF_UP) {
				visdn_debug("Netlink msg: %s UP %s\n",
						ifname,
						(ifi->ifi_flags & IFF_ALLMULTI) ? "NT": "TE");

				visdn_add_interface(ifname);
			} else {
				visdn_debug("Netlink msg: %s DOWN %s\n",
						ifname,
						(ifi->ifi_flags & IFF_ALLMULTI) ? "NT": "TE");

				visdn_rem_interface(ifname);
			}
		}
	}
}

static void visdn_ccb_q931_receive()
{
	struct q931_ccb_message *msg;

	while(1) {
		ast_mutex_lock(&visdn.ccb_q931_queue_lock);

		msg = list_entry(visdn.ccb_q931_queue.next,
				struct q931_ccb_message, node);
		if (&msg->node == &visdn.ccb_q931_queue) {
			ast_mutex_unlock(&visdn.ccb_q931_queue_lock);
			break;
		}

		char buf[1];
		read(visdn.ccb_q931_queue_pipe_read, buf, 1);

		list_del_init(&msg->node);
		ast_mutex_unlock(&visdn.ccb_q931_queue_lock);

		q931_ccb_dispatch(msg);
	}
}

static void visdn_q931_ccb_receive();

static int visdn_q931_thread_do_poll()
{
	longtime_t usec_to_wait = q931_run_timers(visdn.libq931);
	int msec_to_wait;

	if (usec_to_wait < 0) {
		msec_to_wait = -1;
	} else {
		msec_to_wait = usec_to_wait / 1000 + 1;
	}

	if (visdn.open_pending)
		msec_to_wait = (msec_to_wait > 0 && msec_to_wait < 2001) ?
				msec_to_wait : 2001;

	visdn_debug("select timeout = %d\n", msec_to_wait);

	// Uhm... we should lock, copy polls and unlock before poll()
	if (poll(visdn.polls, visdn.npolls, msec_to_wait) < 0) {
		if (errno == EINTR)
			return TRUE;

		ast_log(LOG_WARNING, "poll error: %s\n", strerror(errno));
		exit(1);
	}

	ast_mutex_lock(&visdn.lock);
	if (time(NULL) > visdn.open_pending_nextcheck) {

		struct visdn_interface *intf;
		list_for_each_entry(intf, &visdn.ifs, ifs_node) {

			if (intf->open_pending) {
				visdn_debug("Retry opening interface %s\n",
						intf->name);

				if (visdn_open_interface(intf) < 0)
					visdn.open_pending_nextcheck =
							time(NULL) + 2;
			}
		}

		refresh_polls_list();
	}
	ast_mutex_unlock(&visdn.lock);

	int i;
	for(i = 0; i < visdn.npolls; i++) {
		if (visdn.poll_infos[i].type == POLL_INFO_TYPE_NETLINK) {
			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {
				visdn_netlink_receive();
				break; // polls list may have been changed
			}
		} else if (visdn.poll_infos[i].type == POLL_INFO_TYPE_Q931_CCB) {
			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {
				visdn_q931_ccb_receive();
			}
		} else if (visdn.poll_infos[i].type == POLL_INFO_TYPE_CCB_Q931) {
			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {
				visdn_ccb_q931_receive();
			}
		} else if (visdn.poll_infos[i].type == POLL_INFO_TYPE_INTERFACE) {
			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {
				ast_mutex_lock(&visdn.lock);
				visdn_accept(
					visdn.poll_infos[i].interface,
					visdn.polls[i].fd);
				ast_mutex_unlock(&visdn.lock);
				break; // polls list may have been changed
			}
		} else if (visdn.poll_infos[i].type == POLL_INFO_TYPE_DLC) {
			if (visdn.polls[i].revents &
					(POLLIN | POLLPRI | POLLERR |
					 POLLHUP | POLLNVAL)) {

				int err;
				ast_mutex_lock(&visdn.lock);
				err = q931_receive(visdn.poll_infos[i].dlc);

				if (err == Q931_RECEIVE_REFRESH) {
					refresh_polls_list();
					ast_mutex_unlock(&visdn.lock);

					break;
				}
				ast_mutex_unlock(&visdn.lock);
			}
		}
	}

	ast_mutex_lock(&visdn.lock);
	int active_calls_cnt = 0;
	if (visdn.have_to_exit) {
		active_calls_cnt = 0;

		struct visdn_interface *intf;
		list_for_each_entry(intf, &visdn.ifs, ifs_node) {
			if (intf->q931_intf) {
				struct q931_call *call;
				list_for_each_entry(call,
						&intf->q931_intf->calls,
						calls_node)
					active_calls_cnt++;
			}
		}

		ast_log(LOG_WARNING,
			"There are still %d active calls, waiting...\n",
			active_calls_cnt);
	}
	ast_mutex_unlock(&visdn.lock);

	return (!visdn.have_to_exit || active_calls_cnt > 0);
}


static void *visdn_q931_thread_main(void *data)
{
	ast_mutex_lock(&visdn.lock);

	visdn.npolls = 0;
	refresh_polls_list();

	visdn.have_to_exit = 0;

	ast_mutex_unlock(&visdn.lock);

	while(visdn_q931_thread_do_poll());

	return NULL;
}

#ifdef DEBUG_CODE
#define FUNC_DEBUG()					\
	if (visdn.debug)				\
		ast_verbose(VERBOSE_PREFIX_3		\
			"%s\n", __FUNCTION__);
#else
#define FUNC_DEBUG() do {} while(0)
#endif

static void visdn_q931_alerting_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_queue_control(ast_chan, AST_CONTROL_RINGING);
	ast_setstate(ast_chan, AST_STATE_RINGING);
}

static void visdn_q931_connect_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	q931_send_primitive(q931_call, Q931_CCB_SETUP_COMPLETE_REQUEST, NULL);

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_queue_control(ast_chan, AST_CONTROL_ANSWER);
}

static void visdn_q931_disconnect_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	q931_send_primitive(q931_call, Q931_CCB_RELEASE_REQUEST, NULL);
}

static void visdn_q931_error_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();
}

static void visdn_q931_info_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	if (q931_call->state != U2_OVERLAP_SENDING &&
	    q931_call->state != N2_OVERLAP_SENDING) {
		ast_log(LOG_WARNING, "Received info not in overlap sending\n");
		return;
	}

	struct q931_ie_called_party_number *cdpn = NULL;

	int i;
	for(i=0; i<ies->count; i++) {
		if (ies->ies[i]->type->id == Q931_IE_SENDING_COMPLETE) {
			visdn_chan->sending_complete = TRUE;
		} else if (ies->ies[i]->type->id ==
					 Q931_IE_CALLED_PARTY_NUMBER) {
			cdpn = container_of(ies->ies[i],
				struct q931_ie_called_party_number, ie);
		}
	}

	if (!cdpn)
		return;

	for(i=0; cdpn->number[i]; i++) {
		struct ast_frame f =
			{ AST_FRAME_DTMF, cdpn->number[i] };
		ast_queue_frame(ast_chan, &f);
	}
}

static void visdn_q931_more_info_indication(
	struct q931_call *q931_call,
	const struct q931_ies *user_ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	struct visdn_interface *intf = q931_call->intf->pvt;

	visdn_chan->may_send_digits = TRUE;

	if (strlen(visdn_chan->queued_digits)) {
		visdn_debug("more-info-indication received, flushing"
			" digits queue\n");

		struct q931_ies ies = Q931_IES_INIT;

		struct q931_ie_called_party_number *cdpn =
			q931_ie_called_party_number_alloc();
		cdpn->type_of_number = visdn_type_of_number_to_cdpn(
							intf->type_of_number);
		cdpn->numbering_plan_identificator =
			Q931_IE_CDPN_NPI_ISDN_TELEPHONY;

		strcpy(cdpn->number, visdn_chan->queued_digits);
		q931_ies_add_put(&ies, &cdpn->ie);

		q931_send_primitive(visdn_chan->q931_call,
			Q931_CCB_INFO_REQUEST, &ies);
	}

	ast_mutex_unlock(&ast_chan->lock);
}

static void visdn_q931_notify_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();
}

static void visdn_q931_proceeding_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	visdn_chan->may_send_digits = TRUE;
	ast_mutex_unlock(&ast_chan->lock);

	ast_queue_control(ast_chan, AST_CONTROL_PROCEEDING);
}

static void visdn_q931_progress_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_queue_control(ast_chan, AST_CONTROL_PROGRESS);
}

static void visdn_q931_reject_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_softhangup(ast_chan, AST_SOFTHANGUP_DEV);
}

static void visdn_q931_release_confirm(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_release_confirm_status status)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_softhangup(ast_chan, AST_SOFTHANGUP_DEV);
}

static void visdn_q931_release_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_softhangup(ast_chan, AST_SOFTHANGUP_DEV);
}

static void visdn_q931_resume_confirm(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_resume_confirm_status status)
{
	FUNC_DEBUG();
}

static void visdn_q931_resume_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	enum q931_ie_cause_value cause;

	if (callpvt_to_astchan(q931_call)) {
		ast_log(LOG_WARNING, "Unexpexted ast_chan\n");
		cause = Q931_IE_C_CV_RESOURCES_UNAVAILABLE;
		goto err_ast_chan;
	}

	struct q931_ie_call_identity *ci = NULL;

	int i;
	for (i=0; i<ies->count; i++) {
		if (ies->ies[i]->type->id == Q931_IE_CALL_IDENTITY) {
			ci = container_of(ies->ies[i],
				struct q931_ie_call_identity, ie);
		}
	}

	struct visdn_interface *intf = q931_call->intf->pvt;
	struct visdn_suspended_call *suspended_call;

	int found = FALSE;
	list_for_each_entry(suspended_call, &intf->suspended_calls, node) {
		if ((!ci && suspended_call->call_identity_len == 0) ||
		    (suspended_call->call_identity_len == ci->data_len &&
		     !memcmp(suspended_call->call_identity, ci->data,
					ci->data_len))) {

			found = TRUE;

			break;
		}
	}

	if (!found) {
		ast_log(LOG_NOTICE, "Unable to find suspended call\n");

		if (list_empty(&intf->suspended_calls))
			cause = Q931_IE_C_CV_SUSPENDED_CALL_EXISTS_BUT_NOT_THIS;
		else
			cause = Q931_IE_C_CV_NO_CALL_SUSPENDED;

		goto err_call_not_found;
	}

	assert(suspended_call->ast_chan);

	struct visdn_chan *visdn_chan = to_visdn_chan(suspended_call->ast_chan);

	q931_call->pvt = suspended_call->ast_chan;
	visdn_chan->q931_call = q931_call;
	visdn_chan->suspended_call = NULL;

	if (!strcmp(suspended_call->ast_chan->_bridge->type, VISDN_CHAN_TYPE)) {
		// Wow, the remote channel is ISDN too, let's notify it!

		struct q931_ies response_ies = Q931_IES_INIT;

		struct visdn_chan *remote_visdn_chan =
			to_visdn_chan(suspended_call->ast_chan->_bridge);

		struct q931_call *remote_call = remote_visdn_chan->q931_call;

		struct q931_ie_notification_indicator *notify =
			q931_ie_notification_indicator_alloc();
		notify->description = Q931_IE_NI_D_USER_RESUMED;
		q931_ies_add_put(&response_ies, &notify->ie);

		q931_send_primitive(remote_call,
			Q931_CCB_NOTIFY_REQUEST, &response_ies);
	}

	ast_moh_stop(suspended_call->ast_chan->_bridge);

	// FIXME: Transform suspended_call->q931_chan to IE and pass it
	assert(0);
	q931_send_primitive(q931_call, Q931_CCB_RESUME_RESPONSE, NULL);

	list_del(&suspended_call->node);
	free(suspended_call);

	return;

err_call_not_found:
err_ast_chan:
	;
	struct q931_ies resp_ies = Q931_IES_INIT;
	struct q931_ie_cause *c = q931_ie_cause_alloc();
	c->coding_standard = Q931_IE_C_CS_CCITT;
	c->location = q931_ie_cause_location_call(q931_call);
	c->value = cause;
	q931_ies_add_put(&resp_ies, &c->ie);

	q931_send_primitive(q931_call,
		Q931_CCB_RESUME_REJECT_REQUEST, &resp_ies);

	return;
}

static void visdn_q931_setup_complete_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_setup_complete_indication_status status)
{
	FUNC_DEBUG();
}

static void visdn_q931_setup_confirm(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_setup_confirm_status status)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);

	if (!ast_chan)
		return;

	ast_setstate(ast_chan, AST_STATE_UP);
}

static void visdn_q931_setup_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	char called_number[32] = "";

	struct visdn_chan *visdn_chan;
	visdn_chan = visdn_alloc();
	if (!visdn_chan) {
		ast_log(LOG_ERROR, "Cannot allocate visdn_chan\n");
		goto err_visdn_alloc;
	}

	visdn_chan->q931_call = q931_call_get(q931_call);

	struct visdn_interface *intf = q931_call->intf->pvt;

	int i;
	for(i=0; i<ies->count; i++) {
		if (ies->ies[i]->type->id == Q931_IE_SENDING_COMPLETE) {
			visdn_chan->sending_complete = TRUE;
		} else if (ies->ies[i]->type->id ==
				Q931_IE_CALLED_PARTY_NUMBER) {

			struct q931_ie_called_party_number *cdpn =
				container_of(ies->ies[i],
					struct q931_ie_called_party_number, ie);

			if (strlen(called_number) + strlen(cdpn->number) - 1 >
					sizeof(called_number)) {
				ast_log(LOG_NOTICE,
					"Called number overflow\n");

				struct q931_ies ies = Q931_IES_INIT;

				struct q931_ie_cause *cause =
						q931_ie_cause_alloc();
				cause->coding_standard = Q931_IE_C_CS_CCITT;
				cause->location =
					q931_ie_cause_location_call(q931_call);
				cause->value =
					Q931_IE_C_CV_INVALID_NUMBER_FORMAT;
				q931_ies_add_put(&ies, &cause->ie);

				q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_REJECT_REQUEST, &ies);
			}

			if (cdpn->number[strlen(cdpn->number) - 1] == '#') {
				visdn_chan->sending_complete = TRUE;
				strncat(called_number, cdpn->number,
					strlen(cdpn->number)-1);
			} else {
				strcat(called_number, cdpn->number);
			}

		} else if (ies->ies[i]->type->id ==
				Q931_IE_CALLING_PARTY_NUMBER) {

			struct q931_ie_calling_party_number *cgpn =
				container_of(ies->ies[i],
				struct q931_ie_calling_party_number, ie);

			const char *prefix = "";
			if (cgpn->type_of_number ==
					Q931_IE_CDPN_TON_NATIONAL)
				prefix = intf->national_prefix;
			else if (cgpn->type_of_number ==
					Q931_IE_CDPN_TON_INTERNATIONAL)
				prefix = intf->international_prefix;

			snprintf(visdn_chan->calling_number,
				sizeof(visdn_chan->calling_number),
				"<%s%s>", prefix, cgpn->number);

		} else if (ies->ies[i]->type->id == Q931_IE_BEARER_CAPABILITY) {

			// We should check the destination bearer capability
			// unfortunately we don't know if the destination is
			// compatible until we start the PBX... this is a
			// design flaw in Asterisk

			struct q931_ie_bearer_capability *bc =
				container_of(ies->ies[i],
					struct q931_ie_bearer_capability, ie);

			if (bc->information_transfer_capability ==
				Q931_IE_BC_ITC_UNRESTRICTED_DIGITAL) {

				visdn_chan->is_voice = FALSE;
				q931_call->tones_option = FALSE;

			} else  if (bc->information_transfer_capability ==
					Q931_IE_BC_ITC_SPEECH ||
				    bc->information_transfer_capability ==
					Q931_IE_BC_ITC_3_1_KHZ_AUDIO) {

				visdn_chan->is_voice = TRUE;
				q931_call->tones_option = intf->tones_option;
			} else {
				struct q931_ies ies = Q931_IES_INIT;

				struct q931_ie_cause *cause =
					q931_ie_cause_alloc();
				cause->coding_standard = Q931_IE_C_CS_CCITT;
				cause->location =
					q931_ie_cause_location_call(q931_call);
				cause->value =
					Q931_IE_C_CV_BEARER_CAPABILITY_NOT_IMPLEMENTED;
				q931_ies_add_put(&ies, &cause->ie);

				q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_REJECT_REQUEST, &ies);

				return;
			}
		}
	}

	struct ast_channel *ast_chan;
	ast_chan = visdn_new(visdn_chan, AST_STATE_OFFHOOK);
	if (!ast_chan)
		goto err_visdn_new;

	q931_call->pvt = ast_chan;

	snprintf(ast_chan->name, sizeof(ast_chan->name), "VISDN/%s/%c%ld",
		q931_call->intf->name,
		q931_call->direction == Q931_CALL_DIRECTION_INBOUND ? 'I' : 'O',
		q931_call->call_reference);

	strncpy(ast_chan->context,
		intf->context,
		sizeof(ast_chan->context)-1);

	ast_mutex_lock(&usecnt_lock);
	visdn.usecnt++;
	ast_mutex_unlock(&usecnt_lock);
	ast_update_use_count();

	if (strlen(visdn_chan->calling_number) &&
	    !intf->force_inbound_caller_id)
		ast_chan->caller_id_number =
			strdup(visdn_chan->calling_number);
	else
		ast_chan->caller_id_number =
			strdup(intf->default_inbound_caller_id);

	if (!intf->overlap_sending ||
	    visdn_chan->sending_complete) {
		if (ast_exists_extension(NULL, intf->context,
				called_number, 1,
				visdn_chan->calling_number)) {

			strncpy(ast_chan->exten,
				called_number,
				sizeof(ast_chan->exten)-1);

			ast_setstate(ast_chan, AST_STATE_RING);

			if (ast_pbx_start(ast_chan)) {
				ast_log(LOG_ERROR,
					"Unable to start PBX on %s\n",
					ast_chan->name);
				ast_hangup(ast_chan);

				struct q931_ies ies = Q931_IES_INIT;

				struct q931_ie_cause *cause =
					q931_ie_cause_alloc();
				cause->coding_standard = Q931_IE_C_CS_CCITT;
				cause->location =
					q931_ie_cause_location_call(q931_call);
				cause->value =
					Q931_IE_C_CV_DESTINATION_OUT_OF_ORDER;

				q931_ies_add_put(&ies, &cause->ie);

				q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_REJECT_REQUEST, &ies);
			} else {
				q931_send_primitive(visdn_chan->q931_call,
					Q931_CCB_PROCEEDING_REQUEST, NULL);

				ast_setstate(ast_chan, AST_STATE_RING);
			}
		} else {
			ast_log(LOG_NOTICE,
				"No extension '%s' in context '%s',"
				" rejecting call\n",
				called_number,
				intf->context);

			struct q931_ies ies = Q931_IES_INIT;

			struct q931_ie_cause *cause = q931_ie_cause_alloc();
			cause->coding_standard = Q931_IE_C_CS_CCITT;
			cause->location =
				q931_ie_cause_location_call(q931_call);

			cause->value = Q931_IE_C_CV_NO_ROUTE_TO_DESTINATION;
			q931_ies_add_put(&ies, &cause->ie);

			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_REJECT_REQUEST, &ies);

			ast_hangup(ast_chan);
		}
	} else {

		strncpy(ast_chan->exten, "s",
			sizeof(ast_chan->exten)-1);

		if (ast_pbx_start(ast_chan)) {
			ast_log(LOG_ERROR,
				"Unable to start PBX on %s\n",
				ast_chan->name);
			ast_hangup(ast_chan);

			struct q931_ies ies_proc = Q931_IES_INIT;
			struct q931_ie_cause *cause = q931_ie_cause_alloc();
			cause->coding_standard = Q931_IE_C_CS_CCITT;
			cause->location =
				q931_ie_cause_location_call(q931_call);

			cause->value = Q931_IE_C_CV_DESTINATION_OUT_OF_ORDER;
			q931_ies_add_put(&ies_proc, &cause->ie);

			struct q931_ies ies_disc = Q931_IES_INIT;
			if (visdn_chan->is_voice) {
				struct q931_ie_progress_indicator *pi =
					q931_ie_progress_indicator_alloc();
				pi->coding_standard = Q931_IE_PI_CS_CCITT;
				pi->location =
					q931_ie_progress_indicator_location(
							visdn_chan->q931_call);
				pi->progress_description =
					Q931_IE_PI_PD_IN_BAND_INFORMATION;
				q931_ies_add_put(&ies_disc, &pi->ie);
			}

			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_PROCEEDING_REQUEST, &ies_proc);
			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_DISCONNECT_REQUEST, &ies_disc);
		} else {
			q931_send_primitive(visdn_chan->q931_call,
				Q931_CCB_MORE_INFO_REQUEST, NULL);
		}

		for(i=0; called_number[i]; i++) {
			struct ast_frame f =
				{ AST_FRAME_DTMF, called_number[i] };
			ast_queue_frame(ast_chan, &f);
		}
	}

	return;

err_visdn_new:
	// Free visdn_chan
err_visdn_alloc:
;
}

static void visdn_q931_status_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_status_indication_status status)
{
	FUNC_DEBUG();
}

static void visdn_q931_suspend_confirm(
	struct q931_call *q931_call,
	const struct q931_ies *ies,
	enum q931_suspend_confirm_status status)
{
	FUNC_DEBUG();
}

struct visdn_dual {
	struct ast_channel *chan1;
	struct ast_channel *chan2;
};

static void visdn_q931_suspend_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(q931_call);
	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	enum q931_ie_cause_value cause;

	if (!ast_chan) {
		ast_log(LOG_WARNING, "Unexpexted ast_chan\n");
		cause = Q931_IE_C_CV_RESOURCES_UNAVAILABLE;
		goto err_ast_chan;
	}

	struct q931_ie_call_identity *ci = NULL;

	int i;
	for (i=0; i<ies->count; i++) {
		if (ies->ies[i]->type->id == Q931_IE_CALL_IDENTITY) {
			ci = container_of(ies->ies[i],
				struct q931_ie_call_identity, ie);
		}
	}

	struct visdn_interface *intf = q931_call->intf->pvt;
	struct visdn_suspended_call *suspended_call;
	list_for_each_entry(suspended_call, &intf->suspended_calls, node) {
		if ((!ci && suspended_call->call_identity_len == 0) ||
		    (ci && suspended_call->call_identity_len == ci->data_len &&
		     !memcmp(suspended_call->call_identity,
					ci->data, ci->data_len))) {

			cause = Q931_IE_C_CV_CALL_IDENITY_IN_USE;

			goto err_call_identity_in_use;
		}
	}

	suspended_call = malloc(sizeof(*suspended_call));
	if (!suspended_call) {
		cause = Q931_IE_C_CV_RESOURCES_UNAVAILABLE;
		goto err_suspend_alloc;
	}

	suspended_call->ast_chan = ast_chan;
	suspended_call->q931_chan = q931_call->channel;

	if (ci) {
		suspended_call->call_identity_len = ci->data_len;
		memcpy(suspended_call->call_identity, ci->data, ci->data_len);
	} else {
		suspended_call->call_identity_len = 0;
	}

	suspended_call->old_when_to_hangup = ast_chan->whentohangup;

	list_add_tail(&suspended_call->node, &intf->suspended_calls);

	q931_send_primitive(q931_call, Q931_CCB_SUSPEND_RESPONSE, NULL);

	assert(ast_chan->_bridge);

	ast_moh_start(ast_chan->_bridge, NULL);

	if (!strcmp(ast_chan->_bridge->type, VISDN_CHAN_TYPE)) {
		// Wow, the remote channel is ISDN too, let's notify it!

		struct q931_ies response_ies = Q931_IES_INIT;

		struct visdn_chan *remote_visdn_chan =
					to_visdn_chan(ast_chan->_bridge);

		struct q931_call *remote_call = remote_visdn_chan->q931_call;

		struct q931_ie_notification_indicator *notify =
			q931_ie_notification_indicator_alloc();
		notify->description = Q931_IE_NI_D_USER_SUSPENDED;
		q931_ies_add_put(&response_ies, &notify->ie);

		q931_send_primitive(remote_call,
			Q931_CCB_NOTIFY_REQUEST, &response_ies);
	}

	if (!ast_chan->whentohangup ||
	    time(NULL) + 45 < ast_chan->whentohangup)
		ast_channel_setwhentohangup(ast_chan, intf->T307);

	q931_call->pvt = NULL;
	visdn_chan->q931_call = NULL;
	visdn_chan->suspended_call = suspended_call;
	q931_call_put(q931_call);

	return;

err_suspend_alloc:
err_call_identity_in_use:
err_ast_chan:
	;
	struct q931_ies resp_ies = Q931_IES_INIT;
	struct q931_ie_cause *c = q931_ie_cause_alloc();
	c->coding_standard = Q931_IE_C_CS_CCITT;
	c->location = q931_ie_cause_location_call(q931_call);
	c->value = cause;
	q931_ies_add_put(&resp_ies, &c->ie);

	q931_send_primitive(visdn_chan->q931_call,
		Q931_CCB_SUSPEND_REJECT_REQUEST, &resp_ies);

	return;
}

static void visdn_q931_timeout_indication(
	struct q931_call *q931_call,
	const struct q931_ies *ies)
{
	FUNC_DEBUG();
}

static void visdn_q931_connect_channel(
	struct q931_channel *channel)
{
	FUNC_DEBUG();

	assert(channel->call);
	struct ast_channel *ast_chan = callpvt_to_astchan(channel->call);

	if (!ast_chan)
		return;

	ast_mutex_lock(&ast_chan->lock);

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);
	struct visdn_interface *visdn_intf = channel->intf->pvt;

	char path[100], dest[100];
	snprintf(path, sizeof(path),
		"%s/B%d",
		visdn_intf->remote_port,
		channel->id+1);

	memset(dest, 0, sizeof(dest));
	if (readlink(path, dest, sizeof(dest) - 1) < 0) {
		ast_log(LOG_ERROR, "readlink(%s): %s\n", path, strerror(errno));
		goto err_readlink;
	}

	char *chanid = strrchr(dest, '/');
	if (!chanid || !strlen(chanid + 1)) {
		ast_log(LOG_ERROR,
			"Invalid chanid found in symlink %s\n",
			dest);
		goto err_invalid_chanid;
	}

	visdn_chan->visdn_chan_id = atoi(chanid + 1);

	if (visdn_chan->is_voice) {
		visdn_debug("Connecting streamport to chan '%06d'\n",
				visdn_chan->visdn_chan_id);

		visdn_chan->channel_fd = open("/dev/visdn/streamport", O_RDWR);
		if (visdn_chan->channel_fd < 0) {
			ast_log(LOG_ERROR,
				"Cannot open streamport: %s\n",
				strerror(errno));
			goto err_open;
		}

		struct visdn_connect vc;
		vc.src_chan_id = 0;
		vc.dst_chan_id = visdn_chan->visdn_chan_id;
		vc.flags = 0;

		if (ioctl(visdn_chan->channel_fd, VISDN_IOC_CONNECT_PATH,
		    (caddr_t) &vc) < 0) {
			ast_log(LOG_ERROR,
				"ioctl(VISDN_ENABLE_PATH): %s\n",
				strerror(errno));
			goto err_ioctl;
		}

		if (ioctl(visdn_chan->channel_fd, VISDN_IOC_ENABLE_PATH,
							 NULL) < 0) {
			ast_log(LOG_ERROR,
				"ioctl(VISDN_ENABLE_PATH): %s\n",
				strerror(errno));
			goto err_ioctl;
		}
	}

	ast_mutex_unlock(&ast_chan->lock);

	return;

err_ioctl:
err_open:
err_invalid_chanid:
err_readlink:

	ast_mutex_unlock(&ast_chan->lock);
}

static void visdn_q931_disconnect_channel(
	struct q931_channel *channel)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(channel->call);

	if (!ast_chan)
		return;

	struct visdn_chan *visdn_chan = to_visdn_chan(ast_chan);

	ast_mutex_lock(&ast_chan->lock);

	if (visdn_chan->channel_fd >= 0) {
		if (ioctl(visdn_chan->channel_fd,
				VISDN_IOC_DISCONNECT_PATH, NULL) < 0) {
			ast_log(LOG_ERROR,
				"ioctl(VISDN_IOC_DISCONNECT): %s\n",
				strerror(errno));
		}

		if (close(visdn_chan->channel_fd) < 0) {
			ast_log(LOG_ERROR,
				"close(visdn_chan->channel_fd): %s\n",
				strerror(errno));
		}

		visdn_chan->channel_fd = -1;
	}

	ast_mutex_unlock(&ast_chan->lock);
}

static void visdn_q931_start_tone(struct q931_channel *channel,
	enum q931_tone_type tone)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(channel->call);

	// Unfortunately, after ast_hangup the channel is not valid
	// anymore and we cannot generate further tones thought we should
	if (!ast_chan)
		return;

	switch (tone) {
	case Q931_TONE_DIAL:
		ast_indicate(ast_chan, AST_CONTROL_OFFHOOK);
	break;

	case Q931_TONE_HANGUP:
		ast_indicate(ast_chan, AST_CONTROL_HANGUP);
	break;

	case Q931_TONE_BUSY:
		ast_indicate(ast_chan, AST_CONTROL_BUSY);
	break;

	case Q931_TONE_FAILURE:
		ast_indicate(ast_chan, AST_CONTROL_CONGESTION);
	break;
	default:;
	}
}

static void visdn_q931_stop_tone(struct q931_channel *channel)
{
	FUNC_DEBUG();

	struct ast_channel *ast_chan = callpvt_to_astchan(channel->call);

	if (!ast_chan)
		return;

	ast_indicate(ast_chan, -1);
}

static void visdn_q931_management_restart_confirm(
	struct q931_global_call *gc,
	const struct q931_chanset *chanset)
{
	FUNC_DEBUG();
}

static void visdn_q931_timeout_management_indication(
	struct q931_global_call *gc)
{
	FUNC_DEBUG();
}

static void visdn_q931_status_management_indication(
	struct q931_global_call *gc)
{
	FUNC_DEBUG();
}

static void visdn_logger(int level, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	char msg[200];
	vsnprintf(msg, sizeof(msg), format, ap);
	va_end(ap);

	switch(level) {
	case Q931_LOG_DEBUG:
		if (visdn.debug_q931)
			ast_verbose(VERBOSE_PREFIX_3 "%s", msg);
	break;

	case Q931_LOG_INFO:
		ast_verbose(VERBOSE_PREFIX_3  "%s", msg);
	break;

	case Q931_LOG_NOTICE:
		ast_log(__LOG_NOTICE, "libq931", 0, "", "%s", msg);
	break;

	case Q931_LOG_WARNING:
		ast_log(__LOG_WARNING, "libq931", 0, "", "%s", msg);
	break;

	case Q931_LOG_ERR:
	case Q931_LOG_CRIT:
	case Q931_LOG_ALERT:
	case Q931_LOG_EMERG:
		ast_log(__LOG_ERROR, "libq931", 0, "", "%s", msg);
	break;
	}
}

void visdn_q931_timer_update(struct q931_lib *lib)
{
	pthread_kill(visdn_q931_thread, SIGURG);
}

static void visdn_q931_ccb_receive()
{
	struct q931_ccb_message *msg;

	while(1) {
		ast_mutex_lock(&visdn.q931_ccb_queue_lock);

		msg = list_entry(visdn.q931_ccb_queue.next,
				struct q931_ccb_message, node);
		if (&msg->node == &visdn.q931_ccb_queue) {
			ast_mutex_unlock(&visdn.q931_ccb_queue_lock);
			break;
		}

		char buf[1];
		read(visdn.q931_ccb_queue_pipe_read, buf, 1);

		list_del_init(&msg->node);
		ast_mutex_unlock(&visdn.q931_ccb_queue_lock);

		switch (msg->primitive) {
		case Q931_CCB_ALERTING_INDICATION:
			visdn_q931_alerting_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_CONNECT_INDICATION:
			visdn_q931_connect_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_DISCONNECT_INDICATION:
			visdn_q931_disconnect_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_ERROR_INDICATION:
			visdn_q931_error_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_INFO_INDICATION:
			visdn_q931_info_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_MORE_INFO_INDICATION:
			visdn_q931_more_info_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_NOTIFY_INDICATION:
			visdn_q931_notify_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_PROCEEDING_INDICATION:
			visdn_q931_proceeding_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_PROGRESS_INDICATION:
			visdn_q931_progress_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_REJECT_INDICATION:
			visdn_q931_reject_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_RELEASE_CONFIRM:
			visdn_q931_release_confirm(msg->call, &msg->ies,
								msg->par1);
		break;

		case Q931_CCB_RELEASE_INDICATION:
			visdn_q931_release_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_RESUME_CONFIRM:
			visdn_q931_resume_confirm(msg->call, &msg->ies,
							msg->par1);
		break;

		case Q931_CCB_RESUME_INDICATION:
			visdn_q931_resume_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_SETUP_COMPLETE_INDICATION:
			visdn_q931_setup_complete_indication(msg->call,
						&msg->ies, msg->par1);
		break;

		case Q931_CCB_SETUP_CONFIRM:
			visdn_q931_setup_confirm(msg->call, &msg->ies, msg->par1);
		break;

		case Q931_CCB_SETUP_INDICATION:
			visdn_q931_setup_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_STATUS_INDICATION:
			visdn_q931_status_indication(msg->call, &msg->ies,
						msg->par1);
		break;

		case Q931_CCB_SUSPEND_CONFIRM:
			visdn_q931_suspend_confirm(msg->call, &msg->ies,
						msg->par1);
		break;

		case Q931_CCB_SUSPEND_INDICATION:
			visdn_q931_suspend_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_TIMEOUT_INDICATION:
			visdn_q931_timeout_indication(msg->call, &msg->ies);
		break;

		case Q931_CCB_TIMEOUT_MANAGEMENT_INDICATION:
			visdn_q931_timeout_management_indication(
				(struct q931_global_call *)msg->par1);
		break;

		case Q931_CCB_STATUS_MANAGEMENT_INDICATION:
			visdn_q931_status_management_indication(
				(struct q931_global_call *)msg->par1);
		break;

		case Q931_CCB_MANAGEMENT_RESTART_CONFIRM:
			visdn_q931_management_restart_confirm(
				(struct q931_global_call *)msg->par1,
				(struct q931_chanset *)msg->par2);
		break;

		case Q931_CCB_CONNECT_CHANNEL:
			visdn_q931_connect_channel(
				(struct q931_channel *)msg->par1);
		break;

		case Q931_CCB_DISCONNECT_CHANNEL:
			visdn_q931_disconnect_channel(
				(struct q931_channel *)msg->par1);
		break;

		case Q931_CCB_START_TONE:
			visdn_q931_start_tone(
				(struct q931_channel *)msg->par1, msg->par2);
		break;

		case Q931_CCB_STOP_TONE:
			visdn_q931_stop_tone((struct q931_channel *)msg->par1);
		break;

		default:
			ast_log(LOG_WARNING, "Unexpected primitive %d\n",
				msg->primitive);
		}
	}
}

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int visdn_exec_overlap_dial(struct ast_channel *chan, void *data)
{
	struct localuser *u;
	LOCAL_USER_ADD(u);

	char called_number[64] = "";

	while(ast_waitfor(chan, -1) > -1) {
		struct ast_frame *f;
		f = ast_read(chan);
		if (!f)
			break;

		if (f->frametype == AST_FRAME_DTMF) {
			ast_setstate(chan, AST_STATE_DIALING);

			if(strlen(called_number) >= sizeof(called_number)-1)
				break;

			called_number[strlen(called_number)] = f->subclass;

			if (!ast_canmatch_extension(NULL,
					chan->context,
					called_number, 1,
					chan->caller_id_number)) {

				ast_indicate(chan, AST_CONTROL_CONGESTION);
				ast_safe_sleep(chan, 30000);
				return -1;
			}

			if (ast_exists_extension(NULL,
					chan->context,
					called_number, 1,
					chan->caller_id_number)) {

				if (!ast_matchmore_extension(NULL,
					chan->context,
					called_number, 1,
					chan->caller_id_number)) {

					ast_setstate(chan, AST_STATE_RING);
					ast_indicate(chan,
						AST_CONTROL_PROCEEDING);
				}

				chan->priority = 0;
				strncpy(chan->exten, called_number,
						sizeof(chan->exten));

				ast_frfree(f);

				return 0;
			}
		}

		ast_frfree(f);
	}

	LOCAL_USER_REMOVE(u);
	return -1;
}

static char *visdn_overlap_dial_descr =
"  vISDNOverlapDial():\n";

#ifndef ASTERISK_VERSION_NUM
static int visdn_devicestate(void *data)
{
	return AST_DEVICE_UNKNOWN;
}
#endif

int load_module()
{
	int res = 0;

	// Initialize q.931 library.
	// No worries, internal structures are read-only and thread safe
	ast_mutex_init(&visdn.lock);

	INIT_LIST_HEAD(&visdn.ccb_q931_queue);
	ast_mutex_init(&visdn.ccb_q931_queue_lock);

	INIT_LIST_HEAD(&visdn.q931_ccb_queue);
	ast_mutex_init(&visdn.q931_ccb_queue_lock);

	int filedes[2];
	if (pipe(filedes) < 0) {
		ast_log(LOG_ERROR, "Unable to create pipe: %s\n",
			strerror(errno));
		return -1;
	}

	visdn.ccb_q931_queue_pipe_read = filedes[0];
	visdn.ccb_q931_queue_pipe_write = filedes[1];

	if (pipe(filedes) < 0) {
		ast_log(LOG_ERROR, "Unable to create pipe: %s\n",
			strerror(errno));
		return -1;
	}

	visdn.q931_ccb_queue_pipe_read = filedes[0];
	visdn.q931_ccb_queue_pipe_write = filedes[1];

	INIT_LIST_HEAD(&visdn.ifs);

	visdn.libq931 = q931_init();
	q931_set_logger_func(visdn.libq931, visdn_logger);

	visdn.libq931->timer_update = visdn_q931_timer_update;
	visdn.libq931->queue_primitive = visdn_queue_primitive;

	visdn_reload_config();

	visdn.netlink_socket = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if(visdn.netlink_socket < 0) {
		ast_log(LOG_ERROR, "Unable to open netlink socket: %s\n",
			strerror(errno));
		return -1;
	}

	struct sockaddr_nl snl;
	snl.nl_family = AF_NETLINK;
	snl.nl_pid = getpid();
	snl.nl_groups = RTMGRP_LINK;

	if (bind(visdn.netlink_socket,
			(struct sockaddr *)&snl,
			sizeof(snl)) < 0) {
		ast_log(LOG_ERROR, "Unable to bind netlink socket: %s\n",
			strerror(errno));
		return -1;
	}

	// Enum interfaces and open them
	struct ifaddrs *ifaddrs;
	struct ifaddrs *ifaddr;

	if (getifaddrs(&ifaddrs) < 0) {
		ast_log(LOG_ERROR, "getifaddr: %s\n", strerror(errno));
		return -1;
	}

	int fd;
	fd = socket(PF_LAPD, SOCK_SEQPACKET, 0);
	if (fd < 0) {
		ast_log(LOG_ERROR, "socket: %s\n", strerror(errno));
		return -1;
	}

	for (ifaddr = ifaddrs ; ifaddr; ifaddr = ifaddr->ifa_next) {
		struct ifreq ifreq;

		memset(&ifreq, 0, sizeof(ifreq));

		strncpy(ifreq.ifr_name,
			ifaddr->ifa_name,
			sizeof(ifreq.ifr_name));

		if (ioctl(fd, SIOCGIFHWADDR, &ifreq) < 0) {
			ast_log(LOG_ERROR, "ioctl (%s): %s\n",
				ifaddr->ifa_name, strerror(errno));
			return -1;
		}

		if (ifreq.ifr_hwaddr.sa_family != ARPHRD_LAPD)
			continue;

		if (!(ifaddr->ifa_flags & IFF_UP))
			continue;

		visdn_add_interface(ifreq.ifr_name);

	}
	close(fd);
	freeifaddrs(ifaddrs);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	if (ast_pthread_create(&visdn_q931_thread, &attr,
					visdn_q931_thread_main, NULL) < 0) {
		ast_log(LOG_ERROR, "Unable to start q931 thread.\n");
		return -1;
	}

#ifndef ASTERISK_VERSION_NUM
	if (ast_channel_register_ex(VISDN_CHAN_TYPE, VISDN_DESCRIPTION,
			 AST_FORMAT_ALAW,
			 visdn_request, visdn_devicestate)) {
		ast_log(LOG_ERROR, "Unable to register channel class %s\n",
			VISDN_CHAN_TYPE);
		return -1;
	}
#else
	if (ast_channel_register(&visdn_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel class %s\n",
			VISDN_CHAN_TYPE);
		return -1;
	}
#endif

	ast_cli_register(&debug_visdn_generic);
	ast_cli_register(&no_debug_visdn_generic);
	ast_cli_register(&debug_visdn_q921);
	ast_cli_register(&no_debug_visdn_q921);
	ast_cli_register(&debug_visdn_q931);
	ast_cli_register(&no_debug_visdn_q931);
	ast_cli_register(&visdn_reload);
	ast_cli_register(&show_visdn_channels);
	ast_cli_register(&show_visdn_interfaces);
	ast_cli_register(&show_visdn_calls);

	ast_register_application(
		"VISDNOverlapDial",
		visdn_exec_overlap_dial,
		"Plays dialtone and waits for digits",
		visdn_overlap_dial_descr);

	return res;
}

int unload_module(void)
{
	ast_unregister_application("VISDNOverlapDial");

	ast_cli_unregister(&show_visdn_calls);
	ast_cli_unregister(&show_visdn_interfaces);
	ast_cli_unregister(&show_visdn_channels);
	ast_cli_unregister(&visdn_reload);
	ast_cli_unregister(&no_debug_visdn_q931);
	ast_cli_unregister(&debug_visdn_q931);
	ast_cli_unregister(&no_debug_visdn_q921);
	ast_cli_unregister(&debug_visdn_q921);
	ast_cli_unregister(&no_debug_visdn_generic);
	ast_cli_unregister(&debug_visdn_generic);

#ifndef ASTERISK_VERSION_NUM
	ast_channel_unregister(VISDN_CHAN_TYPE);
#else
	ast_channel_unregister(&visdn_tech);
#endif

	if (visdn.libq931)
		q931_leave(visdn.libq931);

	return 0;
}

int reload(void)
{
	visdn_reload_config();

	return 0;
}

int usecount()
{
	int res;
	ast_mutex_lock(&usecnt_lock);
	res = visdn.usecnt;
	ast_mutex_unlock(&usecnt_lock);
	return res;
}

char *description()
{
	return VISDN_DESCRIPTION;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
