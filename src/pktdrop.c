// SPDX-License-Identifier: GPL-2.0
/* Packet analysis program
 * - dropped packets via an ebpf program on kfree_skb.
 *
 * Copyright (c) 2019-2020 David Ahern <dsahern@gmail.com>
 */
#include <linux/bpf.h>
#include <linux/if_arp.h>
#include <linux/ipv6.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <libgen.h>

#include <bpf/bpf.h>

#include "pktdrop.h"
#include "flow.h"
#include "libbpf_helpers.h"
#include "ksyms.h"
#include "perf_events.h"
#include "str_utils.h"
#include "timestamps.h"

#include "perf_events.c"

static __u64 display_rate = 10 * NSEC_PER_SEC;
static __u64 t_last_display;
static unsigned int drop_thresh = 1;
static unsigned int do_hist;
static const char *hist_sort;
static unsigned int nsid;
static struct ksym_s *ovs_sym;
static bool skip_ovs_upcalls;
static bool skip_unix;
static bool skip_tcp;
static bool done;
static bool debug;

enum {
	HIST_BY_NONE,
	HIST_BY_NETNS,
	HIST_BY_DMAC,
	HIST_BY_SMAC,
	HIST_BY_DIP,
	HIST_BY_SIP,
	HIST_BY_FLOW,
};

enum {
	HIST_LLDP,
	HIST_ARP,
	HIST_ARP_REQ,
	HIST_ARP_REPLY,
	HIST_ARP_OTHER,
	HIST_IPV4,
	HIST_IPV6,
	HIST_TCP,
	HIST_TCP_SYN,
	HIST_TCP_RST,
	HIST_TCP_FIN,
	HIST_UDP,
	HIST_VRRP,
	HIST_OTHER,
	HIST_MAX,
};

struct flow_entry {
	struct list_head list;
	unsigned int	hits;
	__u8		aging;
	struct flow	flow;
};

#define MAX_FLOW_ENTRIES  25
struct flow_buckets {
	struct list_head flows;

	__u8		flow_count;
	bool		overflow;
	bool		failures;
};

struct drop_hist {
	struct rb_node rb_node;
	union {
		unsigned long	addr;
		__u8		dmac[8];  /* 8 > ETH_ALEN */
	};
	char		name[16];
	unsigned int	total_drops;
	__u8		aging;
	bool		dead;
	union {
		unsigned int		buckets[HIST_MAX];
		struct flow_buckets	flb;
	};
};

struct drop_loc {
	struct rb_node	rb_node;
	unsigned long	addr;
	char		name[64];
	unsigned int	total_drops;
	__u8		aging;
	bool		dead;
};

static struct rb_root all_drop_hists, all_drop_loc;
static unsigned int total_drops;
static unsigned int total_drops_unix;

#define PKT_TYPE_MAX    7   /* used as a mask */
static unsigned int total_drops_by_type[PKT_TYPE_MAX + 1];
static const char *drop_by_type_str[PKT_TYPE_MAX + 1] = {
	[PACKET_HOST]		= "this-host",
	[PACKET_BROADCAST]	= "broadcast",
	[PACKET_MULTICAST]	= "multicast",
	[PACKET_OTHERHOST]	= "other-host",
	[PACKET_OUTGOING]	= "outgoing",
	[PACKET_LOOPBACK]	= "loopback",
	[PACKET_USER]		= "to-user",
	[PACKET_KERNEL]		= "to-kernel",
};

static struct {
	const char *str;
	bool skip;
} hist_desc[] = {
	[HIST_LLDP]      = { .str = "LLDP" },
	[HIST_ARP]       = { .str = "ARP" },
	[HIST_ARP_REQ]   = { .str = "ARP req" },
	[HIST_ARP_REPLY] = { .str = "ARP reply" },
	[HIST_ARP_OTHER] = { .str = "ARP other" },
	[HIST_IPV4]      = { .str = "IPv4" },
	[HIST_IPV6]      = { .str = "IPv6" },
	[HIST_TCP]       = { .str = "TCP" },
	[HIST_TCP_SYN]   = { .str = "TCP syn" },
	[HIST_TCP_RST]   = { .str = "TCP reset" },
	[HIST_TCP_FIN]   = { .str = "TCP fin" },
	[HIST_UDP]       = { .str = "UDP" },
	[HIST_VRRP]      = { .str = "VRRP" },
	[HIST_OTHER]     = { .str = "other" },
};

