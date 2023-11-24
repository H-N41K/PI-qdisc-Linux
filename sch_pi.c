#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <net/pkt_sched.h>
#include <net/inet_ecn.h>

#define MAX_PROB U64_MAX
#define PARAMETER_SCALE 10000000000000

/* parameters used */
struct pi_params {
	u32 qref;   /* reference queue length in packets */
	u32 w;		/* timer frequency (in jiffies) */
	u32 limit;		/* number of packets that can be enqueued */
	u64 a;		/* a and b are between 0 and 32 */
	u64 b;		/* and are used for shift relative to 1 */
	u32 ecn;		/* true if ecn is enabled */
	u32 bytemode;		/* to scale drop early prob based on pkt size */
};

/* variables used */
struct pi_vars {
	u64 prob;		/* probability but scaled by u64 limit. */
	u32 qlen_old;		/* in bytes */
};

/* statistics gathering */
struct pi_stats {
	u32 qlen;		/* instantaneous qlen in packets */
	u32 packets_in;		/* total number of packets enqueued */
	u32 dropped;		/* packets dropped due to pi_action */
	u32 overlimit;		/* dropped due to lack of space in queue */
	u32 maxq;		/* maximum queue size */
	u32 ecn_mark;		/* packets marked with ECN */
};

/* private data for the Qdisc */
struct pi_sched_data {
	struct pi_params params;
	struct pi_vars vars;
	struct pi_stats stats;
	struct timer_list adapt_timer;
	struct Qdisc *sch;
};

static void pi_params_init(struct pi_params *params)
{
	params->a = 182200000;
	params->b = 181600000;
	//DOUBT HERE usecs_to_jiffies . 625 or 6250?
	params->w = usecs_to_jiffies(625 * USEC_PER_MSEC / 1000);	/* 6.25 ms */
	params->limit = 1000;	/* default of 1000 packets */
	params->qref = 50; /* reference queue length in packets */
	params->ecn = false;
	params->bytemode = false;
}

static void pi_vars_init(struct pi_vars *vars)
{
	vars->prob = 0;
	vars->qlen_old = 0;
}


static bool drop_early(struct Qdisc *sch, u32 packet_size)
{
	struct pi_sched_data *q = qdisc_priv(sch);
	u64 rnd;
	u64 local_prob = q->vars.prob;
	u32 mtu = psched_mtu(qdisc_dev(sch));

	/* If bytemode is turned on, use packet size to compute new
	 * probablity. Smaller packets will have lower drop prob in this case
	 */
	if (q->params.bytemode && packet_size <= mtu)
		local_prob = (u64)packet_size * div_u64(local_prob, mtu);
	else
		local_prob = q->vars.prob;

	get_random_bytes(&rnd,8);

	if(rnd < local_prob){
		printk("DROPPED THE PACKET EARLY!");
		return true;
	}
		

	return false;
}

static int pi_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
			     struct sk_buff **to_free)
{
	struct pi_sched_data *q = qdisc_priv(sch);
	bool enqueue = false;

	if (unlikely(qdisc_qlen(sch) >= sch->limit)) {
		q->stats.overlimit++;
		goto out;
	}

	if (!drop_early(sch, skb->len)) {
		enqueue = true;
	} else if (q->params.ecn && INET_ECN_set_ce(skb)) {
		/* If packet is ecn capable, mark it */
		q->stats.ecn_mark++;
		enqueue = true;
	}

	/* we can enqueue the packet */
	if (enqueue) {
		q->stats.packets_in++;
		if (qdisc_qlen(sch) > q->stats.maxq)
			q->stats.maxq = qdisc_qlen(sch);

		return qdisc_enqueue_tail(skb, sch);
	}

out:
	q->stats.dropped++;
	printk("STATS DROPPED INCREMENTED to %d !\n", q->stats.dropped);
	return qdisc_drop(skb, sch, to_free);
}

static const struct nla_policy pi_policy[TCA_PI_MAX + 1] = {
	[TCA_PI_QREF] = {.type = NLA_U32},
	[TCA_PI_LIMIT] = {.type = NLA_U32},
	[TCA_PI_W] = {.type = NLA_U32},
	[TCA_PI_A] = {.type = NLA_U64},
	[TCA_PI_B] = {.type = NLA_U64},
	[TCA_PI_ECN] = {.type = NLA_U32},
	[TCA_PI_BYTEMODE] = {.type = NLA_U32},
};

