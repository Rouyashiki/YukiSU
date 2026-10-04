#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "api.h"
#include "internal.h"
#include "uapi/yukizygisk.h"
#include "klog.h" // IWYU pragma: keep

static struct sock *yz_event_sock;

static void yz_emit_event(u32 type, u32 pid, u32 appid)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct yz_event *ev;

	if (!yz_event_sock)
		return;

	skb = nlmsg_new(sizeof(*ev), GFP_ATOMIC);
	if (!skb)
		return;

	nlh = nlmsg_put(skb, 0, 0, YZ_NL_MSG_EVENT, sizeof(*ev), 0);
	if (!nlh) {
		nlmsg_free(skb);
		return;
	}

	ev = nlmsg_data(nlh);
	ev->type = type;
	ev->pid = pid;
	ev->appid = appid;

	/* A broadcast with no listeners is harmless. */
	nlmsg_multicast(yz_event_sock, skb, 0, YZ_NL_GROUP_EVENTS, GFP_ATOMIC);
}

void yz_emit_specialize(u32 pid, u32 appid)
{
	yz_emit_event(YZ_EV_SPECIALIZE, pid, appid);
}

void ksu_yukizygisk_emit_reload(void)
{
	yz_emit_event(YZ_EV_RELOAD, 0, 0);
}

void yz_emit_safemode(u32 pid, u32 crashes)
{
	yz_emit_event(YZ_EV_SAFEMODE, pid, crashes);
}

static void yz_emit_exit(const void *event, size_t size)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;

	if (!yz_event_sock)
		return;
	skb = nlmsg_new(size, GFP_KERNEL);
	if (!skb)
		return;
	nlh = nlmsg_put(skb, 0, 0, YZ_NL_MSG_EVENT, size, 0);
	if (!nlh) {
		nlmsg_free(skb);
		return;
	}
	memcpy(nlmsg_data(nlh), event, size);
	nlmsg_multicast(yz_event_sock, skb, 0, YZ_NL_GROUP_EVENTS, GFP_KERNEL);
}

void yz_emit_zygote_exit(const struct yz_zygote_exit_event *event)
{
	yz_emit_exit(event, sizeof(*event));
}

void yz_emit_target_exit(const struct yz_target_exit_event *event)
{
	yz_emit_exit(event, sizeof(*event));
}

void yz_events_init(void)
{
	struct netlink_kernel_cfg cfg = {
	    .groups = YZ_NL_GROUP_EVENTS,
	};

	yz_event_sock =
	    netlink_kernel_create(&init_net, YZ_NETLINK_PROTO, &cfg);
	if (!yz_event_sock)
		pr_err("yukizygisk: event netlink creation failed\n");
	else
		pr_info("yukizygisk: event netlink ready proto=%d\n",
			YZ_NETLINK_PROTO);
}

void yz_events_exit(void)
{
	if (yz_event_sock) {
		netlink_kernel_release(yz_event_sock);
		yz_event_sock = NULL;
	}
}