static struct drop_hist *new_droph(unsigned long addr)
{
	struct drop_hist *droph = calloc(1, sizeof(struct drop_hist));

	if (!droph)
		return NULL;

	droph->addr = addr;

	if (debug)
		printf("new droph: ");
	if (do_hist == HIST_BY_NETNS) {

		if (!addr) {
			strcpy(droph->name, "<unknown>");
		} else {
			struct ksym_s *sym = find_ksym(addr);

			if (sym)
				strncpy(droph->name, sym->name,
					sizeof(droph->name)-1);
			else
				snprintf(droph->name, sizeof(droph->name),
					 "netns-%d", nsid++);
		}
		if (debug)
			printf("%s %lx\n", droph->name, addr);
	} else if (do_hist == HIST_BY_FLOW) {
		INIT_LIST_HEAD(&droph->flb.flows);
	} else if (debug) {
		print_mac((__u8 *)&addr, true);
		printf("\n");
	}

	return droph;
}

static void remove_droph(struct drop_hist *droph)
{
	struct rb_root *rb_root = &all_drop_hists;

	rb_erase(&droph->rb_node, rb_root);

	if (do_hist == HIST_BY_FLOW) {
		struct flow_buckets *flb = &droph->flb;
		struct flow_entry *fl_entry, *fl_next;

		list_for_each_entry_safe(fl_entry, fl_next, &flb->flows, list) {
			list_del(&fl_entry->list);
			free(fl_entry);
		}
	}

	free(droph);
}

static int insert_droph(struct drop_hist *new_entry)
{
	struct rb_root *rb_root = &all_drop_hists;
        struct rb_node **node = &rb_root->rb_node;
        struct rb_node *parent = NULL;

	while (*node != NULL) {
		struct drop_hist *entry;

		parent = *node;
		entry = container_of(parent, struct drop_hist, rb_node);
		if (new_entry->addr < entry->addr)
			node = &(*node)->rb_left;
		else if (new_entry->addr > entry->addr)
			node = &(*node)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&new_entry->rb_node, parent, node);
	rb_insert_color(&new_entry->rb_node, rb_root);

	return 0;
}

static struct drop_hist *find_droph(unsigned long addr, bool create)
{
	struct rb_root *rb_root = &all_drop_hists;
	struct rb_node **node = &rb_root->rb_node;
	struct rb_node *parent = NULL;
	struct drop_hist *droph;

	while (*node != NULL) {
		parent = *node;

		droph = container_of(parent, struct drop_hist, rb_node);
		if (addr < droph->addr)
			node = &(*node)->rb_left;
		else if (addr > droph->addr)
			node = &(*node)->rb_right;
		else
			return droph;
	}

	if (!create)
		return NULL;

	droph = new_droph(addr);
	if (droph && insert_droph(droph)) {
		free(droph);
		droph = NULL;
	}

	return droph;
}

static struct drop_loc *new_dropl(void)
{
	return calloc(1, sizeof(struct drop_loc));
}

static void remove_dropl(struct drop_loc *dropl)
{
	struct rb_root *rb_root = &all_drop_loc;

	rb_erase(&dropl->rb_node, rb_root);
	free(dropl);
}

static int insert_dropl(struct drop_loc *new_entry)
{
	struct rb_root *rb_root = &all_drop_loc;
        struct rb_node **node = &rb_root->rb_node;
        struct rb_node *parent = NULL;

	while (*node != NULL) {
		struct drop_loc *entry;

		parent = *node;
		entry = container_of(parent, struct drop_loc, rb_node);
		if (new_entry->addr < entry->addr)
			node = &(*node)->rb_left;
		else if (new_entry->addr > entry->addr)
			node = &(*node)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&new_entry->rb_node, parent, node);
	rb_insert_color(&new_entry->rb_node, rb_root);

	return 0;
}

