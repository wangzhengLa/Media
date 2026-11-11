#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>

#include <linux/event.h>
#include <linux/linux_sta.h>
#include <linux/linux_common.h>
#include <wifi_log.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <udhcpc.h>

#define IP_SIZE     16

static int get_ipv4(const char *inf, char *ip)
{
	int sd;
	struct sockaddr_in sin;
	struct ifreq ifr;

	sd = socket(AF_INET, SOCK_DGRAM, 0);
	if(-1 == sd){
		WMG_ERROR("socket error: %s\n", strerror(errno));
		return -1;
	}

	strncpy(ifr.ifr_name, inf, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;

	/* SIOCGIFADDR returns EADDRNOTAVAIL ("Cannot assign requested
	 * address") while the interface has no IPv4 address yet. is_ip_exist()
	 * polls this every 200ms while waiting for the DHCP lease, so that is
	 * an expected intermediate state, not a failure: the old WMG_ERROR
	 * flooded the log on every wifi -c. Real errors (e.g. ENODEV for a
	 * removed interface) are still reported at ERROR level. */
	if(ioctl(sd, SIOCGIFADDR, &ifr) < 0) {
		if (errno == EADDRNOTAVAIL)
			WMG_DEBUG("%s: no ipv4 address yet: %s\n", inf, strerror(errno));
		else
			WMG_ERROR("ioctl error: %s\n", strerror(errno));
		close(sd);
		return -1;
	}

	memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
	snprintf(ip, IP_SIZE, "%s", inet_ntoa(sin.sin_addr));

	close(sd);
	return 0;
}

#define IPV6_LEN 128
static int get_ipv6(const char *inf, char *ipv6, int ip_no)
{
	FILE *f;
	unsigned char scope, prefix, i = 0;
	unsigned char _ipv6[16] = {0};
	char inf_name[IFNAMSIZ] = {0};

	f = fopen("/proc/net/if_inet6", "r");
	if (f == NULL) {
		return -1;
	}

	while((19 == fscanf(f,
		" %2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx\
		%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx \
		%*x %2hhx %2hhx %*x %6s",
		&_ipv6[0], &_ipv6[1], &_ipv6[2], &_ipv6[3],
		&_ipv6[4], &_ipv6[5], &_ipv6[6], &_ipv6[7],
		&_ipv6[8], &_ipv6[9], &_ipv6[10], &_ipv6[11],
		&_ipv6[12], &_ipv6[13], &_ipv6[14], &_ipv6[15],
		&prefix, &scope, inf_name)) && (i < ip_no)) {
		if(!strcmp(inf_name, inf)) {
			if(inet_ntop(AF_INET6, _ipv6, (ipv6 + (i * IPV6_LEN)), IPV6_LEN) != NULL) {
				WMG_DEBUG("%s(%d): %s\n", inf_name, i, (ipv6 + (i * IPV6_LEN)));
				i++;
			} else {
				continue;
			}
		}
	}
	fclose(f);

	return i;
}

/*
 * @check_ip: 01: check if ipv4 exists
 *            10: check if ipv6 exists
 *            11: check if ipv4 and ipv6 are exists
 * @return: 01: check that ipv4 exists
 *          10: check that ipv6 exists
 *          11: check that ipv4 and ipv6 are exists
 */
uint8_t is_ip_exist(char *inf, uint8_t check_ip)
{
	uint8_t vflag = 0;
	char ipaddr[INET6_ADDRSTRLEN];
	char ipv6addr[512] = {0};
	int ipv6_ret = 0, i = 0;

	if(check_ip & IPV4_ADDR_BM){
		if(!get_ipv4(inf, ipaddr)) {
			WMG_DEBUG("%s: get ipv4: %s\n", inf, ipaddr);
			vflag |= IPV4_ADDR_BM;
		} else {
			WMG_DEBUG("%s: can't get ipv4\n", inf);
		}
	}

	if(check_ip & IPV6_ADDR_BM){
		ipv6_ret = get_ipv6(inf, ipv6addr, 4);
		if(ipv6_ret > 0) {
			for(; i < ipv6_ret; i++) {
				if(strstr((ipv6addr + i * IPV6_LEN), "fe80")) {
					WMG_DEBUG("%s: get local link ipv6: %s\n", inf, ipv6addr + i * IPV6_LEN);
				} else {
					WMG_DEBUG("%s: get global ipv6: %s\n", inf, ipv6addr + i * IPV6_LEN);
					vflag |= IPV6_ADDR_BM;
				}
			}
		} else {
			WMG_DEBUG("%s: can't get ipv6\n", inf);
		}
	}

	return vflag;
}

/* Per-interface DHCP client ownership.
 *
 * A global killall terminates clients of other interfaces and provides no
 * ownership. These helpers match /proc entries by comm and by an exact
 * "-i <inf>" argument pair, optionally restricted to one address family. */
/* Check for "-i <inf>" as an exact argument pair, walking the ORIGINAL
 * NUL-separated argv from /proc/<pid>/cmdline. Flattening NUL into spaces
 * would let a single argument that contains spaces (e.g. the script path
 * "/tmp/hook -i wlan0") forge a "-i <inf>" pair (NI-006). */
static int cmdline_has_interface(const char *cmdline, size_t len, const char *inf)
{
	size_t i = 0;
	size_t inf_len = strlen(inf);

	while (i < len) {
		const char *arg = cmdline + i;
		size_t arg_len = 0;

		while (i + arg_len < len && arg[arg_len] != '\0') {
			arg_len++;
		}

		if (arg_len == 2 && arg[0] == '-' && arg[1] == 'i') {
			i += arg_len + 1; /* skip this argument and its NUL */
			if (i < len) {
				const char *val = cmdline + i;
				size_t val_len = 0;
				while (i + val_len < len && val[val_len] != '\0') {
					val_len++;
				}
				if (val_len == inf_len && memcmp(val, inf, inf_len) == 0) {
					return 1;
				}
			}
			return 0;
		}
		i += arg_len + 1;
	}
	return 0;
}

static int comm_matches_ip_type(const char *comm, uint8_t ip_type)
{
	int want_v4 = (ip_type == 0) || (ip_type & IPV4_ADDR_BM);
	int want_v6 = (ip_type == 0) || (ip_type & IPV6_ADDR_BM);

	if (want_v4 && strcmp(comm, "udhcpc") == 0) {
		return 1;
	}
	if (want_v6 && strcmp(comm, "udhcpc6") == 0) {
		return 1;
	}
	return 0;
}

/* Terminate only the DHCP clients bound to this interface and address family.
 * A global killall would stop clients of other interfaces. */
static void kill_udhcpc_for_inf(const char *inf, uint8_t ip_type)
{
	DIR *dir;
	struct dirent *ent;
	char path[64];
	char comm[32];
	char cmdline[512];
	pid_t pids[16];
	int cnt = 0;
	int i, r, alive;

	if (inf == NULL || inf[0] == '\0') {
		return;
	}

	dir = opendir("/proc");
	if (dir == NULL) {
		WMG_WARNG("opendir /proc failed: %s\n", strerror(errno));
		return;
	}

	while ((ent = readdir(dir)) != NULL) {
		pid_t pid;
		FILE *fp;
		size_t n;

		if (ent->d_type != DT_DIR) {
			continue;
		}
		pid = (pid_t)atoi(ent->d_name);
		if (pid <= 1) {
			continue;
		}

		/* Match by process name so the transient shell launcher is not
		 * mistaken for the client process. */
		snprintf(path, sizeof(path), "/proc/%d/comm", pid);
		fp = fopen(path, "r");
		comm[0] = '\0';
		if (fp != NULL) {
			if (fgets(comm, sizeof(comm), fp) != NULL) {
				size_t cl = strlen(comm);
				if (cl > 0 && comm[cl - 1] == '\n') {
					comm[cl - 1] = '\0';
				}
			}
			fclose(fp);
		}
		if (!comm_matches_ip_type(comm, ip_type)) {
			continue;
		}

		/* Confirm the client is bound to this interface. */
		snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
		fp = fopen(path, "r");
		if (fp == NULL) {
			continue;
		}
		n = fread(cmdline, 1, sizeof(cmdline) - 1, fp);
		fclose(fp);
		if (n <= 0) {
			continue;
		}
		cmdline[n] = '\0';
		/* Keep the original NUL argument boundaries for the matcher. */
		if (!cmdline_has_interface(cmdline, n, inf)) {
			continue;
		}

		if (cnt < (int)(sizeof(pids) / sizeof(pids[0]))) {
			pids[cnt++] = pid;
		}
	}
	closedir(dir);

	if (cnt == 0) {
		return;
	}

	/* Ask the clients to exit, then wait and force kill if needed. */
	for (i = 0; i < cnt; i++) {
		WMG_INFO("stop dhcp client pid %d on %s\n", (int)pids[i], inf);
		kill(pids[i], SIGTERM);
	}
	for (r = 0; r < 10; r++) {
		alive = 0;
		for (i = 0; i < cnt; i++) {
			if (pids[i] > 0 && kill(pids[i], 0) == 0) {
				alive++;
			} else {
				pids[i] = -1;
			}
		}
		if (alive == 0) {
			break;
		}
		usleep(100000);
	}
	for (i = 0; i < cnt; i++) {
		if (pids[i] > 0 && kill(pids[i], 0) == 0) {
			WMG_WARNG("dhcp client pid %d not exiting, force kill\n", (int)pids[i]);
			kill(pids[i], SIGKILL);
		}
	}
}

/* WR-015: one DHCP owner transaction for the STA session.
 *
 * The monitor CONNECTED event and the enable-time compensation are two
 * different threads, and neither the config-session lock nor the single
 * wpa command lock covers DHCP. A single mutex serialises start/stop, a
 * generation counter lets an in-flight start notice a concurrent stop,
 * and a blocked interface name makes any late event after the final stop
 * fail instead of spawning a client that outlives the session. */
static pthread_mutex_t dhcp_txn_mutex = PTHREAD_MUTEX_INITIALIZER;
static char dhcp_blocked_inf[IFNAMSIZ] = {0};
static unsigned int dhcp_generation = 0;

void dhcp_owner_session_begin(const char *inf)
{
	if (inf == NULL || inf[0] == '\0') {
		return;
	}
	pthread_mutex_lock(&dhcp_txn_mutex);
	if (strncmp(dhcp_blocked_inf, inf, IFNAMSIZ) == 0) {
		dhcp_blocked_inf[0] = '\0';
	}
	pthread_mutex_unlock(&dhcp_txn_mutex);
}

void dhcp_owner_session_end(const char *inf)
{
	if (inf == NULL || inf[0] == '\0') {
		return;
	}
	/* Set the gate BEFORE stopping the producer (monitor), so no event
	 * already in flight can start a client after the final stop. */
	pthread_mutex_lock(&dhcp_txn_mutex);
	strncpy(dhcp_blocked_inf, inf, IFNAMSIZ - 1);
	dhcp_blocked_inf[IFNAMSIZ - 1] = '\0';
	dhcp_generation++;
	pthread_mutex_unlock(&dhcp_txn_mutex);
}

static int dhcp_txn_is_current(unsigned int gen)
{
	int current;

	pthread_mutex_lock(&dhcp_txn_mutex);
	current = (dhcp_generation == gen);
	pthread_mutex_unlock(&dhcp_txn_mutex);
	return current;
}

wmg_status_t start_udhcpc(char *inf, uint8_t start_ip_type)
{
	int times = 0;
	char cmd[256] = {0};
	unsigned int gen;

	if(inf == NULL) {
		WMG_ERROR("start udhcpc inf is NULL");
		return WMG_STATUS_INVALID;
	}

	WMG_INFO("start_udhcpc: inf=%s, type=%d\n", inf, start_ip_type);

	pthread_mutex_lock(&dhcp_txn_mutex);
	if (dhcp_blocked_inf[0] != '\0' &&
	    strncmp(dhcp_blocked_inf, inf, IFNAMSIZ) == 0) {
		pthread_mutex_unlock(&dhcp_txn_mutex);
		WMG_WARNG("start_udhcpc: session for %s is closed, drop late event\n", inf);
		return WMG_STATUS_FAIL;
	}
	gen = dhcp_generation;

	/* Reap any stale DHCP client bound to this interface so each (re)connect
	 * keeps a single, unambiguous owner and clients do not accumulate. */
	kill_udhcpc_for_inf(inf, start_ip_type);

	/* start udhcpc */
	if((start_ip_type == 0) || (start_ip_type & IPV4_ADDR_BM)){
		/* busybox udhcpc 1.36.1 has no daemonize feature; must background with & to stay alive for lease renewal
		 * -x 0x3d:$CID: Client Identifier (Option 61), built from MAC so the DHCP server assigns the same IP per device unique id */
		sprintf(cmd, "MAC=$(cat /sys/class/net/%s/address); CID=01$(echo $MAC | tr -d ':'); udhcpc -i %s -n -S -x 0x3d:$CID &", inf, inf);
		WMG_INFO("start_udhcpc: executing '%s'\n", cmd);
		wmg_system(cmd, NULL, 0);
	}

	/* start udhcpc6 */
	if((start_ip_type == 0) || (start_ip_type & IPV6_ADDR_BM)){
		sprintf(cmd, "udhcpc6 -i %s -n -S &", inf);
		wmg_system(cmd, NULL, 0);
	}
	pthread_mutex_unlock(&dhcp_txn_mutex);

	/* Wait for the lease outside the lock (it can take up to 10s), but
	 * abandon the transaction if a concurrent stop/session end bumped the
	 * generation. Otherwise a slow start could report success and leave a
	 * client behind after disable has already stopped DHCP. */
	if(start_ip_type == 0) {
		/* start_ip_type == 0 means "either IPv4 or IPv6 is enough"
		 * (see try_start_udhcpc(): ip_need == 0 only skips DHCP when
		 * ip_exist_flag == 0). Wait while NO address exists; the old
		 * "!= 0" was inverted and would loop while an IP already
		 * existed, then return success immediately with no lease.
		 * Note: IPV4_ADDR_BM | IPV6_ADDR_BM is bitwise OR (=3);
		 * || is logical OR (=1); do not misuse. */
		while((is_ip_exist(inf, IPV4_ADDR_BM | IPV6_ADDR_BM ) == 0) && (times < 50)) {
			if (!dhcp_txn_is_current(gen)) {
				WMG_WARNG("start_udhcpc: %s superseded by stop\n", inf);
				stop_udhcpc(inf, start_ip_type);
				return WMG_STATUS_FAIL;
			}
			usleep(200000);
			times++;
		}
	} else {
		while((is_ip_exist(inf, start_ip_type) != start_ip_type) && (times < 50)) {
			if (!dhcp_txn_is_current(gen)) {
				WMG_WARNG("start_udhcpc: %s superseded by stop\n", inf);
				stop_udhcpc(inf, start_ip_type);
				return WMG_STATUS_FAIL;
			}
			usleep(200000);
			times++;
		}
	}

	if (times >= 50 || !dhcp_txn_is_current(gen)) {
		WMG_ERROR("udhcpc %s timeout or superseded\n", inf);
		return WMG_STATUS_FAIL;
	}
	return WMG_STATUS_SUCCESS;
}

void stop_udhcpc(char *inf, uint8_t stop_ip_type)
{
	char cmd[256] = {0};
	if(inf == NULL || inf[0] == '\0') {
		return;
	}

	/* Bump the generation under the same mutex as start so any in-flight
	 * start aborts instead of leaving a client alive after this stop. */
	pthread_mutex_lock(&dhcp_txn_mutex);
	dhcp_generation++;
	/* Kill only the DHCP clients owned by this interface instead of a global
	 * "killall udhcpc", which would also stop clients of other interfaces and
	 * provides no per-interface ownership. */
	if((stop_ip_type == 0) || (stop_ip_type & IPV4_ADDR_BM) || (stop_ip_type & IPV6_ADDR_BM)) {
		kill_udhcpc_for_inf(inf, stop_ip_type);
	}
	pthread_mutex_unlock(&dhcp_txn_mutex);

	if((stop_ip_type == 0) || (stop_ip_type & IPV4_ADDR_BM)){
		sprintf(cmd, "ifconfig %s 0.0.0.0", inf);
		wmg_system(cmd, NULL, 0);
	}
	if((stop_ip_type == 0) || (stop_ip_type & IPV6_ADDR_BM)){
		/* Force the kernel to drop all IPv6 addresses and re-run SLAAC
		 * by toggling disable_ipv6; this avoids depending on busybox
		 * having the 'ip' applet. */
		memset(cmd, 0, sizeof(cmd));
		sprintf(cmd, "echo 1 > /proc/sys/net/ipv6/conf/%s/disable_ipv6", inf);
		wmg_system(cmd, NULL, 0);
		memset(cmd, 0, sizeof(cmd));
		sprintf(cmd, "echo 0 > /proc/sys/net/ipv6/conf/%s/disable_ipv6", inf);
		wmg_system(cmd, NULL, 0);
	}
}
