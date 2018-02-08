/* dhcpc.c
 *
 * udhcp DHCP client
 *
 * Russ Dill <Russ.Dill@asu.edu> July 2001
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <errno.h>
#include <sys/sysinfo.h>

#include "dhcpd.h"
#include "dhcpc.h"
#include "options.h"
#include "clientpacket.h"
#include "packet.h"
#include "script.h"
#include "socket.h"
#include "debug.h"
#include "pidfile.h"
#include "arpping.h"

#ifdef WAN_LAN_IPCONFLICT
#include <sys/wait.h>
#endif
static int state;
static unsigned long requested_ip; /* = 0 */
#ifdef DHCPC_CHOOSE_OLDIP
static unsigned long old_requested_ip;
#endif
static unsigned long server_addr;
static unsigned long timeout;
static int packet_num; /* = 0 */
static int fd;
static int signal_pipe[2];

#define LISTEN_NONE 0
#define LISTEN_KERNEL 1
#define LISTEN_RAW 2
static int listen_mode;
static int orange_flag = 0;

#define DEFAULT_SCRIPT		"/usr/share/udhcpc/default.script"
#define DEFAULT_SCRIPT_AP	"/usr/share/udhcpc/default.script.ap"

struct client_config_t client_config = {
	/* Default options. */
	abort_if_no_lease: 0,
	foreground: 0,
	quit_after_lease: 0,
	background_if_no_lease: 0,
	interface: "eth0",
	pidfile: NULL,
	script: NULL,
	clientid: NULL,
	domain_name: NULL,
	hostname: NULL,
	ifindex: 0,
	arp: "\0\0\0\0\0\0",		/* appease gcc-3.0 */
#ifdef SUPPORT_OPTION_60
	vendor: NULL,
#endif
#ifdef SUPPORT_OPTION_77
	user_class: NULL,
#endif
#ifdef SUPPORT_OPTION_90
	authentication: NULL,
#endif

	apmode: 0,
};

#ifndef BB_VER
static void show_usage(void)
{
	printf(
"Usage: udhcpc [OPTIONS]\n\n"
"  -c, --clientid=CLIENTID         Client identifier\n"
"  -H, --hostname=HOSTNAME         Client hostname\n"
"  -h                              Alias for -H\n"
"  -f, --foreground                Do not fork after getting lease\n"
"  -b, --background                Fork to background if lease cannot be\n"
"                                  immediately negotiated.\n"
"  -i, --interface=INTERFACE       Interface to use (default: eth0)\n"
"  -n, --now                       Exit with failure if lease cannot be\n"
"                                  immediately negotiated.\n"
"  -p, --pidfile=file              Store process ID of daemon in file\n"
"  -d, --domain_name               domain name\n"
"  -q, --quit                      Quit after obtaining lease\n"
"  -r, --request=IP                IP address to request (default: none)\n"
"  -s, --script=file               Run file at dhcp events (default:\n"
"                                  " DEFAULT_SCRIPT ")\n"
"  -v, --version                   Display version\n"
#ifdef SUPPORT_OPTION_60
"  -V, --vendor                    Set vendor identifier\n"
#endif
#ifdef DHCPC_CHOOSE_OLDIP
"  -N,  --oldip                    Ip address of last time\n"
#endif
"  -a, --apmode                    DUT is running in AP mode\n"
	);
	exit(0);
}
#endif


/* just a little helper */
static void change_mode(int new_mode)
{
	DEBUG(LOG_INFO, "entering %s listen mode",
		new_mode ? (new_mode == 1 ? "kernel" : "raw") : "none");
	close(fd);
	fd = -1;
	listen_mode = new_mode;
}


/* perform a renew */
static void perform_renew(void)
{
	LOG(LOG_INFO, "Performing a DHCP renew");
	switch (state) {
	case BOUND:
		change_mode(LISTEN_KERNEL);
	case RENEWING:
	case REBINDING:
		state = RENEW_REQUESTED;
		break;
	case RENEW_REQUESTED: /* impatient are we? fine, square 1 */
		run_script(NULL, "deconfig");
	case REQUESTING:
	case RELEASED:
		change_mode(LISTEN_RAW);
		state = INIT_SELECTING;
		break;
	case INIT_SELECTING:
		break;
	}

	/* start things over */
	packet_num = 0;

	/* Kill any timeouts because the user wants this to hurry along */
	timeout = 0;
}