static struct drop_loc *find_dropl(unsigned long addr, const char *name)
{
	struct rb_root *rb_root = &all_drop_loc;
	struct rb_node **node = &rb_root->rb_node;
	struct rb_node *parent = NULL;
	struct drop_loc *dropl;

	while (*node != NULL) {
		parent = *node;

		dropl = container_of(parent, struct drop_loc, rb_node);
		if (addr < dropl->addr)
			node = &(*node)->rb_left;
		else if (addr > dropl->addr)
			node = &(*node)->rb_right;
		else
			return dropl;
	}

	dropl = new_dropl();
	if (dropl) {
		dropl->addr = addr;
		if (name)
			strncpy(dropl->name, name, sizeof(dropl->name)-1);

		if (insert_dropl(dropl)) {
			free(dropl);
			dropl = NULL;
		}
	}

	return dropl;
}

static void hist_disable_non_ipv4(void)
{
	hist_desc[HIST_LLDP].skip = true;
	hist_desc[HIST_ARP].skip = true;
	hist_desc[HIST_ARP_REQ].skip = true;
	hist_desc[HIST_ARP_REPLY].skip = true;
	hist_desc[HIST_ARP_OTHER].skip = true;
	hist_desc[HIST_IPV6].skip = true;
}

static void show_loc_entries(void)
{
	struct rb_root *rb_root = &all_drop_loc;
	struct drop_loc *dropl;
	struct rb_node *node;

	printf("\n");
	for (node = rb_first(rb_root); node; node = rb_next(node)) {

		dropl = container_of(node, struct drop_loc, rb_node);

		if (dropl->total_drops)
			printf("%32s: %10u\n", dropl->name, dropl->total_drops);

		if (dropl->total_drops)
			dropl->aging = 3;
		else if (--dropl->aging == 0)
			dropl->dead = true;

		dropl->total_drops = 0;
	}

	/* remove dead entries - must be a better way to do this */
restart:
	for (node = rb_first(rb_root); node; node = rb_next(node)) {
		dropl = container_of(node, struct drop_loc , rb_node);
		if (dropl->dead) {
			remove_dropl(dropl);
			goto restart;
		}
	}
}

static void show_flow_buckets(void)
{
	struct rb_root *rb_root = &all_drop_hists;
	struct drop_hist *droph;
	struct rb_node *node;

	for (node = rb_first(rb_root); node; node = rb_next(node)) {
		struct flow_entry *fl_entry, *fl_next;
		struct flow_buckets *flb;
		bool show_buckets;

		droph = container_of(node, struct drop_hist, rb_node);
		flb = &droph->flb;

		show_buckets = (droph->total_drops >= drop_thresh);
		list_for_each_entry_safe(fl_entry, fl_next, &flb->flows, list) {
			if (show_buckets && fl_entry->hits) {
				printf("    hits %4u:   ", fl_entry->hits);
				print_flow(&fl_entry->flow);
			}

			if (fl_entry->hits) {
				fl_entry->aging = 3;
			} else if (--fl_entry->aging == 0) {
				list_del(&fl_entry->list);
				free(fl_entry);
			}

			fl_entry->hits = 0;
		}
		if (flb->overflow)
			printf("too many flow entries for bucket\n");
		if (flb->failures)
			printf("failures processing entry\n");

		if (show_buckets)
			printf("\n");

		if (droph->total_drops)
			droph->aging = 3;
		else if (--droph->aging == 0)
			droph->dead = true;

		droph->total_drops = 0;
	}
}