static int pi_change(struct Qdisc *sch, struct nlattr *opt,
		      struct netlink_ext_ack *extack)
{
	struct pi_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_PI_MAX + 1];
	unsigned int qlen, dropped = 0;
	int err;

	err = nla_parse_nested_deprecated(tb, TCA_PI_MAX, opt, pi_policy, NULL);
	if (err < 0)
		return err;

	sch_tree_lock(sch);

	/* should be number of packets */
	if (tb[TCA_PI_QREF])
		q->params.qref = nla_get_u32(tb[TCA_PI_QREF]);

	/* w is in jiffies */
	if (tb[TCA_PI_W])
		q->params.w =
			usecs_to_jiffies(nla_get_u32(tb[TCA_PI_W]));

	if (tb[TCA_PI_LIMIT]) {
		u32 limit = nla_get_u32(tb[TCA_PI_LIMIT]);

		q->params.limit = limit;
		sch->limit = limit;
	}

	if (tb[TCA_PI_A])
		q->params.a = nla_get_u64(tb[TCA_PI_A]);

	if (tb[TCA_PI_B])
		q->params.b = nla_get_u64(tb[TCA_PI_B]);

	if (tb[TCA_PI_ECN])
		q->params.ecn = nla_get_u32(tb[TCA_PI_ECN]);

	if (tb[TCA_PI_BYTEMODE])
		q->params.bytemode = nla_get_u32(tb[TCA_PI_BYTEMODE]);

	/* Drop excess packets if new limit is lower */
	qlen = sch->q.qlen;
	while (sch->q.qlen > sch->limit) {
		struct sk_buff *skb = __qdisc_dequeue_head(&sch->q);

		dropped += qdisc_pkt_len(skb);
		qdisc_qstats_backlog_dec(sch, skb);
		rtnl_qdisc_drop(skb, sch);
	}
	qdisc_tree_reduce_backlog(sch, qlen - sch->q.qlen, dropped);

	sch_tree_unlock(sch);
	return 0;
}

static void calculate_probability(struct Qdisc *sch)
{
	struct pi_sched_data *q = qdisc_priv(sch);
	u32 qlen = qdisc_qlen(sch);	/* queue size in bytes */
	u32 qlen_old = q->vars.qlen_old;
	s64 delta = 0;		/* determines the change in probability */
	u64 oldprob;
	u64 a, b;
	u64 param_a =q->params.a, param_b = q->params.b;
	u64 mul_a = 1, mul_b=1;
	bool update_prob = true;

	q->vars.qlen_old = qlen;
	q->stats.qlen = qlen;

	printk("old_qlen is %u !\n", qlen_old);
	printk("qlen is %u !\n", qlen);

	/*
	 * Depending on the precision of a and b, increment the scale.
	 * For example, if the required value of 'a' is 1822 * 10^-8,
	 * q->params.a would be 182200000. The logic below would result
	 * in param_a being 1822 and mul_a being 10^5. This would mean
	 * that the calculation in line 250 would be 
	 * ((1822 * MAX_PROB) / 10^13) * 10^5 which is the value we want
	 * 
	*/
	while(param_a %10 ==0) {
		mul_a = mul_a *10;
		param_a = param_a / 10;
	}

	while(param_b %10 ==0) {
		mul_b = mul_b *10;
		param_b = param_b / 10;
	}

	/* In the algorithm, a and b are between 0 and 2 with typical
	 * value for a as 0.125. In this implementation, we use values 0-32
	 * passed from user space to represent this. Also, a and b have
	 * unit of HZ and need to be scaled before they can used to update
	 * probability. a/b are updated locally below by scaling down
	 * by 16 to come to 0-2 range.
	 */
	a = ((param_a *MAX_PROB)/ PARAMETER_SCALE) * mul_a;
	b = ((param_b *MAX_PROB)/ PARAMETER_SCALE) * mul_b;

	printk("a is %llu !\n", a);
	printk("b is %llu !\n", b);
	// printk("qref is %u !\n", q->params.qref);
	// printk("MAX_PROB is %llu !\n", MAX_PROB);
	// printk("PSCHED_TICKS_PER_SEC is %ld !\n", PSCHED_TICKS_PER_SEC);
	// printk("PARAMETER_SCALE is %d !\n", PARAMETER_SCALE);

	// IS u64 typecasting required?
	// IT HAS BEEN REMOVED FROM PIE CODE.
	delta += a * (u64)(qlen - q->params.qref);
	delta -= b * (u64)(qlen_old - q->params.qref);

	printk("delta is %lld !\n", delta);

	oldprob = q->vars.prob;

	printk("prob is %llu !\n", q->vars.prob);
	q->vars.prob += delta;
	printk("updated prob is %llu !\n", q->vars.prob);
	// printk("UINT64_MAX %llu !\n",UINT64_MAX);
	printk("U64_MAX %llu !\n",U64_MAX);
	printk("MAX_PROB %llu !\n",MAX_PROB);
	

	if (delta > 0) {
		/* prevent overflow */
		if (q->vars.prob < oldprob) {
			q->vars.prob = MAX_PROB;
			/* Prevent normalization error. If probability is at
			 * maximum value already, we normalize it here, and
			 * skip the check to do a non-linear drop in the next
			 * section.
			 */
			update_prob = false;
		}
	} else {
		/* prevent underflow */
		if (q->vars.prob > oldprob)
			q->vars.prob = 0;
	}

	q->vars.qlen_old = qlen;
}