/* perform a release */
static void perform_release(void)
{
	char buffer[16];
	struct in_addr temp_addr;

	/* send release packet */
	if (state == BOUND || state == RENEWING || state == REBINDING) {
		temp_addr.s_addr = server_addr;
		sprintf(buffer, "%s", inet_ntoa(temp_addr));
		temp_addr.s_addr = requested_ip;
		LOG(LOG_INFO, "Unicasting a release of %s to %s", 
				inet_ntoa(temp_addr), buffer);
		send_release(server_addr, requested_ip); /* unicast */
		run_script(NULL, "deconfig");
	}
	LOG(LOG_INFO, "Entering released state");

	change_mode(LISTEN_NONE);
	state = RELEASED;
	timeout = 0x7fffffff;
}


/* Exit and cleanup */
static void exit_client(int retval)
{
	pidfile_delete(client_config.pidfile);
	CLOSE_LOG();
	exit(retval);
}


/* Signal handler */
static void signal_handler(int sig)
{
	if (send(signal_pipe[1], &sig, sizeof(sig), MSG_DONTWAIT) < 0) {
		LOG(LOG_ERR, "Could not send signal: %s",
			strerror(errno));
	}
}


static void background(void)
{
	int pid_fd;

	pid_fd = pidfile_acquire(client_config.pidfile); /* hold lock during fork. */
	while (pid_fd >= 0 && pid_fd < 3) pid_fd = dup(pid_fd); /* don't let daemon close it */
	if (daemon(0, 1) == -1) {
		perror("fork");
		exit_client(1);
	}
	client_config.foreground = 1; /* Do not fork again. */
	pidfile_write_release(pid_fd);
}

/*
 * function related to timeout values seems not work well by calling `time(NULL)`,
 * use `uptime` instead.
 *
 * [Bug 11761][DHCP-client]On T1=50% and T2=87.5% leased time, should send DHCP_REQUEST
 *             to reusing the current IP address
 */
static unsigned long uptime(void)
{
       struct sysinfo info;

       sysinfo(&info);

       return (unsigned long) info.uptime;
}
/*
 * "1": WAN PHY is Link Up.
 * "0": WAN PHY is Link Down.
 * It is set by `usr/bin/detcable`.
 */
#define CABLE_FILE	"/tmp/port_status"

static inline int eth_up(void)
{
	char value;
	int fd, link_up;

	fd = open(CABLE_FILE, O_RDONLY, 0666);
	if (fd < 0)
		return 0;

	if (read(fd, &value, 1) == 1 && value == '1')
		link_up = 1;
	else
		link_up = 0;

	close(fd);

	return link_up;
}

#define BR_MODE_ENABLE	"/tmp/enable_br_mode"

static inline int br_mode_enable(void)
{
	char value;
	int fd, enable;

	fd = open(BR_MODE_ENABLE, O_RDONLY, 0666);
	if (fd < 0)
		return 0;

	if (read(fd, &value, 1) == 1 && value == '1')
		enable = 1;
	else
		enable = 0;

	close(fd);

	return enable;
}

/*
 * return:
 *     1  addr free
 *     0  addr used
 *     -1 error
 */
static int client_ip_check(u_int32_t addr)
{
       return arpping(addr, 0, client_config.arp, client_config.interface, NULL);
}

static void set_orange_vlan_egress_priority(int status)
{
    char cmd_buf[256] = {0};

    snprintf(cmd_buf, sizeof(cmd_buf), "/usr/share/udhcpc/org_dhcp_pri_config.script %d %s", status, client_config.interface);
    system(cmd_buf);
}