static void show_hist_buckets(void)
{
	struct rb_root *rb_root = &all_drop_hists;
	struct drop_hist *droph;
	struct rb_node *node;
	char buf[64];
	int i;

	for (node = rb_first(rb_root); node; node = rb_next(node)) {
		droph = container_of(node, struct drop_hist, rb_node);

		if (droph->total_drops < drop_thresh)
			goto do_aging;

		printf("  ");
		switch(do_hist) {
		case HIST_BY_NETNS:
			printf("%10s%c", droph->name, droph->dead ? '*' : ' ');
			break;
		case HIST_BY_DMAC:
		case HIST_BY_SMAC:
			print_mac(droph->dmac, true);
			break;
		case HIST_BY_DIP:
		case HIST_BY_SIP:
			inet_ntop(AF_INET, &droph->addr, buf, sizeof(buf));
			printf("%17s ", buf);
			break;
		}

		for (i = 0; i < HIST_MAX; i++) {
			if (!hist_desc[i].skip)
				printf("  %10u", droph->buckets[i]);
			droph->buckets[i] = 0;
		}
		printf("  %10u\n", droph->total_drops);

do_aging:
		if (droph->total_drops)
			droph->aging = 3;
		else if (--droph->aging == 0)
			droph->dead = true;

		droph->total_drops = 0;
	}
}

static void cleanup_hist_buckets(void)
{
	struct rb_root *rb_root = &all_drop_hists;
	struct drop_hist *droph;
	struct rb_node *node;

	/* remove dead entries - must be a better way to do this */
restart:
	for (node = rb_first(rb_root); node; node = rb_next(node)) {
		droph = container_of(node, struct drop_hist, rb_node);
		if (droph->dead) {
			remove_droph(droph);
			goto restart;
		}
	}
}

static void show_hist(void)
{
	char buf[64];
	int i;

	printf("\n%s: sort by %s,", timestamp(buf, sizeof(buf), 0), hist_sort);
	printf(" total drops: %u (unix sockets %u):\n",
		total_drops, total_drops_unix);
	total_drops = 0;
	total_drops_unix = 0;

	/* name column */
	switch(do_hist) {
	case HIST_BY_DMAC:
	case HIST_BY_SMAC:
	case HIST_BY_DIP:
	case HIST_BY_SIP:
		printf("    %17s", "");
		break;
	case HIST_BY_FLOW:
		break;
	default:
		printf("    %10s", "");
	}

	if (do_hist == HIST_BY_FLOW) {
		show_flow_buckets();
	} else {
		for (i = 0; i < HIST_MAX; i++) {
			if (!hist_desc[i].skip)
				printf("  %10s", hist_desc[i].str);
		}
		printf("  %10s\n", "total");

		show_hist_buckets();
	}

	printf("\n  drops by packet type: ");
	for (i = 0; i <= PKT_TYPE_MAX; ++i) {
		printf("  %s: %u", drop_by_type_str[i], total_drops_by_type[i]);
		total_drops_by_type[i] = 0;
	}
	printf("\n");

	show_loc_entries();

	cleanup_hist_buckets();
}

static void process_tcp(unsigned int *buckets, const struct flow_tcp *flt)
{
	if (flt->fin)
		buckets[HIST_TCP_FIN]++;
	else if (flt->rst)
		buckets[HIST_TCP_RST]++;
	else if (flt->syn)
		buckets[HIST_TCP_SYN]++;
}

static void process_transport(unsigned int *buckets,
			      const struct flow_transport *flt)
{
	switch(flt->proto) {
	case IPPROTO_TCP:
		buckets[HIST_TCP]++;
		process_tcp(buckets, &flt->tcp);
		break;
	case IPPROTO_UDP:
		buckets[HIST_UDP]++;
		break;
	case IPPROTO_VRRP:
		buckets[HIST_VRRP]++;
		break;
	}
}

static void process_ipv6(unsigned int *buckets, const struct flow_ip6 *fl6)
{
	buckets[HIST_IPV6]++;
	process_transport(buckets, &fl6->trans);
}

static void process_ipv4(unsigned int *buckets, const struct flow_ip4 *fl4)
{
	buckets[HIST_IPV4]++;
	process_transport(buckets, &fl4->trans);
}

static void process_arp(unsigned int *buckets, const struct flow_arp *fla)
{
	buckets[HIST_ARP]++;

	switch(fla->op) {
	case ARPOP_REQUEST:
		buckets[HIST_ARP_REQ]++;
		break;
	case ARPOP_REPLY:
		buckets[HIST_ARP_REPLY]++;
		break;
	default:
		buckets[HIST_ARP_OTHER]++;
		break;
	}
}