static void pi_timer(struct timer_list *t)
{
	struct pi_sched_data *q = from_timer(q, t, adapt_timer);
	struct Qdisc *sch = q->sch;
	spinlock_t *root_lock;
	rcu_read_lock();
	root_lock = qdisc_lock(qdisc_root_sleeping(sch));
	spin_lock(root_lock);
	calculate_probability(sch);

	/* reset the timer to fire after 'w'. w is in jiffies. */
	if (q->params.w)
		mod_timer(&q->adapt_timer, jiffies + q->params.w);
	spin_unlock(root_lock);
	rcu_read_unlock();
}

static int pi_init(struct Qdisc *sch, struct nlattr *opt,
		    struct netlink_ext_ack *extack)
{
	struct pi_sched_data *q = qdisc_priv(sch);

	pi_params_init(&q->params);
	pi_vars_init(&q->vars);
	sch->limit = q->params.limit;

	q->sch = sch;
	timer_setup(&q->adapt_timer, pi_timer, 0);

	if (opt) {
		int err = pi_change(sch, opt, extack);

		if (err)
			return err;
	}

	mod_timer(&q->adapt_timer, jiffies + HZ / 2);
	return 0;
}

static int pi_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct pi_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;

	opts = nla_nest_start_noflag(skb, TCA_OPTIONS);
	if (!opts)
		goto nla_put_failure;

	if (nla_put_u32(skb, TCA_PI_QREF, q->params.qref) ||
	    nla_put_u32(skb, TCA_PI_LIMIT, sch->limit) ||
	    nla_put_u32(skb, TCA_PI_W,
			jiffies_to_usecs(q->params.w)) ||
	    nla_put_u64_64bit(skb, TCA_PI_A, q->params.a, 0) ||
	    nla_put_u64_64bit(skb, TCA_PI_B, q->params.b, 0) ||
	    nla_put_u32(skb, TCA_PI_ECN, q->params.ecn) ||
	    nla_put_u32(skb, TCA_PI_BYTEMODE, q->params.bytemode))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	nla_nest_cancel(skb, opts);
	return -1;
}

static int pi_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct pi_sched_data *q = qdisc_priv(sch);
	struct tc_pi_xstats st = { // Need to verify if all params are needed
		.prob		= q->vars.prob,
		.qlen		= q->stats.qlen,
		.packets_in	= q->stats.packets_in,
		.overlimit	= q->stats.overlimit,
		.maxq		= q->stats.maxq,
		.dropped	= q->stats.dropped,
		.ecn_mark	= q->stats.ecn_mark,
	};
	printk("DUMPING q->vars.prob as %llu",q->vars.prob);
	printk("DUMPING q->stats.qlen as %u",q->stats.qlen);
	printk("DUMPING q->stats.packets_in as %u",q->stats.packets_in);
	printk("DUMPING q->stats.overlimit as %u",q->stats.overlimit);
	printk("DUMPING q->stats.maxq as %u",q->stats.maxq);
	printk("DUMPING q->stats.dropped as %u",q->stats.dropped);
	printk("DUMPING q->stats.ecn_mark as %u", q->stats.ecn_mark);

	return gnet_stats_copy_app(d, &st, sizeof(st));
}

static struct sk_buff *pi_qdisc_dequeue(struct Qdisc *sch)
{
	struct sk_buff *skb = qdisc_dequeue_head(sch);

	if (!skb)
		return NULL;

	// pi_process_dequeue(sch, skb);
	return skb;
}

static void pi_reset(struct Qdisc *sch)
{
	struct pi_sched_data *q = qdisc_priv(sch);
	qdisc_reset_queue(sch);
	pi_vars_init(&q->vars);
}

static void pi_destroy(struct Qdisc *sch)
{
	struct pi_sched_data *q = qdisc_priv(sch);

	q->params.w = 0;
	del_timer_sync(&q->adapt_timer);
}

static struct Qdisc_ops pi_qdisc_ops __read_mostly = {
	.id = "pi",
	.priv_size	= sizeof(struct pi_sched_data),
	.enqueue	= pi_qdisc_enqueue,
	.dequeue	= pi_qdisc_dequeue,
	.peek		= qdisc_peek_dequeued,
	.init		= pi_init,
	.destroy	= pi_destroy,
	.reset		= pi_reset,
	.change		= pi_change,
	.dump		= pi_dump,
	.dump_stats	= pi_dump_stats,
	.owner		= THIS_MODULE,
};

static int __init pi_module_init(void)
{
	return register_qdisc(&pi_qdisc_ops);
}

static void __exit pi_module_exit(void)
{
	unregister_qdisc(&pi_qdisc_ops);
}

module_init(pi_module_init);
module_exit(pi_module_exit);

MODULE_DESCRIPTION("Proportional Integral controller (PI) scheduler");
MODULE_LICENSE("GPL");