#ifdef COMBINED_BINARY
int udhcpc_main(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
	unsigned char *temp, *message;
	unsigned long nak_server_addr;
	unsigned long t1 = 0, t2 = 0, xid = 0;
	unsigned long start = 0, lease;
	fd_set rfds;
	int retval;
	struct timeval tv;
	int c, len;
	struct dhcpMessage packet;
#ifdef DHCPC_CHOOSE_OLDIP
	unsigned long T = 0, T1 = 0;
#endif
	struct in_addr temp_addr;
	int pid_fd;
	time_t now;
	int max_fd;
	int sig;
	int script_specified = 0;

	static struct option arg_options[] = {
		{"clientid",	required_argument,	0, 'c'},
		{"foreground",	no_argument,		0, 'f'},
		{"background",	no_argument,		0, 'b'},
		{"hostname",	required_argument,	0, 'H'},
		{"hostname",    required_argument,      0, 'h'},
		{"domain_name",    required_argument,   0, 'd'},
		{"interface",	required_argument,	0, 'i'},
		{"now", 	no_argument,		0, 'n'},
		{"pidfile",	required_argument,	0, 'p'},
		{"quit",	no_argument,		0, 'q'},
		{"request",	required_argument,	0, 'r'},
		{"script",	required_argument,	0, 's'},
		{"version",	no_argument,		0, 'v'},
#ifdef SUPPORT_OPTION_60
		{"Vendor",	required_argument,	0, 'V'},
#endif
#ifdef SUPPORT_OPTION_77
		{"User_class",	required_argument,	0, 'U'},
#endif
#ifdef SUPPORT_OPTION_90
		{"Authen",	required_argument,	0, 'A'},
#endif

#ifdef DHCPC_CHOOSE_OLDIP
		{"oldip",	required_argument,	0, 'N'},
#endif
		{"apmode",	no_argument,		0, 'a'},
		{"help",	no_argument,		0, '?'},
		{"orange hidden flag",        no_argument,            0, 'O'},
		{0, 0, 0, 0}
	};

	/* get options */
	char optstr[64] = { [0 ... 63] = 0x00 };
	strcpy(optstr, "Oac:fbH:h:d:i:np:qr:s:v");
#ifdef SUPPORT_OPTION_60
	strcpy(optstr + strlen(optstr), "V:");
#endif
#ifdef SUPPORT_OPTION_77
	strcpy(optstr + strlen(optstr), "U:");
#endif
#ifdef SUPPORT_OPTION_90
	strcpy(optstr + strlen(optstr), "A:");
#endif

#ifdef DHCPC_CHOOSE_OLDIP
       strcpy(optstr + strlen(optstr), "N:");
#endif
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, optstr, arg_options, &option_index);
		if (c == -1) break;
		
		switch (c) {
		case 'c':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.clientid) free(client_config.clientid);
			client_config.clientid = xmalloc(len + 2);
			client_config.clientid[OPT_CODE] = DHCP_CLIENT_ID;
			client_config.clientid[OPT_LEN] = len;
			client_config.clientid[OPT_DATA] = '\0';
			strncpy(client_config.clientid + OPT_DATA, optarg, len);
			break;
		case 'f':
			client_config.foreground = 1;
			break;
		case 'b':
			client_config.background_if_no_lease = 1;
			break;
		case 'h':
		case 'H':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.hostname) free(client_config.hostname);
			client_config.hostname = xmalloc(len + 2);
			client_config.hostname[OPT_CODE] = DHCP_HOST_NAME;
			client_config.hostname[OPT_LEN] = len;
			strncpy(client_config.hostname + 2, optarg, len);
			break;
		case 'd':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.domain_name) free(client_config.domain_name);
			client_config.domain_name= xmalloc(len + 2);
			client_config.domain_name[OPT_CODE] = DHCP_DOMAIN_NAME;
			client_config.domain_name[OPT_LEN] = len;
			strncpy(client_config.domain_name + 2, optarg, len);
			break;
		case 'i':
			client_config.interface =  optarg;
			break;
		case 'n':
			client_config.abort_if_no_lease = 1;
			break;
		case 'p':
			client_config.pidfile = optarg;
			break;
		case 'q':
			client_config.quit_after_lease = 1;
			break;
		case 'r':
			requested_ip = inet_addr(optarg);
			break;
#ifdef DHCPC_CHOOSE_OLDIP
		case 'N':
			old_requested_ip = inet_addr(optarg);
			break;
#endif
		case 's':
			client_config.script = optarg;
			script_specified = 1;
			break;
		case 'v':
			printf("udhcpcd, version %s\n\n", VERSION);
			exit_client(0);
			break;