static void process_flow(struct flow_buckets *flb, struct flow *flow)
{
	struct flow_entry *fl_entry;
	bool found = false;

	list_for_each_entry(fl_entry, &flb->flows, list) {
		if (!memcmp(&fl_entry->flow, flow, sizeof(*flow))) {
			found = true;
			break;
		}
	}

	if (!found) {
		if (flb->flow_count > MAX_FLOW_ENTRIES) {
			flb->overflow = true;
			return;
		}
		fl_entry = calloc(1, sizeof(*fl_entry));
		if (!fl_entry) {
			flb->failures = true;
			return;
		}
		memcpy(&fl_entry->flow, flow, sizeof(*flow));
		list_add(&fl_entry->list, &flb->flows);
	}
	fl_entry->hits++;
}

static void process_exit(struct data *data)
{
	struct drop_hist *droph;

	droph = find_droph(data->netns, false);
	if (droph) {
		printf("droph %s/%lx is dead\n", droph->name, droph->addr);
		droph->dead = true;
	}
}

static void do_histogram(struct flow *fl, __u64	netns)
{
	struct drop_hist *droph;
	unsigned long addr = 0;
	__u8 *p = (__u8 *)&addr, i;

	switch(do_hist) {
	case HIST_BY_NETNS:
		addr = netns;
		break;
	case HIST_BY_FLOW:   /* histogram by flow managed by dmac */
	case HIST_BY_DMAC:
		for (i = 0; i < 6; ++i)
			p[i] = fl->dmac[5-i];
		break;
	case HIST_BY_SMAC:
		for (i = 0; i < 6; ++i)
			p[i] = fl->smac[5-i];
		break;
	case HIST_BY_DIP:
	case HIST_BY_SIP:
		if (fl->proto != ETH_P_IP)
			return;
		if (do_hist == HIST_BY_DIP)
			memcpy(&addr, &fl->ip4.daddr, 4);
		else
			memcpy(&addr, &fl->ip4.saddr, 4);
		break;
	default:
		return;
	}

	droph = find_droph(addr, true);
	if (!droph) {
		fprintf(stderr, "failed to allocate droph for addr %lx\n",
			addr);
		return;
	}

	droph->total_drops++;

	if (do_hist == HIST_BY_FLOW) {
		process_flow(&droph->flb, fl);
		return;
	}

	switch(fl->proto) {
	case ETH_P_ARP:
		process_arp(droph->buckets, &fl->arp);
		break;
        case ETH_P_IP:
		process_ipv4(droph->buckets, &fl->ip4);
		break;
        case ETH_P_IPV6:
		process_ipv6(droph->buckets, &fl->ip6);
		break;
        case ETH_P_LLDP:
		droph->buckets[HIST_LLDP]++;
		break;
	default:
		droph->buckets[HIST_OTHER]++;
		break;
	}
}

static void process_packet(struct data *data, struct ksym_s *sym)
{
	struct drop_loc *dropl;
	struct flow fl = {};

	total_drops++;
	total_drops_by_type[data->pkt_type & PKT_TYPE_MAX]++;

	dropl = find_dropl(data->location, sym ? sym->name : NULL);
	if (dropl)
		dropl->total_drops++;

	if (sym && sym->is_unix) {
		total_drops_unix++;
		return;
	}

	if (data->vlan_tci) {
		fl.has_vlan = true;
		fl.vlan.outer_vlan_TCI = data->vlan_tci;
	}
	if (parse_pkt(&fl, data->protocol, data->pkt_data, data->pkt_len)) {
		fprintf(stderr, "Failed to parse packet\n");
		return;
	}

	do_histogram(&fl, data->netns);
}

static struct ksym_s *find_ksym_droph(unsigned long addr)
{
	struct ksym_s *sym;

	if (!addr)
		return NULL;

	sym = find_ksym(addr);
	if (!sym) {
		char buf[16];

		snprintf(buf, sizeof(buf), "droph-%d", ++nsid);
		sym = new_ksym(addr, buf, "[kernel]");
		if (insert_ksym(sym)) {
			free_ksym(sym);
			sym = NULL;
		}
	}

	return sym;
}

