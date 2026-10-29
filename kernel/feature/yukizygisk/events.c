#include <linux/netlink.h>
#include <linux/rcupdate.h>
#include <linux/random.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "api.h"
#include "internal.h"
#include "uapi/yukizygisk.h"
#include "klog.h" // IWYU pragma: keep

static struct sock __rcu *yz_event_sock;
static u32 yz_event_generation;

static int yz_multicast(struct sk_buff *skb)
{
	struct sock *sock;
	int ret;

	rcu_read_lock();
	sock = rcu_dereference(yz_event_sock);
	if (sock) {
		nlmsg_hdr(skb)->nlmsg_seq = READ_ONCE(yz_event_generation);
		ret = nlmsg_multicast(sock, skb, 0, YZ_NL_GROUP_EVENTS,
				      GFP_ATOMIC);
	} else {
		nlmsg_free(skb);
		ret = -ENOTCONN;
	}
	rcu_read_unlock();
	return ret;
}

static int yz_emit_event(u32 type, u32 pid, u32 appid)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct yz_event *ev;

	skb = nlmsg_new(sizeof(*ev), GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	nlh = nlmsg_put(skb, 0, 0, YZ_NL_MSG_EVENT, sizeof(*ev), 0);
	if (!nlh) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	ev = nlmsg_data(nlh);
	ev->type = type;
	ev->pid = pid;
	ev->appid = appid;

	return yz_multicast(skb);
}

void yz_emit_specialize(u32 pid, u32 appid)
{
	yz_emit_event(YZ_EV_SPECIALIZE, pid, appid);
}

int ksu_yukizygisk_emit_reload(void)
{
	return yz_emit_event(YZ_EV_RELOAD, 0, 0);
}

void yz_emit_safemode(u32 pid, u32 crashes)
{
	yz_emit_event(YZ_EV_SAFEMODE, pid, crashes);
}

static void yz_emit_exit(const void *event, size_t size)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;

	skb = nlmsg_new(size, GFP_KERNEL);
	if (!skb)
		return;
	nlh = nlmsg_put(skb, 0, 0, YZ_NL_MSG_EVENT, size, 0);
	if (!nlh) {
		nlmsg_free(skb);
		return;
	}
	memcpy(nlmsg_data(nlh), event, size);
	(void)yz_multicast(skb);
}

void yz_emit_zygote_exit(const struct yz_zygote_exit_event *event)
{
	yz_emit_exit(event, sizeof(*event));
}

void yz_emit_target_exit(const struct yz_target_exit_event *event)
{
	yz_emit_exit(event, sizeof(*event));
}

/* Serialized by yz_feature_lock, including rollback and shutdown. */
int yz_events_init(void)
{
	struct netlink_kernel_cfg cfg = {
	    .groups = YZ_NL_GROUP_EVENTS,
	};
	struct sock *sock;

	if (rcu_access_pointer(yz_event_sock))
		return 0;
	sock = netlink_kernel_create(&init_net, YZ_NETLINK_PROTO, &cfg);
	if (!sock) {
		pr_err("yukizygisk: event netlink creation failed\n");
		return -ENODEV;
	}
	yz_event_generation = get_random_u32() ?: 1;
	rcu_assign_pointer(yz_event_sock, sock);
	pr_info("yukizygisk: event netlink ready proto=%d\n", YZ_NETLINK_PROTO);
	return 0;
}

void yz_events_exit(void)
{
	struct sock *sock = rcu_access_pointer(yz_event_sock);

	if (!sock)
		return;
	RCU_INIT_POINTER(yz_event_sock, NULL);
	synchronize_rcu();
	netlink_kernel_release(sock);
}

int yz_events_get_channel(void __user *arg)
{
	struct yz_event_channel_cmd cmd = {.version = 1};

	rcu_read_lock();
	if (rcu_dereference(yz_event_sock))
		cmd.generation = READ_ONCE(yz_event_generation);
	rcu_read_unlock();
	if (!cmd.generation)
		return -ENOTCONN;
	return copy_to_user(arg, &cmd, sizeof(cmd)) ? -EFAULT : 0;
}
