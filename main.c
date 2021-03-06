/* pingcheck - Check connectivity of interfaces in OpenWRT
 *
 * Copyright (C) 2015 Bruno Randolf <br1@einfach.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include "main.h"

/* main list of interfaces */
static struct ping_intf intf[MAX_NUM_INTERFACES];

void state_change(enum online_state state_new, struct ping_intf* pi)
{
	if (pi->state == state_new) /* no change */
		return;

	pi->state = state_new;

	printlog(LOG_INFO, "Interface '%s' changed to %s", pi->name,
		 get_status_str(pi));

	scripts_run(pi, state_new);
}

const char* get_status_str(struct ping_intf* pi)
{
	switch (pi->state) {
		case UNKNOWN: return "UNKNOWN";
		case DOWN: return "DOWN";
		case NO_ROUTE: return "NO_ROUTE";
		case UP: return "UP";
		case OFFLINE: return "OFFLINE";
		case ONLINE: return "ONLINE";
		default: return "INVALID";
	}
}

const char* get_global_status_str(void)
{
	for (int i=0; i < MAX_NUM_INTERFACES && intf[i].name[0]; i++) {
		if (intf[i].state == ONLINE)
			return "ONLINE";
	}
	return "OFFLINE";
}

int get_online_interface_names(const char** dest, int destLen)
{
	int j = 0;
	for (int i=0; i < MAX_NUM_INTERFACES && intf[i].name[0]; i++) {
		if (intf[i].state == ONLINE && j < destLen)
			dest[j++] = intf[i].name;
	}
	return j;
}

int get_all_interface_names(const char** dest, int destLen)
{
	int i = 0;
	for (; i < MAX_NUM_INTERFACES && intf[i].name[0] && i < destLen; i++) {
		dest[i] = intf[i].name;
	}
	return i;
}

/* called from ubus interface event */
void notify_interface(const char* interface, const char* action)
{
	struct ping_intf* pi = get_interface(interface);
	if (pi == NULL)
		return;

	if (strcmp(action, "ifup") == 0) {
		printlog(LOG_INFO, "Interface '%s' event UP", interface);
		ping_init(pi);
		ping_send(pi);
	} else if (strcmp(action, "ifdown") == 0) {
		printlog(LOG_INFO, "Interface '%s' event DOWN", interface);
		ping_stop(pi);
		state_change(DOWN, pi);
	}
}

/* also called from ubus server_status */
struct ping_intf* get_interface(const char* interface)
{
	struct ping_intf* pi = NULL;
	/* find interface in our list */
	for (int i=0; i < MAX_NUM_INTERFACES && intf[i].name[0]; i++) {
		if (strncmp(intf[i].name, interface, MAX_IFNAME_LEN) == 0) {
			pi = &intf[i];
			break;
		}
	}
	return pi;
}

/* reset counters for all (pass NULL) or one interface */
void reset_counters(const char* interface) {
	for (int i=0; i < MAX_NUM_INTERFACES && intf[i].name[0]; i++) {
		if (interface == NULL || strncmp(intf[i].name, interface, MAX_IFNAME_LEN) == 0) {
			intf[i].cnt_sent = 0;
			intf[i].cnt_succ = 0;
			intf[i].last_rtt = 0;
			intf[i].max_rtt = 0;
		}
	}
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char** argv)
{
	int ret;

	openlog("pingcheck", LOG_PID|LOG_CONS, LOG_DAEMON);

	ret = uloop_init();
	if (ret < 0) {
		printlog(LOG_CRIT, "Could not initialize uloop");
		return EXIT_FAILURE;
	}

	ret = uci_config_pingcheck(intf, MAX_NUM_INTERFACES);
	if (!ret) {
		printlog(LOG_CRIT, "Could not read UCI config");
		goto exit;
	}

	scripts_init();

	ret = ubus_init();
	if (!ret)
		goto exit;

	/* listen for ubus network interface events */
	ret = ubus_listen_network_events();
	if (!ret) {
		printlog(LOG_CRIT, "Could not listen to interface events");
		goto exit;
	}

	ubus_register_server();

	/* start ping on all available interfaces */
	for (int i=0; i < MAX_NUM_INTERFACES && intf[i].name[0]; i++) {
		ping_init(&intf[i]);
	}

	/* main loop */
	uloop_run();

	/* print statistics and cleanup */
	printf("\n");
	for (int i=0; i < MAX_NUM_INTERFACES && intf[i].name[0]; i++) {
		printf("%s:\t%-8s %3.0f%% (%d/%d on %s)\n", intf[i].name,
		       get_status_str(&intf[i]),
		       (float)intf[i].cnt_succ*100/intf[i].cnt_sent,
		       intf[i].cnt_succ, intf[i].cnt_sent, intf[i].device);
		ping_stop(&intf[i]);
	}

exit:
	scripts_finish();
	uloop_done();
	ubus_finish();
	closelog();

	return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}