static void show_packet(struct data *data, struct ksym_s *sym)
{
	__u8 pkt_type = data->pkt_type & PKT_TYPE_MAX;
	struct ksym_s *symns;
	bool is_unix;
	char buf[64];
	__u32 len;

	printf("%15s  %3u  ",
	       timestamp(buf, sizeof(buf), data->time), data->ifindex);

	printf("%12s  ", drop_by_type_str[pkt_type]);

	symns = find_ksym_droph(data->netns);
	if (symns)
		printf("%10s", symns->name);
	else
		printf("%llx", data->netns);

	printf("  %3u  %3u  %3u  ",
		data->pkt_len, data->nr_frags, data->gso_size);

	if (sym) {
		__u64 offset = data->location - sym->addr;

		printf("%s+0x%llx (%llx)\n",
		       sym->name, offset, data->location);
		is_unix = sym->is_unix;
	} else {
		printf("%llx\n", data->location);
		is_unix = false;
	}

	len = data->pkt_len;
	if (len > sizeof(data->pkt_data))
		len = sizeof(data->pkt_data);

	if (data->protocol || !is_unix) {
		struct flow fl = {};

		if (data->vlan_tci) {
			fl.has_vlan = true;
			fl.vlan.outer_vlan_TCI = data->vlan_tci;
		}
		if (parse_pkt(&fl, data->protocol, data->pkt_data, len))
			printf("*** failed to parse ***\n");
		else
			print_flow(&fl);
	}
	printf("\n");
}

static void process_event(struct data *data)
{
	struct ksym_s *sym;

	switch (data->event_type) {
	case EVENT_SAMPLE:
		sym = find_ksym(data->location);
		if (skip_ovs_upcalls && sym == ovs_sym)
			return;
		if (skip_unix && sym->is_unix)
			return;
		if (skip_tcp && sym->is_tcp)
			return;

		if (do_hist)
			process_packet(data, sym);
		else
			show_packet(data, sym);
		break;
	case EVENT_EXIT:
		process_exit(data);
		break;
	}
}

static int pktdrop_complete(void)
{
	process_events();

	if (do_hist) {
		__u64 t_mono = get_time_ns(CLOCK_MONOTONIC);

		if (t_mono > t_last_display + display_rate) {
			t_last_display = t_mono;
			show_hist();
		}
	}
	return done;
}

static int check_sort_arg(const char *arg)
{
	if (strcmp(optarg, "netns") == 0) {
		hist_sort = "network namespace";
		do_hist = HIST_BY_NETNS;
	} else if (strcmp(optarg, "dmac") == 0) {
		hist_sort = "destination mac";
		do_hist = HIST_BY_DMAC;
	} else if (strcmp(optarg, "smac") == 0) {
		hist_sort = "source mac";
		do_hist = HIST_BY_SMAC;
	} else if (strcmp(optarg, "dip") == 0) {
		hist_sort = "destination ip";
		do_hist = HIST_BY_DIP;
	} else if (strcmp(optarg, "sip") == 0) {
		hist_sort = "source ip";
		do_hist = HIST_BY_SIP;
	} else if (strcmp(optarg, "flow") == 0) {
		hist_sort = "dmac and flow";
		do_hist = HIST_BY_FLOW;
	} else {
		fprintf(stderr, "Invalid sort option\n");
		return 1;
	}
	return 0;
}

static void sig_handler(int signo)
{
	printf("Terminating by signal %d\n", signo);
	done = true;
}

static void print_dropmon_usage(const char *prog)
{
	printf(
	"usage: %s OPTS\n\n"
	"	-f bpf-file    bpf filename to load\n"
	"	-i             ignore kprobe error (4.14 can not install kprobe on fib_net_exit)\n"
	"	-k kallsyms    load kernel symbols from this file\n"
	"	-m count       set number of pages in perf buffers\n"
	"	-O             ignore ovs upcalls\n"
	"	-r rate        display rate (seconds) to dump summary\n"
	"	-s <type>      show summary by type (netns, dmac, smac, dip, sip, flow)\n"
	"	-t num         only display entries with drops more than num\n"
	"	-T             ignore tcp drops\n"
	"	-U             ignore unix drops\n"
	, prog);
}