#ifdef SUPPORT_OPTION_60
		case 'V':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.vendor) free(client_config.vendor);
			client_config.vendor = xmalloc(len + 2);
			client_config.vendor[OPT_CODE] = DHCP_VENDOR;
			client_config.vendor[OPT_LEN] = len;
			client_config.vendor[OPT_DATA] = '\0';
			strncpy(client_config.vendor + OPT_DATA, optarg, len);
			break;
#endif
#ifdef SUPPORT_OPTION_77
		case 'U':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.user_class) free(client_config.user_class);
			client_config.user_class = xmalloc(len + 2);
			client_config.user_class[OPT_CODE] = DHCP_USER_CLASS;
			client_config.user_class[OPT_LEN] = len+1;
			client_config.user_class[OPT_DATA] = len;
			strncpy(client_config.user_class + OPT_DATA + 1, optarg, len);
			break;
#endif
#ifdef SUPPORT_OPTION_90
		case 'A':
			len = strlen(optarg) + strlen("fti/");
			if (len > 255) len = 255;
			if (client_config.authentication) free(client_config.authentication);
			client_config.authentication = xmalloc(len + 12);
			client_config.authentication[OPT_CODE] = DHCP_AUTHEN;
			client_config.authentication[OPT_LEN] = len+11;
			memset(&client_config.authentication[OPT_DATA],0x00,11);
			if ((orange_flag == 1) && (strstr(optarg, "fti/") == NULL)) //search fti
			{
			        snprintf(client_config.authentication + OPT_DATA + 11, len + 1, "fti/%s", optarg);
			}
			else
			{
				strncpy(client_config.authentication + OPT_DATA + 11, optarg, len);
			}
			break;
#endif

		case 'a':
			client_config.apmode = 1;
			break;

		case 'O':
			orange_flag = 1;
			break;

		default:
			show_usage();
		}
	}

	//Set default orange option value for option 60 and 77
	if (orange_flag == 1)
	{
	    //set option 60 to sagem
	    len = strlen("sagem");
	    if (client_config.vendor) free(client_config.vendor);
	    client_config.vendor = xmalloc(len + 2);
	    client_config.vendor[OPT_CODE] = DHCP_VENDOR;
	    client_config.vendor[OPT_LEN] = len;
	    client_config.vendor[OPT_DATA] = '\0';
	    strncpy(client_config.vendor + OPT_DATA, "sagem", len);

	    if (strcmp(client_config.interface, "brwan") == 0)
	    {	 
	    	//set option 77 to FSVDSL_livebox.Internet.softathome.Livebox3
	    	len = strlen("FSVDSL_livebox.Internet.softathome.Livebox3");
	    }
	    else if (strcmp(client_config.interface, "brotv") == 0)
	    {
		//set option 77 to FSVDSL_livebox.MLTV.softathome.Livebox3
		len = strlen("FSVDSL_livebox.MLTV.softathome.Livebox3");
	    }

	    if (client_config.user_class) free(client_config.user_class);
	    client_config.user_class = xmalloc(len + 2);
	    client_config.user_class[OPT_CODE] = DHCP_USER_CLASS;
	    client_config.user_class[OPT_LEN] = len+1;
	    client_config.user_class[OPT_DATA] = len;
	    if (strcmp(client_config.interface, "brwan") == 0)
	    {
		strncpy(client_config.user_class + OPT_DATA + 1, "FSVDSL_livebox.Internet.softathome.Livebox3", len);
	    }
	    else if (strcmp(client_config.interface, "brotv") == 0)
	    {
		strncpy(client_config.user_class + OPT_DATA + 1, "FSVDSL_livebox.MLTV.softathome.Livebox3", len);
	    }
	}

	if (!script_specified) {
		if (client_config.apmode)
			client_config.script = DEFAULT_SCRIPT_AP;
		else
			client_config.script = DEFAULT_SCRIPT;
	}

	OPEN_LOG("udhcpc");
	LOG(LOG_INFO, "udhcp client (v%s) started", VERSION);

	pid_fd = pidfile_acquire(client_config.pidfile);
	pidfile_write_release(pid_fd);

	if (read_interface(client_config.interface, &client_config.ifindex, 
			   NULL, client_config.arp) < 0)
		exit_client(1);
		
	if (!client_config.clientid) {
		client_config.clientid = xmalloc(6 + 3);
		client_config.clientid[OPT_CODE] = DHCP_CLIENT_ID;
		client_config.clientid[OPT_LEN] = 7;
		client_config.clientid[OPT_DATA] = 1;
		memcpy(client_config.clientid + 3, client_config.arp, 6);
	}

	/* setup signal handlers */
	socketpair(AF_UNIX, SOCK_STREAM, 0, signal_pipe);
	signal(SIGUSR1, signal_handler);
	signal(SIGUSR2, signal_handler);
	signal(SIGTERM, signal_handler);
	
	state = INIT_SELECTING;
	run_script(NULL, "deconfig");
	change_mode(LISTEN_RAW);

	for (;;) {

		tv.tv_sec = timeout - uptime();
		tv.tv_usec = 0;
		FD_ZERO(&rfds);

		if (listen_mode != LISTEN_NONE && fd < 0) {
			if (listen_mode == LISTEN_KERNEL)
				fd = listen_socket(INADDR_ANY, CLIENT_PORT, client_config.interface);
			else
				fd = raw_socket(client_config.ifindex, CLIENT_PORT);
			if (fd < 0) {
				LOG(LOG_ERR, "FATAL: couldn't listen on socket, %s", strerror(errno));
				exit_client(0);
			}
		}
		if (fd >= 0) FD_SET(fd, &rfds);
		FD_SET(signal_pipe[0], &rfds);		

		if (tv.tv_sec > 0) {
			DEBUG(LOG_INFO, "Waiting on select...\n");
			max_fd = signal_pipe[0] > fd ? signal_pipe[0] : fd;
			retval = select(max_fd + 1, &rfds, NULL, NULL, &tv);
		} else retval = 0; /* If we already timed out, fall through */

		now = uptime();
		if (retval == 0) {
			/* timeout dropped to zero */
			switch (state) {
			case INIT_SELECTING:
				if (packet_num < (client_config.apmode ? (!br_mode_enable() ? 5 : 5 ) : 3)) {
					/*
					 * [NETGEAR Lancelot.Wang]: Before sending discover packet, should check
					 * whether WAN cable is plugged. If WAN cable is not plugged, the discovery
					 * packet should not be sent.
					 */
					if (!eth_up() && !client_config.apmode) {
						if (client_config.background_if_no_lease && !client_config.foreground)
							background();

						/* LOG(LOG_INFO, "WAN cable is not plugged, NOT send discover!!!"); */

						packet_num = 0;
						timeout = now + 2;

						break;
					}

					if (packet_num == 0)
						xid = random_xid();

					/* send discover packet */
					/*if ( client_config.apmode != 1 )
						sleep(5);       */

					set_orange_vlan_egress_priority(0);
					send_discover(xid, requested_ip); /* broadcast */
					
					timeout = uptime() + (client_config.apmode ? 5 : ((packet_num == 2) ? 4 : 2));
					packet_num++;
				} else {
					/* If forked, don't fork again. */
					if (client_config.background_if_no_lease && !client_config.foreground) {
						LOG(LOG_INFO, "No lease, forking to background.");
						background();
					} else if (client_config.abort_if_no_lease) {
						LOG(LOG_INFO, "No lease, failing.");
						exit_client(1);
				  	}

					if (client_config.apmode)
						run_script(NULL, "runzcip");

					/* wait to try again */
					packet_num = 0;
					/* 
					 * [NETGEAR SPEC V1.6 --- 5.1 DHCP (DHCP client)]: If the whole DHCP
					 * request procedure is failed, router should restart the procedure 
					 * every 5 minutes. 
					 * [NETGEAR SPEC V2.0 -- 12.9.3	Auto IP]:
					 * Periodic checking for dynamic address availability
					 * A device that has auto-configured an IP address MUST periodically check 
					 * every 4 minutes for the existence of a DHCP server.
					 * [new Router Spec Rev.11 2012.12.03 DHCP (DHCP client)]: if the whole DHCP
					 * request procedure is failed, router should restart the procedure every 10 seconds.
					 */
					timeout = now + (client_config.apmode ? (!br_mode_enable() ? 10 : 10) : 10);
				}
				break;
			case RENEW_REQUESTED:
			case REQUESTING:
				if (packet_num < 3) {
					/* send request packet */
					if (state == RENEW_REQUESTED)
					{
						set_orange_vlan_egress_priority(0);
						send_renew(xid, server_addr, requested_ip); /* unicast */
					}
					else send_selecting(xid, server_addr, requested_ip); /* broadcast */
					
					/*
					 * [new Router Spec Rev.11 2012.12.03 DHCP (DHCP client)]: for the cases of Ethernet cable plug-off and plug-in,
					 * client should send 3 consecutive DHCP_REQUEST with 1 second interval and include current IP address as preferred
					 * client IP address.
					 */
					timeout = now + ((packet_num == 2) ? 10 : 1);
					packet_num++;
				} else {
					/* timed out, go back to init state */
					if (state == RENEW_REQUESTED) run_script(NULL, "deconfig");
					state = INIT_SELECTING;
					timeout = now;
					packet_num = 0;
					change_mode(LISTEN_RAW);
				}
				break;
			case BOUND:
				/* Lease is starting to run out, time to enter renewing state */
				state = RENEWING;
				change_mode(LISTEN_KERNEL);
				DEBUG(LOG_INFO, "Entering renew state");
				/* fall right through */
			case RENEWING:
				/* Either set a new T1, or enter REBINDING state */
				if ((t2 - t1) <= (lease / 14400 + 1)) {
					/* timed out, enter rebinding state */
					state = REBINDING;
					timeout = now + (t2 - t1);
					DEBUG(LOG_INFO, "Entering rebinding state");
				} else {
					set_orange_vlan_egress_priority(0);
					/* send a request packet */
					send_renew(xid, server_addr, requested_ip); /* unicast */
					
					t1 = (t2 - t1) / 2 + t1;
					timeout = t1 + start;
				}
				break;
			case REBINDING:
				/* Either set a new T2, or enter INIT state */
				if ((lease - t2) <= (lease / 14400 + 1)) {
					/* timed out, enter init state */
					state = INIT_SELECTING;
					LOG(LOG_INFO, "Lease lost, entering init state");
					run_script(NULL, "deconfig");
					timeout = now;
					packet_num = 0;
					change_mode(LISTEN_RAW);
				} else {
					set_orange_vlan_egress_priority(0);
					/* send a request packet */
					send_renew(xid, 0, requested_ip); /* broadcast */

					t2 = (lease - t2) / 2 + t2;
					timeout = t2 + start;
				}
				break;
			case RELEASED:
				/* yah, I know, *you* say it would never happen */
				timeout = 0x7fffffff;
				break;
			}
		} else if (retval > 0 && listen_mode != LISTEN_NONE && FD_ISSET(fd, &rfds)) {
			/* a packet is ready, read it */
			
			if (listen_mode == LISTEN_KERNEL)
				len = get_packet(&packet, fd);
			else len = get_raw_packet(&packet, fd);
			
			if (len == -1 && errno != EINTR) {
				DEBUG(LOG_INFO, "error on read, %s, reopening socket", strerror(errno));
				change_mode(listen_mode); /* just close and reopen */
			}
			if (len < 0) continue;
			
			if (packet.xid != xid) {
				DEBUG(LOG_INFO, "Ignoring XID %lx (our xid is %lx)",
					(unsigned long) packet.xid, xid);
				continue;
			}
			
			if ((message = get_option(&packet, DHCP_MESSAGE_TYPE)) == NULL) {
				DEBUG(LOG_ERR, "couldnt get option from packet -- ignoring");
				continue;
			}
			
			switch (state) {
			case INIT_SELECTING:
				/* Must be a DHCPOFFER to one of our xid's */
				if (*message == DHCPOFFER) {
					if ((temp = get_option(&packet, DHCP_SERVER_ID))) {
						memcpy(&server_addr, temp, 4);
						xid = packet.xid;
						requested_ip = packet.yiaddr;

						/* enter requesting state */
						state = REQUESTING;
						packet_num = 0;
#ifdef DHCPC_CHOOSE_OLDIP
						if (packet.yiaddr == old_requested_ip) {
							timeout = now;
							break;
						}

						T = uptime();
						T1 = T + 1;
						while (T1 > T) {
							tv.tv_sec = timeout - uptime();
							tv.tv_usec = 0;
							FD_ZERO(&rfds);

							if (listen_mode != LISTEN_NONE && fd < 0) {
								if (listen_mode == LISTEN_KERNEL)
									fd = listen_socket(INADDR_ANY, CLIENT_PORT, client_config.interface);
								else
									fd = raw_socket(client_config.ifindex, CLIENT_PORT);
								if (fd < 0) {
									LOG(LOG_ERR, "FATAL: couldn't listen on socket, %s", strerror(errno));
									exit_client(0);
								}
							}
							if (fd >= 0) FD_SET(fd, &rfds);
							FD_SET(signal_pipe[0], &rfds);		

							if (tv.tv_sec > 0) {
								DEBUG(LOG_INFO, "Waiting on select...\n");
								max_fd = signal_pipe[0] > fd ? signal_pipe[0] : fd;
								retval = select(max_fd + 1, &rfds, NULL, NULL, &tv);
							} else
								retval = 0; /* If we already timed out, fall through */		

							if (retval == 0) break;

							if (listen_mode == LISTEN_KERNEL)
								len = get_packet(&packet, fd);
							else 
								len = get_raw_packet(&packet, fd);

							if (len == -1 && errno != EINTR) {
								DEBUG(LOG_INFO, "error on read, %s, reopening socket", strerror(errno));
								change_mode(listen_mode); /* just close and reopen */
							}
							if (len < 0) {
								T = uptime();
								continue;
							}

							if (packet.xid != xid) {
								DEBUG(LOG_INFO, "Ignoring XID %lx (our xid is %lx)",
										(unsigned long) packet.xid, xid);
								T = uptime();
								continue;
							}

							if ((message = get_option(&packet, DHCP_MESSAGE_TYPE)) == NULL) {
								DEBUG(LOG_ERR, "couldnt get option from packet -- ignoring");
								T = uptime();
								continue;
							}

							if ((*message == DHCPOFFER) && (temp = get_option(&packet, DHCP_SERVER_ID))) {
								if (packet.yiaddr == old_requested_ip) {
									memcpy(&server_addr, temp, 4);
									xid = packet.xid;
									requested_ip = packet.yiaddr;
									break;
								}
							}

							T = uptime();
						}
#endif
						timeout = now;
					} else {
						DEBUG(LOG_ERR, "No server ID in message");
					}
				}
				break;
			case RENEW_REQUESTED:
			case REQUESTING:
			case RENEWING:
			case REBINDING:
				if (*message == DHCPACK) {
                                        /*
                                         * [NETGEAR SPEC V1.6 --- 5.1 DHCP (DHCP client)]: An implementation
                                         * MUST use ARP to detect conflict with the assigned IP address after
                                         * DHCPACK is received. If there is a conflict detected, an implemen-
                                         * tation MUST follow the spec to send a DHCPDECLINE back to the server
                                         * and restart the DHCP process after 10 seconds to request for another
                                         * IP address.
                                         */
                                        if (!client_ip_check(requested_ip)) {
                                                struct in_addr in;
                                                in.s_addr = requested_ip;

                                                send_decline(packet.xid, requested_ip, server_addr);

                                                state = INIT_SELECTING;
                                                timeout = now + 10;
                                                requested_ip = 0;
                                                server_addr = 0;

                                                change_mode(LISTEN_RAW);

                                                LOG(LOG_ERR, "IP %s conflict !!", inet_ntoa(in));

                                                if (client_config.background_if_no_lease && !client_config.foreground) {
                                                        LOG(LOG_INFO, "Decline the DHCPACK, forking to background.");
                                                        background();
                                                }
                                                break;
                                        }
					if (!(temp = get_option(&packet, DHCP_LEASE_TIME))) {
						LOG(LOG_ERR, "No lease time with ACK, using 1 hour lease");
						lease = 60 * 60;
					} else {
						memcpy(&lease, temp, 4);
						lease = ntohl(lease);
					}
#ifdef WAN_LAN_IPCONFLICT
                                       /*
                                        * According to NETGEAR Spec v1.6:
                                        * --4.4 WAN/LAN IP conflict detection
                                        * ----4.4.2 Implementation detail
                                        * Figure-1: DHCP client Finite State Machine FSM.
                                        * If WAN/LAN IP conflict is detected, DHPC client needs send request,
                                        * and transits to `Rebooting' state to do address solicit again.
                                        */
                                       if ((temp = get_option(&packet, DHCP_SUBNET)) != NULL && !client_config.apmode) {
                                               int ret;
                                               unsigned char *ipaddr = (unsigned char *)&packet.yiaddr;
                                               char cmd[128];
                                               sprintf(cmd, "%s %d.%d.%d.%d %d.%d.%d.%d",
                                                               IP_CONFLICT_CMD,ipaddr[0], ipaddr[1], ipaddr[2], ipaddr[3],
                                                               temp[0], temp[1], temp[2], temp[3]);
					       ret = system(cmd);
                                               if (WEXITSTATUS(ret) == 1) {
                                                       run_script(NULL, "deconfig");
                                                       state = INIT_SELECTING;
                                                       timeout = now;
                                                       requested_ip = 0;
                                                       packet_num = 0;
                                                       change_mode(LISTEN_RAW);
                                                       break;
                                               }
                                        }
#endif
					set_orange_vlan_egress_priority(1);
					/* enter bound state */
					t1 = lease / 2;
					
					/* little fixed point for n * .875 */
					t2 = (lease * 0x7) >> 3;
					temp_addr.s_addr = packet.yiaddr;
					LOG(LOG_INFO, "Lease of %s obtained, lease time %ld", 
						inet_ntoa(temp_addr), lease);
					syslog(6, "[Internet connected] IP address: %s,", inet_ntoa(temp_addr));
					start = now;
					timeout = t1 + start;
					requested_ip = packet.yiaddr;
					run_script(&packet,
						   ((state == RENEWING || state == REBINDING) ? "renew" : "bound"));

					state = BOUND;
					change_mode(LISTEN_NONE);
					if (client_config.quit_after_lease) 
						exit_client(0);
					if (!client_config.foreground)
						background();
				} else if (*message == DHCPNAK) {
					/*
					 * Fix bug 24603 - [DHCP]sometimes the DUT will always sending discover for the 
					 * previcious dynamic ip address until you plug off the cable of wan side.
					 * if the bad server don't follow RFC and sends NAK to client that didn't 
					 * request ip to it, we better check whether the server is the one client requests.
					 */
					/* If there's not server id in NAK packet, we should ignore this packet
					 * refer to rfc 2131 table 3.
					 */
					if(!(temp = get_option(&packet, DHCP_SERVER_ID)))
						continue;
					else {
					     memcpy(&nak_server_addr, temp, 4);
					     if (nak_server_addr != server_addr) {
						continue;
					     }
					}
					/* return to init state */
					LOG(LOG_INFO, "Received DHCP NAK");
					run_script(&packet, "nak");
					if (state != REQUESTING)
						run_script(NULL, "deconfig");
					state = INIT_SELECTING;
					timeout = now;
					requested_ip = 0;
					packet_num = 0;
					change_mode(LISTEN_RAW);
					sleep(3); /* avoid excessive network traffic */
				}
				break;
			/* case BOUND, RELEASED: - ignore all packets */
			}	
		} else if (retval > 0 && FD_ISSET(signal_pipe[0], &rfds)) {
			if (read(signal_pipe[0], &sig, sizeof(sig)) < 0) {
				DEBUG(LOG_ERR, "Could not read signal: %s", 
					strerror(errno));
				continue; /* probably just EINTR */
			}
			switch (sig) {
			case SIGUSR1:
				set_orange_vlan_egress_priority(0);
				perform_renew();
				break;
			case SIGUSR2:
				syslog(6, "[Internet disconnected]");
			   	set_orange_vlan_egress_priority(0);	
				perform_release();
				if (client_config.apmode)
					run_script(NULL, "runzcip");
				break;
			case SIGTERM:
				LOG(LOG_INFO, "Received SIGTERM");
				syslog(6, "[Internet disconnected]");
				set_orange_vlan_egress_priority(0);
				/* 
				 * [NETGEAR Spec 1.6]: It is recommended to send a DHCP RELEASE message to
				 * the server under the case software shutdown or reboot.
				 */
				perform_release();
				exit_client(0);
			}
		} else if (retval == -1 && errno == EINTR) {
			/* a signal was caught */		
		} else {
			/* An error occured */
			DEBUG(LOG_ERR, "Error on select");
		}
		
	}
	return 0;
}