static int drop_monitor(const char *prog, int argc, char **argv)
{
	struct bpf_prog_load_attr prog_load_attr = { };
	const char *kallsyms = "/proc/kallsyms";
	bool skip_kprobe_err = false;
	char *objfile = "pktdrop.o";
	bool filename_set = false;
	const char *probes[] = {
		"fib_net_exit",
		NULL,
	};
	const char *tps[] = {
		"skb/kfree_skb",
		NULL,
	};
	struct bpf_object *obj;
	int nevents = 1000;
	int pg_cnt = 0;
	int rc, r;

	while ((rc = getopt(argc, argv, "f:ik:m:Or:s:t:TU")) != -1)
	{
		switch(rc) {
		case 'f':
			objfile = optarg;
			filename_set = true;
			break;
		case 'i':
			skip_kprobe_err = true;
			break;
		case 'k':
			kallsyms = optarg;
			break;
		case 'm':
			if (str_to_int(optarg, 64, 32768, &pg_cnt)) {
				fprintf(stderr, "Invalid page count\n");
				return 1;
			}
			break;
		case 'O':
			skip_ovs_upcalls = true;
			break;
		case 'r':
			r = atoi(optarg);
			if (!r) {
				fprintf(stderr, "Invalid display rate\n");
				return 1;
			}
			display_rate = r * NSEC_PER_SEC;
			break;
		case 's':
			if (check_sort_arg(optarg))
				return 1;
			break;
		case 't':
			r = atoi(optarg);
			if (!r) {
				fprintf(stderr, "Invalid drop threshold\n");
				return 1;
			}
			drop_thresh = r;
			break;
		case 'T':
			skip_tcp = true;
			break;
		case 'U':
			skip_unix = true;
			break;
		default:
			print_dropmon_usage(prog);
			return 1;
		}
	}

	if (pg_cnt)
		perf_set_page_cnt(pg_cnt);

	if (set_reftime())
		return 1;

	if (load_ksyms(kallsyms))
		return 1;

	ovs_sym = find_ksym_by_name("queue_userspace_packet");
	if (skip_ovs_upcalls && !ovs_sym) {
		fprintf(stderr,
			"Failed to find symbol entry for queue_userspace_packet\n");
		return 1;
	}

	if (load_obj_file(&prog_load_attr, &obj, objfile, filename_set))
		return 1;

	if (do_tracepoint(obj, tps))
		return 1;

	switch(do_hist) {
	case HIST_BY_NETNS:
		if (do_kprobe(obj, probes, 0) && !skip_kprobe_err)
			return 1;
		break;
	case HIST_BY_DIP:
	case HIST_BY_SIP:
		hist_disable_non_ipv4();
		break;
	}

	if (signal(SIGINT, sig_handler) ||
	    signal(SIGHUP, sig_handler) ||
	    signal(SIGTERM, sig_handler)) {
		perror("signal");
		return 1;
	}

	setlinebuf(stdout);
	setlinebuf(stderr);

	if (configure_perf_event_channel(obj, nevents))
		return 1;

	if (do_hist)
		t_last_display = get_time_ns(CLOCK_MONOTONIC);

	/* main event loop */
	return perf_event_loop(NULL, NULL, pktdrop_complete);
}

static const struct {
	const char *name;
	int (*fn)(const char *prog, int argc, char **argv);
} cmds[] = {
	{ .name = "drop", .fn = drop_monitor },
};

static void print_main_usage(const char *prog)
{
	fprintf(stderr, "usage: %s { drop }\n", prog);
}

int main(int argc, char **argv)
{
	const char *prog = basename(argv[0]);
	const char *cmd;
	int i;

	if (argc < 2) {
		print_main_usage(prog);
		return 1;
	}

	cmd = argv[1];
	argc--;
	argv++;

	for (i = 0; i < ARRAY_SIZE(cmds); ++i) {
		if (!strcmp(cmds[i].name, cmd))
			return cmds[i].fn(prog, argc, argv);
	}

	fprintf(stderr, "Invalid command\n");
	return 1;
}
