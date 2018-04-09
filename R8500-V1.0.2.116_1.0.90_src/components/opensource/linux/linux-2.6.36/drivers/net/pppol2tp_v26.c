/*****************************************************************************
 * Linux PPP over L2TP (PPPoX/PPPoL2TP) Sockets
 *
 * PPPoX    --- Generic PPP encapsulation socket family
 * PPPoL2TP --- PPP over L2TP (RFC 2661)
 *
 *
 * Version:    0.17.0
 *
 * 251003 :	Copied from pppoe.c version 0.6.9.
 *
 * Authors:	Martijn van Oosterhout <kleptog@svana.org>
 *		James Chapman (jchapman@katalix.com)
 * Contributors:
 *		Michal Ostrowski <mostrows@speakeasy.net>
 *		Arnaldo Carvalho de Melo <acme@xconectiva.com.br>
 *		David S. Miller (davem@redhat.com)
 *
 * License:
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 */

/* This driver handles only L2TP data frames; control frames are handled by a
 * userspace application.
 *
 * To send data in an L2TP session, userspace opens a PPPoL2TP socket and
 * attaches it to a bound UDP socket with local tunnel_id / session_id and
 * peer tunnel_id / session_id set. Data can then be sent or received using
 * regular socket sendmsg() / recvmsg() calls. Kernel parameters of the socket
 * can be read or modified using ioctl() or [gs]etsockopt() calls.
 *
 * When a PPPoL2TP socket is connected with local and peer session_id values
 * zero, the socket is treated as a special tunnel management socket.
 *
 * Here's example userspace code to create a socket for sending/receiving data
 * over an L2TP session:-
 *
 *	struct sockaddr_pppol2tp sax;
 *	int fd;
 *	int session_fd;
 *
 *	fd = socket(AF_PPPOX, SOCK_DGRAM, PX_PROTO_OL2TP);
 *
 *	sax.sa_family = AF_PPPOX;
 *	sax.sa_protocol = PX_PROTO_OL2TP;
 *	sax.pppol2tp.fd = tunnel_fd;	// bound UDP socket
 *	sax.pppol2tp.pid = 0;		// current pid owns UDP socket
 *	sax.pppol2tp.addr.sin_addr.s_addr = addr->sin_addr.s_addr;
 *	sax.pppol2tp.addr.sin_port = addr->sin_port;
 *	sax.pppol2tp.addr.sin_family = AF_INET;
 *	sax.pppol2tp.s_tunnel  = tunnel_id;
 *	sax.pppol2tp.s_session = session_id;
 *	sax.pppol2tp.d_tunnel  = peer_tunnel_id;
 *	sax.pppol2tp.d_session = peer_session_id;
 *
 *	session_fd = connect(fd, (struct sockaddr *)&sax, sizeof(sax));
 *
 */

#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#include <linux/module.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/list.h>
#include <asm/uaccess.h>

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/errno.h>

#include <linux/netdevice.h>
#include <linux/net.h>
#include <linux/inetdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/udp.h>
#include <linux/if_pppox.h>
#include <linux/if_pppol2tp.h>
#include <net/sock.h>
#include <linux/ppp_channel.h>
#include <linux/ppp_defs.h>
#include <linux/if_ppp.h>
#include <linux/file.h>
#include <linux/hash.h>
#include <linux/proc_fs.h>
#include <net/dst.h>
#include <net/ip.h>
#include <net/udp.h>
#include <net/xfrm.h>

#include <asm/byteorder.h>
#include <asm/atomic.h>
#include <linux/netfilter_ipv4.h>
#include <asm/checksum.h>


#define PPPOL2TP_DRV_VERSION	"V0.17"

/* Developer debug code. */
/* #define DEBUG */	/* Define to compile in very verbose developer debug */

/* Timeouts are specified in milliseconds to/from userspace */
#define JIFFIES_TO_MS(t) ((t) * 1000 / HZ)
#define MS_TO_JIFFIES(j) ((j * HZ) / 1000)

/* L2TP header constants */
#define L2TP_HDRFLAG_T	   0x8000
#define L2TP_HDRFLAG_L	   0x4000
#define L2TP_HDRFLAG_S	   0x0800
#define L2TP_HDRFLAG_O	   0x0200
#define L2TP_HDRFLAG_P	   0x0100

#define L2TP_HDR_VER_MASK  0x000F
#define L2TP_HDR_VER	   0x0002

/* Space for UDP, L2TP and PPP headers */
#define PPPOL2TP_HEADER_OVERHEAD	40

/* Just some random numbers */
#define L2TP_TUNNEL_MAGIC   0x42114DDA
#define L2TP_SESSION_MAGIC  0x0C04EB7D

#define PPPOL2TP_HASH_BITS 4
#define PPPOL2TP_HASH_SIZE (1 << PPPOL2TP_HASH_BITS)

/* Default trace flags */
#ifdef DEBUG
#define PPPOL2TP_DEFAULT_DEBUG_FLAGS	-1
#else
#define PPPOL2TP_DEFAULT_DEBUG_FLAGS	0
#endif


/* Debug kernel message control.
 * Verbose debug messages (L2TP_MSG_DEBUG flag) are optionally compiled in.
 */
#ifdef DEBUG
#define DPRINTK(_mask, _fmt, args...)					\
	do {								\
		if ((_mask) & PPPOL2TP_MSG_DEBUG)			\
			printk(KERN_DEBUG "PPPOL2TP %s: " _fmt,		\
			       __FUNCTION__, ##args);			\
	} while(0)
#else
#define DPRINTK(_mask, _fmt, args...) do { } while(0)
#endif /* DEBUG */

#define PRINTK(_mask, _type, _lvl, _fmt, args...)			\
	do {								\
		if ((_mask) & (_type))					\
			printk(_lvl "PPPOL2TP: " _fmt, ##args);		\
	} while(0)

/* Extra driver debug. Should only be enabled by developers working on
 * this driver.
 */
#ifdef DEBUG
#define ENTER_FUNCTION	 printk(KERN_DEBUG "PPPOL2TP: --> %s\n", __FUNCTION__)
#define EXIT_FUNCTION	 printk(KERN_DEBUG "PPPOL2TP: <-- %s\n", __FUNCTION__)
#else
#define ENTER_FUNCTION	 do { } while(0)
#define EXIT_FUNCTION	 do { } while(0)
#endif

/* Number of bytes to build transmit L2TP headers.
 * Unfortunately the size is different depending on whether sequence numbers
 * are enabled.
 */
#define PPPOL2TP_L2TP_HDR_SIZE_SEQ		10
#define PPPOL2TP_L2TP_HDR_SIZE_NOSEQ		6

struct pppol2tp_send {
	struct pppol2tp_session *session;
	struct pppol2tp_tunnel *tunnel;
	struct msghdr *msg;
	struct sk_buff *skb;
 	struct sock *tunnel_sock;
	struct iovec *iov;
	struct work_struct send_task;
	int total_len;
        struct kiocb iocb;
        struct sock_iocb siocb;
	u8 hdr[PPPOL2TP_L2TP_HDR_SIZE_SEQ];
};

struct pppol2tp_tunnel;

/* Describes a session. It is the sk_user_data field in the PPPoL2TP
 * socket. Contains information to determine incoming packets and transmit
 * outgoing ones.
 */
struct pppol2tp_session
{
	int			magic;		/* should be
						 * L2TP_SESSION_MAGIC */
	int			owner;		/* pid that opened the socket */

	struct sock		*sock;		/* Pointer to the session
						 * PPPoX socket */
	struct sock		*tunnel_sock;	/* Pointer to the tunnel UDP
						 * socket */

	struct pppol2tp_addr	tunnel_addr;	/* Description of tunnel */

	struct pppol2tp_tunnel	*tunnel;	/* back pointer to tunnel
						 * context */

	char			name[20];	/* "sess xxxxx/yyyyy", where
						 * x=tunnel_id, y=session_id */
	int			mtu;
	int			mru;
	int			flags;		/* accessed by PPPIOCGFLAGS.
						 * Unused. */
	int			recv_seq:1;	/* expect receive packets with
						 * sequence numbers? */
	int			send_seq:1;	/* send packets with sequence
						 * numbers? */
	int			lns_mode:1;	/* behave as LNS? LAC enables
						 * sequence numbers under
						 * control of LNS. */
	int			debug;		/* bitmask of debug message
						 * categories */
	int			reorder_timeout; /* configured reorder timeout
						  * (in jiffies) */
	u16			nr;		/* session NR state (receive) */
	u16			ns;		/* session NR state (send) */
	struct sk_buff_head	reorder_q;	/* receive reorder queue */
	struct pppol2tp_ioc_stats stats;
	struct hlist_node	hlist;		/* Hash list node */
};

/* The sk_user_data field of the tunnel's UDP socket. It contains info to track
 * all the associated sessions so incoming packets can be sorted out
 */
struct pppol2tp_tunnel
{
	int			magic;		/* Should be L2TP_TUNNEL_MAGIC */

	struct workqueue_struct	*wq;		/* Per-tunnel work queue */

	struct proto		*old_proto;	/* original proto */
	struct proto		l2tp_proto;	/* L2TP proto */
	rwlock_t		hlist_lock;	/* protect session_hlist */
	struct hlist_head	session_hlist[PPPOL2TP_HASH_SIZE];
						/* hashed list of sessions,
						 * hashed by id */
	int			debug;		/* bitmask of debug message
						 * categories */
	char			name[12];	/* "tunl xxxxx" */
	struct pppol2tp_ioc_stats stats;

	void (*old_data_ready)(struct sock *, int);
	void (*old_sk_destruct)(struct sock *);

	struct sock		*sock;		/* Parent socket */
	struct list_head	list;		/* Keep a list of all open
						 * prepared sockets */

	atomic_t		session_count;
};

/* Private data stored for received packets in the skb.
 */
struct pppol2tp_skb_cb {
	u16			ns;
	u16			nr;
	int			has_seq;
	int			length;
	unsigned long		expires;
};

#define PPPOL2TP_SKB_CB(skb)	((struct pppol2tp_skb_cb *) &skb->cb[sizeof(struct inet_skb_parm)])

static int pppol2tp_xmit(struct ppp_channel *chan, struct sk_buff *skb);

static struct ppp_channel_ops pppol2tp_chan_ops = { pppol2tp_xmit , NULL };
static struct proto_ops pppol2tp_ops;
static LIST_HEAD(pppol2tp_tunnel_list);

/* Macros to derive session/tunnel context pointers from a socket. */
#define SOCK_2_SESSION(sock, session, err, errval, label, quiet) \
	session = (struct pppol2tp_session *)((sock)->sk_user_data);  \
	if (!session /*|| session->magic != L2TP_SESSION_MAGIC*/) {	       \
		if (!quiet) \
			printk(KERN_ERR "%s: %s:%d: BAD SESSION MAGIC " \
			       "(" #sock "=%p) session=%p magic=%x\n", \
			       __FUNCTION__, __FILE__, __LINE__, sock, \
			       session, session ? session->magic : 0); \
		err = errval; \
		goto label; \
	}

#define SOCK_2_TUNNEL(sock, tunnel, err, errval, label, quiet) \
	tunnel = (struct pppol2tp_tunnel *)((sock)->sk_user_data);	 \
	if (!tunnel || tunnel->magic != L2TP_TUNNEL_MAGIC) {	     \
		if (!quiet) \
			printk(KERN_ERR "%s: %s:%d: BAD TUNNEL MAGIC " \
			       "(" #sock "=%p) tunnel=%p magic=%x\n", \
			       __FUNCTION__, __FILE__, __LINE__, sock, \
			       tunnel, tunnel ? tunnel->magic : 0); \
		err = errval; \
		goto label; \
	}

/* Session hash list.
 * The session_id SHOULD be random according to RFC2661, but several
 * L2TP implementations (Cisco and Microsoft) use incrementing
 * session_ids.  So we do a real hash on the session_id, rather than a
 * simple bitmask.
 */
static inline struct hlist_head *
pppol2tp_session_id_hash(struct pppol2tp_tunnel *tunnel, u16 session_id)
{
	unsigned long hash_val = (unsigned long) session_id;
	return &tunnel->session_hlist[hash_long(hash_val, PPPOL2TP_HASH_BITS)];
}

/* Lookup a session by id
 */
static struct pppol2tp_session *
pppol2tp_session_find(struct pppol2tp_tunnel *tunnel, u16 session_id)
{
	struct hlist_head *session_list =
		pppol2tp_session_id_hash(tunnel, session_id);
	struct hlist_node *tmp;
	struct hlist_node *walk;
	struct pppol2tp_session *session;

	hlist_for_each_safe(walk, tmp, session_list) {
		session = hlist_entry(walk, struct pppol2tp_session, hlist);
		if (session->tunnel_addr.s_session == session_id) {
			return session;
		}
	}

	return NULL;
}

/*****************************************************************************
 * Receive data handling
 *****************************************************************************/

/* Queue a skb in order. If the skb has no sequence number, queue it
 * at the tail.
 */
static void pppol2tp_recv_queue_skb(struct pppol2tp_session *session, struct sk_buff *skb)
{
	struct sk_buff *next;
	struct sk_buff *prev;
	u16 ns = PPPOL2TP_SKB_CB(skb)->ns;

	ENTER_FUNCTION;

	spin_lock(&session->reorder_q.lock);

	prev = (struct sk_buff *) &session->reorder_q;
	next = prev->next;
	while (next != prev) {
		if (PPPOL2TP_SKB_CB(next)->ns > ns) {
			__skb_insert(skb, next->prev, next, &session->reorder_q);
			PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_DEBUG,
			       "%s: pkt %hu, inserted before %hu, reorder_q len=%d\n",
			       session->name, ns, PPPOL2TP_SKB_CB(next)->ns,
			       skb_queue_len(&session->reorder_q));
			session->stats.rx_oos_packets++;
			goto out;
		}
		next = next->next;
	}

	__skb_queue_tail(&session->reorder_q, skb);

out:
	spin_unlock(&session->reorder_q.lock);
	EXIT_FUNCTION;
}

/* Dequeue a single skb, passing it either to ppp or to userspace.
 */
static void pppol2tp_recv_dequeue_skb(struct pppol2tp_session *session, struct sk_buff *skb)
{
	struct pppol2tp_tunnel *tunnel = session->tunnel;
	int length = PPPOL2TP_SKB_CB(skb)->length;
	struct sock *session_sock = NULL;

	ENTER_FUNCTION;

	/* We're about to requeue the skb, so unlink it and return resources
	 * to its current owner (a socket receive buffer).
	 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,14))
	skb_unlink(skb, &session->reorder_q);
#else
	skb_unlink(skb);
#endif
	skb_orphan(skb);

	tunnel->stats.rx_packets++;
	tunnel->stats.rx_bytes += length;
	session->stats.rx_packets++;
	session->stats.rx_bytes += length;

	if (PPPOL2TP_SKB_CB(skb)->has_seq) {
		/* Bump our Nr */
		session->nr++;
		PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_DEBUG,
		       "%s: updated nr to %hu\n", session->name, session->nr);
	}

	/* If the socket is bound, send it in to PPP's input queue.  Otherwise
	 * queue it on the session socket.
	 */
	session_sock = session->sock;
	if (session_sock->sk_state & PPPOX_BOUND) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
		struct pppox_sock *po;
#else
		struct pppox_opt *po;
#endif
		PRINTK(session->debug, PPPOL2TP_MSG_DATA, KERN_DEBUG,
		       "%s: recv %d byte data frame, passing to ppp\n",
		       session->name, length);

		/* We need to forget all info related to the L2TP packet
		 * gathered in the skb as we are going to reuse the same
		 * skb for the inner packet.
		 * Namely we need to:
		 * - reset xfrm (IPSec) information as it applies to
		 *   the outer L2TP packet and not to the inner one
		 * - release the dst to force a route lookup on the inner
		 *   IP packet since skb->dst currently points to the dst
		 *   of the UDP tunnel
		 * - reset netfilter information as it doesn't apply
		 *   to the inner packet either
		 */
		secpath_reset(skb);
		dst_release(skb->dst);
		skb->dst = NULL;
		nf_reset(skb);

		po = pppox_sk(session_sock);
        /* Foxconn added start pling 09/15/2010 */
        /* Since this module uses cb to store info. 
         * Before we send the skb to ppp, we clear 
         * the cb buffer first to avoid
         * un-wanted garbage in cb buffer.
         */
        memset(skb->cb, 0, sizeof(skb->cb));
        /* Foxconn added end pling 09/15/2010 */
		ppp_input(&po->chan, skb);
	} else {
		PRINTK(session->debug, PPPOL2TP_MSG_DATA, KERN_INFO,
		       "%s: socket not bound\n", session->name);
		/* Not bound. Queue it now */
		if (sock_queue_rcv_skb(session_sock, skb) < 0) {
			session->stats.rx_errors++;
			kfree_skb(skb);
			if (!sock_flag(session_sock, SOCK_DEAD))
				session_sock->sk_data_ready(session_sock, 0);
		}
	}

	DPRINTK(session->debug, "calling sock_put; refcnt=%d\n",
		session->sock->sk_refcnt.counter);
	sock_put(session->sock);
	EXIT_FUNCTION;
}

/* Dequeue skbs from the session's reorder_q, subject to packet order.
 * Skbs that have been in the queue for too long are simply discarded.
 */
static void pppol2tp_recv_dequeue(struct pppol2tp_session *session)
{
	struct sk_buff *next;
	struct sk_buff *prev;

	ENTER_FUNCTION;

	prev = (struct sk_buff *) &session->reorder_q;
	spin_lock(&session->reorder_q.lock);
	next = prev->next;

	/* If the pkt at the head of the queue has the nr that we
	 * expect to send up next, dequeue it and any other
	 * in-sequence packets behind it.
	 */
	while (next != prev) {
		struct sk_buff *skb = next;
		next = next->next;
		spin_unlock(&session->reorder_q.lock);

		if (time_after(jiffies, PPPOL2TP_SKB_CB(skb)->expires)) {
			session->stats.rx_seq_discards++;
			session->stats.rx_errors++;
			PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_DEBUG,
			       "%s: oos pkt %hu len %d discarded (too old), waiting for %hu, reorder_q_len=%d\n",
			       session->name, PPPOL2TP_SKB_CB(skb)->ns,
			       PPPOL2TP_SKB_CB(skb)->length, session->nr,
			       skb_queue_len(&session->reorder_q));
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,14))
			skb_unlink(skb, &session->reorder_q);
#else
			skb_unlink(skb);
#endif
			kfree_skb(skb);
			goto again;
		}

		if (PPPOL2TP_SKB_CB(skb)->has_seq) {
			if (PPPOL2TP_SKB_CB(skb)->ns != session->nr) {
				PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_DEBUG,
					"%s: holding oos pkt %hu len %d, waiting for %hu, reorder_q_len=%d\n",
					session->name, PPPOL2TP_SKB_CB(skb)->ns,
					PPPOL2TP_SKB_CB(skb)->length, session->nr,
					skb_queue_len(&session->reorder_q));
				goto out;
			}
		}
		pppol2tp_recv_dequeue_skb(session, skb);
again:
		spin_lock(&session->reorder_q.lock);
	}

	spin_unlock(&session->reorder_q.lock);
out:
	EXIT_FUNCTION;
}

/* Internal receive frame. Do the real work of receiving an L2TP data frame
 * here. The skb is not on a list when we get here.
 * Returns 0 if the packet was a data packet and was successfully passed on.
 * Returns 1 if the packet was not a good data packet and could not be
 * forwarded.  All such packets are passed up to userspace to deal with.
 */
static int pppol2tp_recv_core(struct sock *sock, struct sk_buff *skb)
{
	struct pppol2tp_session *session = NULL;
	int error = 0;
	struct pppol2tp_tunnel *tunnel;
	unsigned char *ptr;
	u16 hdrflags;
	u16 tunnel_id, session_id;
	int length;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22))
	struct udphdr *uh;
#endif
    u16 proto_ppphdr;   /* foxconn, add by MJ., protocol in PPP hdr.*/

	ENTER_FUNCTION;

	SOCK_2_TUNNEL(sock, tunnel, error, 1, end, 0);

	/* Short packet? */
	if (skb->len < sizeof(struct udphdr)) {
		PRINTK(tunnel->debug, PPPOL2TP_MSG_DATA, KERN_INFO,
		       "%s: recv short packet (len=%d)\n", tunnel->name, skb->len);
		goto end;
	}

	/* Point to L2TP header */
	ptr = skb->data + sizeof(struct udphdr);

	/* Get L2TP header flags */
	hdrflags = ntohs(*(u16*)ptr);

	/* Trace packet contents, if enabled */
	if (tunnel->debug & PPPOL2TP_MSG_DATA) {
		printk(KERN_DEBUG "%s: recv: ", tunnel->name);

		for (length = 0; length < 16; length++)
			printk(" %02X", ptr[length]);
		printk("\n");
	}

	/* Get length of L2TP packet */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22))
	uh = (struct udphdr *) skb_transport_header(skb);
	length = ntohs(uh->len) - sizeof(struct udphdr);
#else
	length = ntohs(skb->h.uh->len) - sizeof(struct udphdr);
#endif

	/* Too short? */
	if (length < 12) {
		PRINTK(tunnel->debug, PPPOL2TP_MSG_DATA, KERN_INFO,
		       "%s: recv short L2TP packet (len=%d)\n", tunnel->name, length);
		goto end;
	}


    proto_ppphdr = ntohs(*(u16*)(ptr+10));

	/* If type is control packet, it is handled by userspace. */
    /* Foxconn, add by MJ. for changing the checking of control packets. 
        Only forward IP-packets(0x0021) to PPP device. */
	//if (hdrflags & L2TP_HDRFLAG_T) { //commented by MJ.
    if ((hdrflags & L2TP_HDRFLAG_T) || (proto_ppphdr != 0x0021))
    {
		PRINTK(tunnel->debug, PPPOL2TP_MSG_DATA, KERN_DEBUG,
		       "%s: recv control packet, len=%d\n", tunnel->name, length);
		goto end;
	}

	/* Skip flags */
	ptr += 2;

	/* If length is present, skip it */
	if (hdrflags & L2TP_HDRFLAG_L)
		ptr += 2;

	/* Extract tunnel and session ID */
	tunnel_id = ntohs(*(u16 *) ptr);
	ptr += 2;
	session_id = ntohs(*(u16 *) ptr);
	ptr += 2;

	/* Find the session context */
	session = pppol2tp_session_find(tunnel, session_id);
	if (!session) {
		/* Not found? Pass to userspace to deal with */
		PRINTK(tunnel->debug, PPPOL2TP_MSG_DATA, KERN_INFO,
		       "%s: no socket found (%hu/%hu). Passing up.\n",
		       tunnel->name, tunnel_id, session_id);
		goto end;
	}
	sock_hold(session->sock);

	DPRINTK(session->debug, "%s: socket rcvbuf alloc=%d\n",
		session->name, atomic_read(&sock->sk_rmem_alloc));

	/* The ref count on the socket was increased by the above call since
	 * we now hold a pointer to the session. Take care to do sock_put()
	 * when exiting this function from now on...
	 */

	/* Handle the optional sequence numbers.  If we are the LAC,
	 * enable/disable sequence numbers under the control of the LNS.  If
	 * no sequence numbers present but we were expecting them, discard
	 * frame.
	 */
	if (hdrflags & L2TP_HDRFLAG_S) {
		u16 ns, nr;
		ns = ntohs(*(u16 *) ptr);
		ptr += 2;
		nr = ntohs(*(u16 *) ptr);
		ptr += 2;

		/* Received a packet with sequence numbers. If we're the LNS,
		 * check if we sre sending sequence numbers and if not,
		 * configure it so.
		 */
		if ((!session->lns_mode) && (!session->send_seq)) {
			PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_INFO,
			       "%s: requested to enable seq numbers by LNS\n",
			       session->name);
			session->send_seq = -1;
		}

		/* Store L2TP info in the skb */
		PPPOL2TP_SKB_CB(skb)->ns = ns;
		PPPOL2TP_SKB_CB(skb)->nr = nr;
		PPPOL2TP_SKB_CB(skb)->has_seq = 1;

		PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_DEBUG,
		       "%s: recv data ns=%hu, nr=%hu, session nr=%hu\n",
		       session->name, ns, nr, session->nr);
	} else {
		/* No sequence numbers.
		 * If user has configured mandatory sequence numbers, discard.
		 */
		if (session->recv_seq) {
			PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_WARNING,
			       "%s: recv data has no seq numbers when required. "
			       "Discarding\n", session->name);
			session->stats.rx_seq_discards++;
			session->stats.rx_errors++;
			goto discard;
		}

		/* If we're the LAC and we're sending sequence numbers, the
		 * LNS has requested that we no longer send sequence numbers.
		 * If we're the LNS and we're sending sequence numbers, the
		 * LAC is broken. Discard the frame.
		 */
		if ((!session->lns_mode) && (session->send_seq)) {
			PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_INFO,
			       "%s: requested to disable seq numbers by LNS\n",
			       session->name);
			session->send_seq = 0;
		} else if (session->send_seq) {
			PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_WARNING,
			       "%s: recv data has no seq numbers when required. "
			       "Discarding\n", session->name);
			session->stats.rx_seq_discards++;
			session->stats.rx_errors++;
			goto discard;
		}

		/* Store L2TP info in the skb */
		PPPOL2TP_SKB_CB(skb)->has_seq = 0;
	}

	/* If offset bit set, skip it. */
	if (hdrflags & L2TP_HDRFLAG_O)
		ptr += 2 + ntohs(*(u16 *) ptr);

	skb_pull(skb, ptr - skb->data);

	/* Skip PPP header, if present.	 In testing, Microsoft L2TP clients
	 * don't send the PPP header (PPP header compression enabled), but
	 * other clients can include the header. So we cope with both cases
	 * here. The PPP header is always FF03 when using L2TP.
	 *
	 * Note that skb->data[] isn't dereferenced from a u16 ptr here since
	 * the field may be unaligned.
	 */
	if ((skb->data[0] == 0xff) && (skb->data[1] == 0x03))
		skb_pull(skb, 2);

	/* Prepare skb for adding to the session's reorder_q.  Hold
	 * packets for max reorder_timeout or 1 second if not
	 * reordering.
	 */
	PPPOL2TP_SKB_CB(skb)->length = length;
	PPPOL2TP_SKB_CB(skb)->expires = jiffies +
		(session->reorder_timeout ? session->reorder_timeout : HZ);

	/* Add packet to the session's receive queue. Reordering is done here, if
	 * enabled. Saved L2TP protocol info is stored in skb->sb[].
	 */
	if (PPPOL2TP_SKB_CB(skb)->has_seq) {
		if (session->reorder_timeout != 0) {
			/* Packet reordering enabled. Add skb to session's
			 * reorder queue, in order of ns.
			 */
			pppol2tp_recv_queue_skb(session, skb);
		} else {
			/* Packet reordering disabled. Discard out-of-sequence
			 * packets
			 */
			if (PPPOL2TP_SKB_CB(skb)->ns != session->nr) {
				session->stats.rx_seq_discards++;
				session->stats.rx_errors++;
				PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_DEBUG,
				       "%s: oos pkt %hu len %d discarded, waiting for %hu, reorder_q_len=%d\n",
				       session->name, PPPOL2TP_SKB_CB(skb)->ns,
				       PPPOL2TP_SKB_CB(skb)->length, session->nr,
				       skb_queue_len(&session->reorder_q));
				goto discard;
			}
			skb_queue_tail(&session->reorder_q, skb);
		}
	} else {
		/* No sequence numbers. Add the skb to the tail of the
		 * reorder queue. This ensures that it will be
		 * delivered after all previous sequenced skbs.
		 */
		skb_queue_tail(&session->reorder_q, skb);
	}

	/* Try to dequeue as many skbs from reorder_q as we can. */
	pppol2tp_recv_dequeue(session);

	EXIT_FUNCTION;
	return 0;

discard:
	DPRINTK(session->debug, "discarding skb, len=%d\n", skb->len);
	kfree_skb(skb);

	DPRINTK(session->debug, "calling sock_put; refcnt=%d\n",
		session->sock->sk_refcnt.counter);
	sock_put(session->sock);
	EXIT_FUNCTION;
	return 0;

end:
	EXIT_FUNCTION;
	return 1;
}

/* The data_ready hook on the UDP socket. Scan the incoming packet list for
 * packets to process. Only control or bad data packets are delivered to
 * userspace.
 */
static void pppol2tp_data_ready(struct sock *sk, int len)
{
	int err;
	struct pppol2tp_tunnel *tunnel;
	struct sk_buff *skb;

	ENTER_FUNCTION;
	SOCK_2_TUNNEL(sk, tunnel, err, -EBADF, end, 0);

	PRINTK(tunnel->debug, PPPOL2TP_MSG_DATA, KERN_DEBUG,
	       "%s: received %d bytes\n", tunnel->name, len);

	skb = skb_dequeue(&sk->sk_receive_queue);
	if (skb != NULL) {
		if (pppol2tp_recv_core(sk, skb)) {
			DPRINTK(tunnel->debug, "%s: packet passing to userspace\n",
				tunnel->name);
			skb_queue_head(&sk->sk_receive_queue, skb);
			tunnel->old_data_ready(sk, len);
		} else {
			DPRINTK(tunnel->debug, "%s: data packet received\n",
				tunnel->name);
		}
	}
end:
	EXIT_FUNCTION;
	return;
}

/* Receive message. This is the recvmsg for the PPPoL2TP socket.
 */
static int pppol2tp_recvmsg(struct kiocb *iocb, struct socket *sock,
			    struct msghdr *msg, size_t len,
			    int flags)
{
	int err = 0;
	struct sk_buff *skb = NULL;
	struct sock *sk = sock->sk;

	ENTER_FUNCTION;

	err = -EIO;
	if (sock->state & PPPOX_BOUND)
		goto error;

	msg->msg_namelen = 0;

	skb=skb_recv_datagram(sk, flags & ~MSG_DONTWAIT,
			      flags & MSG_DONTWAIT, &err);
	if (skb) {
		err = memcpy_toiovec(msg->msg_iov, (unsigned char *) skb->data,
				     skb->len);
		if (err < 0)
			goto do_skb_free;
		err = skb->len;
	}
do_skb_free:
	if (skb)
		kfree_skb(skb);
error:
	EXIT_FUNCTION;
	return err;
}

/************************************************************************
 * Transmit handling
 ***********************************************************************/

/* Internal UDP socket transmission
 */
static int pppol2tp_udp_sock_send(struct kiocb *iocb,
				  struct pppol2tp_session *session,
				  struct pppol2tp_tunnel *tunnel,
				  struct msghdr *msg, int total_len)
{
	mm_segment_t fs;
	int error;

	ENTER_FUNCTION;

	DPRINTK(session->debug, "%s: udp_sendmsg call...\n", session->name);
#ifdef DEBUG
	/* Catch bad socket parameter errors */
	if (msg->msg_name) {
		struct sockaddr_in * usin = (struct sockaddr_in*)msg->msg_name;
		if (msg->msg_namelen < sizeof(*usin)) {
			printk(KERN_ERR "msg->msg_namelen wrong, %d\n", msg->msg_namelen);
			return -EINVAL;
		}
		if (usin->sin_family != AF_INET) {
			if (usin->sin_family != AF_UNSPEC) {
				printk(KERN_ERR "addr family wrong: %d\n", usin->sin_family);
				return -EINVAL;
			}
		}
		if ((usin->sin_addr.s_addr == 0) || (usin->sin_port == 0)) {
			printk(KERN_ERR "udp addr=%x/%hu\n", usin->sin_addr.s_addr, usin->sin_port);
			return -EINVAL;
		}
	}
#endif /* DEBUG */

	/* Set to userspace data segment while we do a sendmsg() call.	We're
	 * actually calling a userspace API from the kernel here...
	 */
	fs = get_fs();
	set_fs(get_ds());

	/* The actual sendmsg() call... */
	error = tunnel->old_proto->sendmsg(iocb, session->tunnel_sock, msg, total_len);
	if (error == -EIOCBQUEUED)
		error = wait_on_sync_kiocb(iocb);

	/* Back to kernel space */
	set_fs(fs);

	if (error >= 0) {
		tunnel->stats.tx_packets++;
		tunnel->stats.tx_bytes += error;
		session->stats.tx_packets++;
		session->stats.tx_bytes += error;
	} else {
		tunnel->stats.tx_errors++;
		session->stats.tx_errors++;
	}

	DPRINTK(session->debug, "%s: %s: returning result %d\n", __FUNCTION__,
		session->name, error);
	kfree(msg->msg_iov);
	kfree(msg);

	EXIT_FUNCTION;
	return error;
}

/* Build an L2TP header for the session into the buffer provided.
 */
static int pppol2tp_build_l2tp_header(struct pppol2tp_session *session,
				      void *buf)
{
	u16 *bufp = buf;
	u16 flags = L2TP_HDR_VER;

	if (session->send_seq) {
		flags |= L2TP_HDRFLAG_S;
	}

	/* Setup L2TP header.
	 * FIXME: Can this ever be unaligned? Is direct dereferencing of
	 * 16-bit header fields safe here for all architectures?
	 */
	*bufp++ = htons(flags);
	*bufp++ = htons(session->tunnel_addr.d_tunnel);
	*bufp++ = htons(session->tunnel_addr.d_session);
	if (session->send_seq) {
		*bufp++ = htons(session->ns);
		*bufp++ = 0;
		session->ns++;
		PRINTK(session->debug, PPPOL2TP_MSG_SEQ, KERN_DEBUG,
		       "%s: updated ns to %hu\n", session->name, session->ns);
	}
	/* This is the PPP header really */
    /* Foxconn removed start pling 09/15/2010 */
    /* This is not necessary and may overwrite buffers outside 'buf'
     * since this is only 10 bytes long currently.
     */
	//*bufp = htons(0xff03);
    /* Foxconn removed end pling 09/15/2010 */

	return ((void *) bufp) - buf;
}

/* This is the sendmsg for the PPPoL2TP pppol2tp_session socket.  We come here
 * when a user application does a sendmsg() on the session socket. L2TP and
 * PPP headers must be inserted into the user's data.
 */
static int pppol2tp_sendmsg(struct kiocb *iocb, struct socket *sock, struct msghdr *m,
			    size_t total_len)
{
	static unsigned char ppph[2] = { 0xff, 0x03 };
	struct sock *sk = sock->sk;
	int error = 0;
	u8 hdr[PPPOL2TP_L2TP_HDR_SIZE_SEQ];
	int hdr_len;
	struct msghdr *msg;
	struct pppol2tp_session *session;
	struct pppol2tp_tunnel *tunnel;

	ENTER_FUNCTION;

	if (sock_flag(sk, SOCK_DEAD) || !(sk->sk_state & PPPOX_CONNECTED)) {
		error = -ENOTCONN;
		goto end;
	}

	/* Get session and tunnel contexts */
	SOCK_2_SESSION(sk, session, error, -EBADF, end, 0);
	SOCK_2_TUNNEL(session->tunnel_sock, tunnel, error, -EBADF, end, 0);

	/* Setup L2TP header */
	hdr_len = pppol2tp_build_l2tp_header(session, &hdr);

	if (session->send_seq)
		PRINTK(session->debug, PPPOL2TP_MSG_DATA, KERN_DEBUG,
		       "%s: send %d bytes, ns=%hu\n", session->name,
		       total_len, session->ns - 1);
	else
		PRINTK(session->debug, PPPOL2TP_MSG_DATA, KERN_DEBUG,
		       "%s: send %d bytes\n", session->name, total_len);

	if (session->debug & PPPOL2TP_MSG_DATA) {
		int i, j, count;

		printk(KERN_DEBUG "%s: xmit:", session->name);
		count = 0;
		for (i = 0; i < m->msg_iovlen; i++) {
			for (j = 0; j < m->msg_iov[i].iov_len; j++) {
				printk(" %02X", ((unsigned char *) m->msg_iov[i].iov_base)[j]);
				count++;
				if (count == 15) {
					printk(" ...");
					break;
				}
			}
		}
		printk("\n");
	}

	/* Unfortunately, there is no direct way for us to pass an skb to the
	 * UDP layer, we have to pretend to be sending ordinary data and use
	 * sendmsg.
	 *
	 * We add the L2TP and PPP headers here. To do so, we create a new
	 * struct msghdr and insert the headers as the first iovecs.
	 */
	msg = kmalloc(sizeof(struct msghdr), GFP_ATOMIC);
	if (msg == NULL) {
		error = -ENOBUFS;
		tunnel->stats.tx_errors++;
		session->stats.tx_errors++;
		goto end;
	}

	msg->msg_iov = kmalloc((m->msg_iovlen + 2) * sizeof(struct iovec),
			       GFP_ATOMIC);
	if (msg->msg_iov == NULL) {
		error = -ENOBUFS;
		tunnel->stats.tx_errors++;
		session->stats.tx_errors++;
		kfree(msg);
		goto end;
	}

	msg->msg_iov[0].iov_base = &hdr;
	msg->msg_iov[0].iov_len	 = hdr_len;
	msg->msg_iov[1].iov_base = &ppph;
	msg->msg_iov[1].iov_len	 = sizeof(ppph);
	memcpy(&msg->msg_iov[2], &m->msg_iov[0],
	       m->msg_iovlen * sizeof(struct iovec));
	msg->msg_iovlen = m->msg_iovlen + 2;

	/* If the user calls sendto() that's just too bad */
	msg->msg_name	 = &session->tunnel_addr.addr;
	msg->msg_namelen = sizeof(session->tunnel_addr.addr);

	msg->msg_control    = m->msg_control;
	msg->msg_controllen = m->msg_controllen;
	msg->msg_flags	    = m->msg_flags;

	/* Do the real work. This always frees msg, regardless of whether
	 * there was an error
	 */
	error = pppol2tp_udp_sock_send(iocb, session, tunnel, msg,
				       total_len + hdr_len + sizeof(ppph));

end:
	EXIT_FUNCTION;
	return error;
}

/* Work queue handler for pppol2tp_xmit().
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,20))
static void pppol2tp_wq_send(struct work_struct *work)
#else
static void pppol2tp_wq_send(void *data)
#endif
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,20))
	struct pppol2tp_send *send = container_of(work, struct pppol2tp_send, send_task);
#else
	struct pppol2tp_send *send = (struct pppol2tp_send *) data;
#endif
	int error;
	mm_segment_t oldfs;

	oldfs = get_fs();
        set_fs(KERNEL_DS);

        error = send->tunnel->old_proto->sendmsg(&send->iocb,
						 send->session->tunnel_sock,
						 send->msg, send->total_len);
	if (error == -EIOCBQUEUED)
		error = wait_on_sync_kiocb(&send->iocb);

        set_fs(oldfs);

	if (error >= 0) {
		send->tunnel->stats.tx_packets++;
		send->tunnel->stats.tx_bytes += error;
		send->session->stats.tx_packets++;
		send->session->stats.tx_bytes += error;
	} else {
		send->tunnel->stats.tx_errors++;
		send->session->stats.tx_errors++;
	}

	kfree(send->iov);
	kfree(send->msg);
	kfree_skb(send->skb);
	kfree(send);
}

const char iphdr4l2tp[] = {0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0xff, 0x11, 0x00, 0x00}; /* no sa & da */
/* foxconn wklin added start, 08/04/2010 @l2tp throughput */
static void pppol2tp_fill_ip_header(struct sock *sk,
                struct sk_buff *skb,
                struct iphdr *iphdr,
                int total_len)
{
    struct rtable *rt = (struct rtable *)skb->dst;
	struct inet_sock *inet = inet_sk(sk);

    memcpy((char *)iphdr, iphdr4l2tp, sizeof(iphdr4l2tp));
    iphdr->saddr = inet->saddr; /* Source IP address */
    iphdr->daddr = inet->daddr; /* Destination IP address */
    iphdr->tot_len = htons(total_len);
    ip_select_ident((struct iphdr *)iphdr, &rt->u.dst, sk);
    /* Header checksum */
    iphdr->check = ip_fast_csum((unsigned char *) iphdr, iphdr->ihl);
}
/* foxconn wklin added end, 08/04/2010 */

/* Transmit function called by generic PPP driver.  Sends PPP frame over
 * PPPoL2TP socket.
 *
 * This is almost the same as pppol2tp_sendmsg(), but rather than being called
 * with a msghdr from userspace, it is called with a skb from the kernel.
 * Since this function cannot block, we must queue up the actual socket send
 * on a work queue.
 */
/* foxconn wklin modified start, 04/14/2011 */
static int pppol2tp_xmit(struct ppp_channel *chan, struct sk_buff *skb)
{
	struct sock *sk = (struct sock *) chan->private;
	static unsigned char ppph[2] = { 0xff, 0x03 };
	int error = 0;
	int hdr_len;
	struct msghdr *msg = NULL;
	struct pppol2tp_session *session;
	struct pppol2tp_tunnel *tunnel;
	struct pppol2tp_send *send = NULL;
    /* foxconn wklin added start, 08/04/2010 @l2tp throughput */
    struct inet_sock *inet = NULL;
    struct dst_entry *dst;
    char *p;
    struct udphdr *uh, tmp_uh;
    struct iphdr *iph, tmp_iph;
    int headroom;
    /* foxconn wklin added end, 08/04/2010 */

	ENTER_FUNCTION;

	if (sock_flag(sk, SOCK_DEAD) || !(sk->sk_state & PPPOX_CONNECTED)) {
		DPRINTK(-1, "dead=%d state=%x\n", sock_flag(sk, SOCK_DEAD), sk->sk_state);
		error = -ENOTCONN;
		goto end;
	}

	/* Get session and tunnel contexts from the socket */
	SOCK_2_SESSION(sk, session, error, -EBADF, keep_go_1, 0);
keep_go_1:    
	SOCK_2_TUNNEL(session->tunnel_sock, tunnel, error, -EBADF, keep_go_2, 0);
keep_go_2:

	send = kmalloc(sizeof(struct pppol2tp_send), GFP_ATOMIC);
	if (send == NULL) {
		error = -ENOBUFS;
		tunnel->stats.tx_errors++;
		session->stats.tx_errors++;
		goto end;
	}

	/* Setup L2TP header */
	hdr_len = pppol2tp_build_l2tp_header(session, &send->hdr);
#if 0
	if (session->send_seq)
		PRINTK(session->debug, PPPOL2TP_MSG_DATA, KERN_DEBUG,
		       "%s: send %d bytes, ns=%hu\n",
		       session->name, skb->len, session->ns - 1);
	else
		PRINTK(session->debug, PPPOL2TP_MSG_DATA, KERN_DEBUG,
		       "%s: send %d bytes\n", session->name, skb->len);

	if (session->debug & PPPOL2TP_MSG_DATA) {
		int i;

		printk(KERN_DEBUG "%s: xmit:", session->name);
		for (i = 0; i < skb->len; i++) {
			printk(" %02X", skb->data[i]);
			if (i == 15) {
				printk(" ...");
				break;
			}
		}
		printk("\n");
	}
#endif

    /* foxconn wklin added start, 08/04/2010 @l2tp throughput */
#define ETHADDR         12
#define BCMTAG53115     4
#define VLANID          4
#define ETHTYPE         2
#define IPHDR           20
#define UDPHDR          8
#define L2TPHDR         8
#define PPPADDRCTRL     2
#define PPPPROTOCOL     2
#define HEADROOM_NEEDED (ETHADDR+BCMTAG53115+VLANID+ETHTYPE+IPHDR+UDPHDR+L2TPHDR+PPPADDRCTRL)
    headroom = skb_headroom(skb);
	if (headroom < HEADROOM_NEEDED) {
        /* printk("<0>go to slowpath, because of headroom %d (%d needed)\n",
           headroom, HEADROOM_NEEDED); */
        goto slow_path;
    }

    dst = sk_dst_check(session->tunnel_sock, 0);
    if (!dst) {
        /* printk("<0>goto slow_path because of null dst\n"); */
        goto slow_path;
    }

    dst_release(skb->dst); /* release dst if any */
    skb->dst = dst;
    /* Foxconn modified start pling 09/15/2010 */
    /* 1. no need to call 'dst_hold' again as 'sk_dst_check' already
     *    called 'dst_hold'.
     * 2. For skb from IP stack, don't replace the original sk,
     *    as might this might cause user-space apps to wrongly
     *    terminate the tunnel socket in some cases.
     */
    //dst_hold(dst);
    if (!skb->sk)
        skb->sk = session->tunnel_sock;
    /* Foxconn modified end pling 09/15/2010 */
    
    /* ppph */
    p = skb_push(skb, sizeof(ppph));
    memcpy(p, &ppph[0], sizeof(ppph));
    /* l2tp header */
    p = skb_push(skb, hdr_len);
    memcpy(p, &(send->hdr[0]), hdr_len);
    /* udp header */
    inet = inet_sk(session->tunnel_sock);
    tmp_uh.source = inet->sport;
    tmp_uh.dest = inet->dport;
    tmp_uh.len = htons(skb->len);
    tmp_uh.check = 0;
    uh = (struct udphdr *)skb_push(skb, sizeof(struct udphdr));
    tmp_uh.len = htons(skb->len);
    memcpy((char *)uh, (char *)&tmp_uh, sizeof(struct udphdr));
#define ENABLE_UDP_CSUM
#ifdef ENABLE_UDP_CSUM
    uh->check = csum_tcpudp_magic(inet->saddr, inet->daddr, skb->len, 
                                IPPROTO_UDP, csum_partial(uh, skb->len, 0));
    if (!uh->check)
        uh->check = CSUM_MANGLED_0;
#endif
    skb_reset_transport_header(skb);

    /* fill ip hdr */
    iph = (struct iphdr *)skb_push(skb, sizeof(struct iphdr));
    /* Foxconn modified start pling 09/15/2010 */
    /* 'pppol2tp_fill_ip_header' need to use sk to fill in some info.
     * Use the tunnel sock info instead.
     */
    //pppol2tp_fill_ip_header(skb->sk, skb, &tmp_iph, skb->len);
    pppol2tp_fill_ip_header(session->tunnel_sock, skb, &tmp_iph, skb->len);
    /* Foxconn modified end pling 09/15/2010 */
    memcpy((char *)iph, (char *)&tmp_iph, sizeof(tmp_iph));
    skb_reset_network_header(skb);

	if (send)
		kfree(send);

    IPCB(skb)->flags |= IPSKB_REROUTED;
    dst_output(skb);
    return 1;
slow_path:
    /* foxconn wklin added end, 08/04/2010, @l2tp throughput */
	/* Unfortunately there doesn't appear to be a way for us to pass an skb
	 * to the UDP layer, we have to pretend to be sending ordinary data
	 * and use sendmsg
	 */
	msg = kmalloc(sizeof(struct msghdr), GFP_ATOMIC);
	if (msg == NULL) {
		error = -ENOBUFS;
		tunnel->stats.tx_errors++;
		session->stats.tx_errors++;
		goto end;
	}

	msg->msg_iov = kmalloc(3 * sizeof(struct iovec), GFP_ATOMIC);
	if (msg->msg_iov == NULL) {
		error = -ENOBUFS;
		tunnel->stats.tx_errors++;
		session->stats.tx_errors++;
		goto end;
	}
	msg->msg_iov[0].iov_base = &send->hdr;
	msg->msg_iov[0].iov_len	 = hdr_len;
	/* FIXME: do we need to handle skb fragments here? */
    msg->msg_iov[1].iov_base = &ppph;
    msg->msg_iov[1].iov_len  = sizeof(ppph);
	msg->msg_iov[2].iov_base = skb->data;
	msg->msg_iov[2].iov_len	 = skb->len;
	msg->msg_iovlen = 3;

	/* If the user calls sendto() that's just too bad */
    /* foxconn wklin modified start, 08/04/2010, 
     * remove this so we will have a cached dst in sk 
     * */
	//msg->msg_name	 = &session->tunnel_addr.addr;
	//msg->msg_namelen = sizeof(session->tunnel_addr.addr);
    msg->msg_name	 = NULL;
	msg->msg_namelen = 0;
    /* foxconn wklin moified end, 08/04/2010 */

	msg->msg_control    = NULL;
	msg->msg_controllen = 0;
	msg->msg_flags	    = MSG_DONTWAIT;	/* Need this to prevent blocking */

	send->session = session;
	send->tunnel = tunnel;
	send->msg = msg;
	send->skb = skb;
	send->tunnel_sock = session->tunnel_sock;
	send->iov = msg->msg_iov;
	send->total_len = skb->len + hdr_len + sizeof(ppph);
	init_sync_kiocb(&send->iocb, NULL);
	send->iocb.private = &send->siocb;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,20))
	INIT_WORK(&send->send_task, pppol2tp_wq_send);
#else
	INIT_WORK(&send->send_task, pppol2tp_wq_send, send);
#endif
	queue_work(tunnel->wq, &send->send_task);
	return 1;

end:
	if (msg != NULL)
		kfree(msg);
	if (send != NULL)
		kfree(send);

	EXIT_FUNCTION;
	return error;
}
/* foxconn wklin modified end, 04/14/2011 */
/*****************************************************************************
 * Session (and tunnel control) socket create/destroy.
 *****************************************************************************/

/* When the tunnel UDP socket is closed, all the attached sockets need to go
 * too. This handles that.
 */
static void pppol2tp_tunnel_closeall(struct pppol2tp_tunnel *tunnel)
{
	int hash;
	struct hlist_node *walk;
	struct hlist_node *tmp;
	struct pppol2tp_session *session;
	struct sock *sk;

	ENTER_FUNCTION;

	if (tunnel == NULL)
		BUG();

	PRINTK(tunnel->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
	       "%s: closing all sessions...\n", tunnel->name);

	for (hash = 0; hash < PPPOL2TP_HASH_SIZE; hash++) {
		hlist_for_each_safe(walk, tmp, &tunnel->session_hlist[hash]) {
			session = hlist_entry(walk, struct pppol2tp_session, hlist);

			sk = session->sock;

			PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
			       "%s: closing session\n", session->name);

			write_lock_bh(&tunnel->hlist_lock);
			hlist_del_init(&session->hlist);
			write_unlock_bh(&tunnel->hlist_lock);

			sock_hold(sk);

			lock_sock(sk);

			if (sk->sk_state & (PPPOX_CONNECTED | PPPOX_BOUND)) {
				pppox_unbind_sock(sk);
				sk->sk_state = PPPOX_DEAD;
				sk->sk_state_change(sk);
			}

			/* Purge any queued data */
			skb_queue_purge(&sk->sk_receive_queue);
			skb_queue_purge(&sk->sk_write_queue);

			release_sock(sk);

			DPRINTK(session->debug, "calling sock_put; refcnt=%d\n",
				sk->sk_refcnt.counter);
			sock_put(sk);
		}
	}

	EXIT_FUNCTION;
}

/* Really kill the tunnel.
 * Come here only when all sessions have been cleared from the tunnel.
 */
static void pppol2tp_tunnel_free(struct pppol2tp_tunnel *tunnel)
{
	struct sock *sk = tunnel->sock;

	ENTER_FUNCTION;

	/* Remove from socket list */
	list_del_init(&tunnel->list);

	sk->sk_prot = tunnel->old_proto;
       	sk->sk_data_ready = tunnel->old_data_ready;
	sk->sk_destruct = tunnel->old_sk_destruct;
	sk->sk_user_data = NULL;

	flush_workqueue(tunnel->wq);
	destroy_workqueue(tunnel->wq);

	DPRINTK(tunnel->debug, "%s: MOD_DEC_USE_COUNT\n", tunnel->name);
	kfree(tunnel);

	EXIT_FUNCTION;
}

/* Tunnel UDP socket destruct hook.
 * The tunnel context is deleted only when all session sockets have been
 * closed.
 */
static void pppol2tp_tunnel_destruct(struct sock *sk)
{
	struct pppol2tp_tunnel *tunnel;
	int error = 0;
	ENTER_FUNCTION;

	SOCK_2_TUNNEL(sk, tunnel, error, -EBADF, end, 0);

	PRINTK(tunnel->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
	       "%s: closing...\n", tunnel->name);

	pppol2tp_tunnel_closeall(tunnel);

end:
	EXIT_FUNCTION;
	return;
}

/* Really kill the socket. (Called from sock_put if refcnt == 0.)
 */
static void pppol2tp_session_destruct(struct sock *sk)
{
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
	struct pppox_opt *po = pppox_sk(sk);
#endif
	struct pppol2tp_session *session = NULL;
	int error = 0;

	ENTER_FUNCTION;

	if (sk->sk_user_data != NULL) {
		struct pppol2tp_tunnel *tunnel;

		SOCK_2_SESSION(sk, session, error, -EBADF, out, 0);
		skb_queue_purge(&session->reorder_q);

		/* Don't use SOCK_2_TUNNEL() here to get the tunnel context
		 * because the tunnel socket might have already been closed
		 * (its sk->sk_user_data will be NULL) so use the session's
		 * private tunnel ptr instead.
		 */
		tunnel = session->tunnel;
		if (tunnel != NULL) {
			if (tunnel->magic != L2TP_TUNNEL_MAGIC) {
				printk(KERN_ERR "%s: %s:%d: BAD TUNNEL MAGIC "
				       "( tunnel=%p magic=%x )\n",
				       __FUNCTION__, __FILE__, __LINE__,
				       tunnel, tunnel->magic);
				goto out;
			}
		}

		/* Delete tunnel context if this was the last session on the
		 * tunnel.  This was allocated when the first session was
		 * created on the tunnel. See
		 * pppol2tp_prepare_tunnel_socket().
		 */
		DPRINTK(tunnel->debug, "%s: session_count=%d\n",
			tunnel->name, atomic_read(&tunnel->session_count));
		if (atomic_dec_and_test(&tunnel->session_count)) {
			pppol2tp_tunnel_free(tunnel);
		}
	}

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
	if (po)
		kfree(po);
#endif

	if (session != NULL)
		kfree(session);

out:
	EXIT_FUNCTION;
}

/* Called when the PPPoX socket (session) is closed.
 */
static int pppol2tp_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct pppol2tp_session *session = NULL;
	struct pppol2tp_tunnel *tunnel;
	int error = 0;
	ENTER_FUNCTION;

	if (!sk)
		return 0;

	if (sock_flag(sk, SOCK_DEAD) != 0)
		return -EBADF;

	if (sk->sk_user_data) {	    /* Was this socket actually connected? */
		SOCK_2_SESSION(sk, session, error, -EBADF, end, 0);

		/* Don't use SOCK_2_TUNNEL() here to get the tunnel context
		 * because the tunnel socket might have already been closed
		 * (its sk->sk_user_data will be NULL) so use the session's
		 * private tunnel ptr instead.
		 */
		tunnel = session->tunnel;
		if (tunnel != NULL) {
			if (tunnel->magic == L2TP_TUNNEL_MAGIC) {
				/* Delete the session socket from the hash */
				write_lock_bh(&tunnel->hlist_lock);
				hlist_del_init(&session->hlist);
				write_unlock_bh(&tunnel->hlist_lock);
			} else {
				printk(KERN_ERR "%s: %s:%d: BAD TUNNEL MAGIC "
				       "( tunnel=%p magic=%x )\n",
				       __FUNCTION__, __FILE__, __LINE__,
				       tunnel, tunnel->magic);
				goto end;
			}
		}
	}

	lock_sock(sk);

	pppox_unbind_sock(sk);

	/* Signal the death of the socket. */
	sk->sk_state = PPPOX_DEAD;
	sock_orphan(sk);
	sock->sk = NULL;

	/* Purge any queued data */
	skb_queue_purge(&sk->sk_receive_queue);
	skb_queue_purge(&sk->sk_write_queue);

	release_sock(sk);

	if (session != NULL)
		DPRINTK(session->debug, "calling sock_put; refcnt=%d\n",
			session->sock->sk_refcnt.counter);
	sock_put(sk);

end:
	EXIT_FUNCTION;

	return error;
}

/* Copied from fget() in fs/file_table.c.
 * Allows caller to specify the pid that owns the fd.
 */
static struct file *pppol2tp_fget(pid_t pid, unsigned int fd)
{
	struct file *file;
	struct files_struct *files = current->files;

	if (pid != 0) {
		struct task_struct *tsk = find_task_by_pid(pid);
		if (tsk == NULL)
			return NULL;
		files = tsk->files;
	}

	spin_lock(&files->file_lock);
	file = fcheck_files(files, fd);
	if (file)
		get_file(file);
	spin_unlock(&files->file_lock);
	return file;
}

/* Copied from sockfd_lookup() in net/socket.c.
 * Allows caller to specify the pid that owns the fd.
 */
static struct socket *pppol2tp_sockfd_lookup(pid_t pid, int fd, int *err)
{
	struct file *file;
	struct inode *inode;
	struct socket *sock;

	if (!(file = pppol2tp_fget(pid, fd))) {
		*err = -EBADF;
		return NULL;
	}

	inode = file->f_dentry->d_inode;
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,10))
	if (!inode->i_sock || !(sock = SOCKET_I(inode))) {
#else
	if (!(sock = SOCKET_I(inode))) {
#endif
		*err = -ENOTSOCK;
		fput(file);
		return NULL;
	}

	if (sock->file != file) {
		printk(KERN_ERR "socki_lookup: socket file changed!\n");
		sock->file = file;
	}
	return sock;
}

/* Internal function to prepare a tunnel (UDP) socket to have PPPoX sockets
 * attached to it
 */
static struct sock *pppol2tp_prepare_tunnel_socket(pid_t pid, int fd,
						   u16 tunnel_id, int *error)
{
	int err;
	struct socket *sock = NULL;
	struct sock *sk;
	struct pppol2tp_tunnel *tunnel;
	struct sock *ret = NULL;

	ENTER_FUNCTION;

	/* Get the socket from the fd */
	err = -EBADF;
	sock = pppol2tp_sockfd_lookup(pid, fd, &err);
	if (!sock) {
		PRINTK(-1, PPPOL2TP_MSG_CONTROL, KERN_ERR,
		       "tunl %hu: sockfd_lookup(fd=%d) returned %d\n",
		       tunnel_id, fd, err);
		goto err;
	}

	/* Quick sanity checks */
	err = -ESOCKTNOSUPPORT;
	if (sock->type != SOCK_DGRAM) {
		PRINTK(-1, PPPOL2TP_MSG_CONTROL, KERN_ERR,
		       "tunl %hu: fd %d wrong type, got %d, expected %d\n",
		       tunnel_id, fd, sock->type, SOCK_DGRAM);
		goto err;
	}
	err = -EAFNOSUPPORT;
	if (sock->ops->family!=AF_INET) {
		PRINTK(-1, PPPOL2TP_MSG_CONTROL, KERN_ERR,
		       "tunl %hu: fd %d wrong family, got %d, expected %d\n",
		       tunnel_id, fd, sock->ops->family, AF_INET);
		goto err;
	}

	err = -ENOTCONN;
	sk = sock->sk;

	/* Check if this socket has already been prepped */
	tunnel = (struct pppol2tp_tunnel *)sk->sk_user_data;
	if (tunnel != NULL) {
		/* User-data field already set */
		err = -EBUSY;
		if (tunnel->magic != L2TP_TUNNEL_MAGIC) {
			printk(KERN_ERR "%s: %s:%d: BAD TUNNEL MAGIC "
			       "( tunnel=%p magic=%x )\n",
			       __FUNCTION__, __FILE__, __LINE__,
			       tunnel, tunnel->magic);
			goto err;
		}

		/* This socket has already been prepped */
		ret = tunnel->sock;
		goto out;
	}

	/* This socket is available and needs prepping. Create a new tunnel
	 * context and init it.
	 */
	sk->sk_user_data = tunnel = kmalloc(sizeof(struct pppol2tp_tunnel), GFP_KERNEL);
	if (sk->sk_user_data == NULL) {
		err = -ENOMEM;
		goto err;
	}

	memset(tunnel, 0, sizeof(struct pppol2tp_tunnel));

	tunnel->magic = L2TP_TUNNEL_MAGIC;
	sprintf(&tunnel->name[0], "tunl %hu", tunnel_id);

	tunnel->stats.tunnel_id = tunnel_id;

	tunnel->debug = PPPOL2TP_DEFAULT_DEBUG_FLAGS;

	DPRINTK(tunnel->debug, "tunl %hu: allocated tunnel=%p, sk=%p, sock=%p\n",
		tunnel_id, tunnel, sk, sock);

	tunnel->wq = create_workqueue("kl2tpd");
	if (!tunnel->wq) {
		err = -ENOMEM;
		goto err_free_tunnel;
	}

	/* Setup the new protocol stuff */
	tunnel->old_proto  = sk->sk_prot;
	tunnel->l2tp_proto = *sk->sk_prot;

	sk->sk_prot = &tunnel->l2tp_proto;

	tunnel->old_data_ready = sk->sk_data_ready;
	sk->sk_data_ready = &pppol2tp_data_ready;

	tunnel->old_sk_destruct = sk->sk_destruct;
	sk->sk_destruct = &pppol2tp_tunnel_destruct;

	tunnel->sock = sk;
	sk->sk_allocation = GFP_ATOMIC;

	rwlock_init(&tunnel->hlist_lock);

	/* Add tunnel to our list */
	INIT_LIST_HEAD(&tunnel->list);
	list_add(&tunnel->list, &pppol2tp_tunnel_list);

	ret = tunnel->sock;

	*error = 0;
out:
	if (sock)
		sockfd_put(sock);
	EXIT_FUNCTION;

	return ret;

err_free_tunnel:
	kfree(tunnel);
err:
	*error = err;
	goto out;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
static struct proto pppol2tp_sk_proto = {
	.name	  = "PPPOL2TP",
	.owner	  = THIS_MODULE,
	.obj_size = sizeof(struct pppox_sock),
};
#endif

/* socket() handler. Initialize a new struct sock.
 */
static int pppol2tp_create(struct socket *sock)
{
	int error = -ENOMEM;
	struct sock *sk;
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
	struct pppox_opt *po;
#endif

	ENTER_FUNCTION;
	DPRINTK(-1, "sock=%p\n", sock);

	try_module_get(THIS_MODULE);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
	sk = sk_alloc(PF_PPPOX, GFP_KERNEL, &pppol2tp_sk_proto, 1);
#else
	sk = sk_alloc(PF_PPPOX, GFP_KERNEL, 1, NULL);
#endif
	if (!sk)
		goto out;

	sock_init_data(sock, sk);

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
	sk_set_owner(sk, THIS_MODULE);
#endif

	sock->state  = SS_UNCONNECTED;
	sock->ops    = &pppol2tp_ops;

	sk->sk_backlog_rcv = pppol2tp_recv_core;
	sk->sk_protocol    = PX_PROTO_OL2TP;
	sk->sk_family      = PF_PPPOX;
	sk->sk_state       = PPPOX_NONE;
	sk->sk_type        = SOCK_STREAM;
	sk->sk_destruct    = pppol2tp_session_destruct;

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
	po = sk->sk_protinfo = kmalloc(sizeof(struct pppox_opt), GFP_KERNEL);
	if (!po) {
		error = -ENOMEM;
		sk_free(sk);
		goto out;
	}

	memset((void *) po, 0, sizeof(*po));
	po->sk = sk;

	sock->sk = sk;
#endif
	error = 0;

out:
	module_put(THIS_MODULE);

	EXIT_FUNCTION;
	return error;
}

/* connect() handler..	Attach a PPPoX socket to a tunnel UDP socket
 */
int pppol2tp_connect(struct socket *sock, struct sockaddr *uservaddr,
		     int sockaddr_len, int flags)
{
	struct sock *sk = sock->sk;
	struct sockaddr_pppol2tp *sp = (struct sockaddr_pppol2tp *) uservaddr;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
	struct pppox_sock *po = pppox_sk(sk);
#else
	struct pppox_opt *po = pppox_sk(sk);
#endif
	struct sock *tunnel_sock = NULL;
	struct pppol2tp_session *session = NULL;
	struct pppol2tp_tunnel *tunnel;
	struct dst_entry *dst;
	int error = 0;

	ENTER_FUNCTION;

	DPRINTK(-1, "sock=%p, uservaddr=%p, sockaddr_len=%d, flags=%d, addr=%x/%hu\n",
		sock, uservaddr, sockaddr_len, flags,
		ntohl(sp->pppol2tp.addr.sin_addr.s_addr), ntohs(sp->pppol2tp.addr.sin_port));
	lock_sock(sk);

	error = -EINVAL;
	if (sp->sa_protocol != PX_PROTO_OL2TP)
		goto end;

	/* Check for already bound sockets */
	error = -EBUSY;
	if (sk->sk_state & PPPOX_CONNECTED)
		goto end;

	/* We don't supporting rebinding anyway */
	if (sk->sk_user_data)
		goto end; /* socket is already attached */

	/* Don't bind if s_tunnel is 0 */
	error = -EINVAL;
	if (sp->pppol2tp.s_tunnel == 0)
		goto end;

	/* Look up the tunnel socket and configure it if necessary */
	tunnel_sock = pppol2tp_prepare_tunnel_socket(sp->pppol2tp.pid,
						     sp->pppol2tp.fd,
						     sp->pppol2tp.s_tunnel,
						     &error);
	if (tunnel_sock == NULL)
		goto end;
	tunnel = tunnel_sock->sk_user_data;

	/* Allocate and initialize a new session context.
	 */
	session = kmalloc(sizeof(struct pppol2tp_session), GFP_KERNEL);
	if (session == NULL) {
		error = -ENOMEM;
		goto end;
	}

	memset(session, 0, sizeof(struct pppol2tp_session));

	skb_queue_head_init(&session->reorder_q);

	session->magic	     = L2TP_SESSION_MAGIC;
	session->owner	     = current->pid;
	session->sock	     = sk;
	session->tunnel	     = tunnel;
	session->tunnel_sock = tunnel_sock;
	session->tunnel_addr = sp->pppol2tp;
	sprintf(&session->name[0], "sess %hu/%hu",
		session->tunnel_addr.s_tunnel,
		session->tunnel_addr.s_session);

	session->stats.tunnel_id  = session->tunnel_addr.s_tunnel;
	session->stats.session_id = session->tunnel_addr.s_session;

	INIT_HLIST_NODE(&session->hlist);

	session->debug = PPPOL2TP_DEFAULT_DEBUG_FLAGS;

	/* Default MTU must allow space for UDP/L2TP/PPP
	 * headers. Leave some slack.
	 */
	session->mtu = session->mru = 1500 - PPPOL2TP_HEADER_OVERHEAD;

	/* If PMTU discovery was enabled, use the MTU that was discovered */
	dst = sk_dst_get(sk);
	if (dst != NULL) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
		u32 pmtu = dst_mtu(__sk_dst_get(sk));
#else
		u32 pmtu = dst_pmtu(__sk_dst_get(sk));
#endif
		if (pmtu != 0) {
			session->mtu = session->mru = pmtu -
				PPPOL2TP_HEADER_OVERHEAD;
			DPRINTK(session->debug,
				"%s: MTU set by Path MTU discovery: mtu=%d\n",
				session->name, session->mtu);
		}
		dst_release(dst);
	}

	/* Special case: if source & dest session_id == 0x0000, this socket is
	 * being created to manage the tunnel. Don't add the session to the
	 * session hash list, just set up the internal context for use by
	 * ioctl() and sockopt() handlers.
	 */
	if ((session->tunnel_addr.s_session == 0) &&
	    (session->tunnel_addr.d_session == 0)) {
		error = 0;
		DPRINTK(session->debug,
			"tunl %hu: socket created for tunnel mgmt ops\n",
			session->tunnel_addr.s_tunnel);
		sk->sk_user_data = session;
		goto out_no_ppp;
	}

	DPRINTK(session->debug, "%s: allocated session=%p, sock=%p, owner=%d\n",
		session->name, session, sk, session->owner);

	/* Add session to the tunnel's hash list */
	SOCK_2_TUNNEL(tunnel_sock, tunnel, error, -EBADF, end, 0);
	write_lock_bh(&tunnel->hlist_lock);
	hlist_add_head(&session->hlist,
		       pppol2tp_session_id_hash(tunnel,
						session->tunnel_addr.s_session));
	write_unlock_bh(&tunnel->hlist_lock);

	/* This is how we get the session context from the socket. */
	sk->sk_user_data = session;

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
	/* We don't store any more options in the pppox_opt, everything is in
	 * user_data (struct pppol2tp_session)
	 */
	po->sk = sk;
#endif

	/* Right now, because we don't have a way to push the incoming skb's
	 * straight through the UDP layer, the only header we need to worry
	 * about is the L2TP header. This size is different depending on
	 * whether sequence numbers are enabled for the data channel.
	 */
	po->chan.hdrlen = PPPOL2TP_L2TP_HDR_SIZE_NOSEQ;

	po->chan.private = sk;
	po->chan.ops	 = &pppol2tp_chan_ops;
	po->chan.mtu	 = session->mtu;

	error = ppp_register_channel(&po->chan);
	if (error)
		goto end;

out_no_ppp:
	atomic_inc(&tunnel->session_count);
	sk->sk_state = PPPOX_CONNECTED;
	PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
	       "%s: created\n", session->name);

end:
	release_sock(sk);

	if (error != 0)
		PRINTK(session ? session->debug : -1, PPPOL2TP_MSG_CONTROL, KERN_WARNING,
		       "%s: connect failed: %d\n", session->name, error);

	EXIT_FUNCTION;

	return error;
}

/* getname() support.
 */
static int pppol2tp_getname(struct socket *sock, struct sockaddr *uaddr,
			    int *usockaddr_len, int peer)
{
	int len = sizeof(struct sockaddr_pppol2tp);
	struct sockaddr_pppol2tp sp;
	int error = 0;
	struct pppol2tp_session *session;

	ENTER_FUNCTION;

	error = -ENOTCONN;
	if (sock->sk->sk_state != PPPOX_CONNECTED)
		goto end;

	SOCK_2_SESSION(sock->sk, session, error, -EBADF, end, 0);

	sp.sa_family	= AF_PPPOX;
	sp.sa_protocol	= PX_PROTO_OL2TP;
	memcpy(&sp.pppol2tp, &session->tunnel_addr,
	       sizeof(struct pppol2tp_addr));

	memcpy(uaddr, &sp, len);

	*usockaddr_len = len;

	error = 0;
end:
	EXIT_FUNCTION;
	return error;
}

/****************************************************************************
 * ioctl() handlers.
 *
 * The PPPoX socket is created for L2TP sessions: tunnels have their own UDP
 * sockets. However, in order to control kernel tunnel features, we allow
 * userspace to create a special "tunnel" PPPoX socket which is used for
 * control only.  Tunnel PPPoX sockets have session_id == 0 and simply allow
 * the user application to issue L2TP setsockopt(), getsockopt() and ioctl()
 * calls.
 ****************************************************************************/

/* Session ioctl helper.
 */
static int pppol2tp_session_ioctl(struct pppol2tp_session *session,
				  unsigned int cmd, unsigned long arg)
{
	struct ifreq ifr;
	int err = 0;
	struct sock *sk = session->sock;
	int val = (int) arg;

	sock_hold(sk);

	PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_DEBUG,
	       "%s: pppol2tp_session_ioctl(cmd=%#x, arg=%#lx)\n",
	       session->name, cmd, arg);

	switch (cmd) {
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16))
		/* From pppox_ioctl() in drivers/net/pppox.c */
	case PPPIOCGCHAN: {
		int index;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
		struct pppox_sock *po = pppox_sk(sk);
#else
		struct pppox_opt *po = pppox_sk(sk);
#endif

		err = -ENOTCONN;
		if (!(sk->sk_state & PPPOX_CONNECTED))
			break;

		err = -EINVAL;
		index = ppp_channel_index(&po->chan);
		if (put_user(index , (int __user *) arg))
			break;

		err = 0;
		sk->sk_state |= PPPOX_BOUND;
		break;
	}
#endif /* (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)) */
	case SIOCGIFMTU:
		err = -ENXIO;
		if (!(sk->sk_state & PPPOX_CONNECTED))
			break;

		err = -EFAULT;
		if (copy_from_user(&ifr, (void __user *) arg, sizeof(struct ifreq)))
			break;
		ifr.ifr_mtu = session->mtu;
		if (copy_to_user((void __user *) arg, &ifr, sizeof(struct ifreq)))
			break;

		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get mtu=%d\n", session->name, session->mtu);
		err = 0;
		break;

	case SIOCSIFMTU:
		err = -ENXIO;
		if (!(sk->sk_state & PPPOX_CONNECTED))
			break;

		err = -EFAULT;
		if (copy_from_user(&ifr, (void __user *) arg, sizeof(struct ifreq)))
			break;

		session->mtu = ifr.ifr_mtu;
;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set mtu=%d\n", session->name, session->mtu);
		err = 0;
		break;

	case PPPIOCGMRU:
		err = -ENXIO;
		if (!(sk->sk_state & PPPOX_CONNECTED))
			break;

		err = -EFAULT;
		if (put_user(session->mru, (int __user *) arg))
			break;

		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get mru=%d\n", session->name, session->mru);
		err = 0;
		break;

	case PPPIOCSMRU:
		err = -ENXIO;
		if (!(sk->sk_state & PPPOX_CONNECTED))
			break;

		err = -EFAULT;
		if (get_user(val,(int __user *) arg))
			break;

		session->mru = val;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set mru=%d\n", session->name, session->mru);
		err = 0;
		break;

	case PPPIOCGFLAGS:
		err = -EFAULT;
		if (put_user(session->flags, (int __user *) arg))
			break;

		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get flags=%d\n", session->name, session->flags);
		err = 0;
		break;

	case PPPIOCSFLAGS:
		err = -EFAULT;
		if (get_user(val, (int __user *) arg))
			break;
		session->flags = val;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set flags=%d\n", session->name, session->flags);
		err = 0;
		break;

	case PPPIOCGL2TPSTATS:
		err = -ENXIO;

		if (!(sk->sk_state & PPPOX_CONNECTED))
			break;

		if (copy_to_user((void __user *) arg, &session->stats,
				 sizeof(session->stats)))
			break;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get L2TP stats\n", session->name);
		err = 0;
		break;

	default:
		err = -ENOSYS;
		break;
	}

	sock_put(sk);

	return err;
}

/* Tunnel ioctl helper.
 *
 * Note the special handling for PPPIOCGL2TPSTATS below. If the ioctl data
 * specifies a session_id, the session ioctl handler is called. This allows an
 * application to retrieve session stats via a tunnel socket.
 */
static int pppol2tp_tunnel_ioctl(struct pppol2tp_tunnel *tunnel,
				 unsigned int cmd, unsigned long arg)
{
	int err = 0;
	struct sock *sk = tunnel->sock;
	struct pppol2tp_ioc_stats stats_req;

	sock_hold(sk);

	PRINTK(tunnel->debug, PPPOL2TP_MSG_CONTROL, KERN_DEBUG,
	       "%s: pppol2tp_tunnel_ioctl(cmd=%#x, arg=%#lx)\n", tunnel->name,
	       cmd, arg);

	switch (cmd) {
	case PPPIOCGL2TPSTATS:
		err = -ENXIO;

		if (!(sk->sk_state & PPPOX_CONNECTED))
			break;

		if (copy_from_user(&stats_req, (void __user *) arg,
				   sizeof(stats_req))) {
			err = -EFAULT;
			break;
		}
		if (stats_req.session_id != 0) {
			/* resend to session ioctl handler */
			struct pppol2tp_session *session =
				pppol2tp_session_find(tunnel, stats_req.session_id);
			if (session != NULL)
				err = pppol2tp_session_ioctl(session, cmd, arg);
			else
				err = -EBADR;
			break;
		}
#ifdef CONFIG_XFRM
		tunnel->stats.using_ipsec = (sk->sk_policy[0] || sk->sk_policy[1]) ? 1 : 0;
#endif
		if (copy_to_user((void __user *) arg, &tunnel->stats,
				 sizeof(tunnel->stats))) {
			err = -EFAULT;
			break;
		}
		PRINTK(tunnel->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get L2TP stats\n", tunnel->name);
		err = 0;
		break;

	default:
		err = -ENOSYS;
		break;
	}

	sock_put(sk);

	return err;
}

/* Main ioctl() handler.
 * Dispatch to tunnel or session helpers depending on the socket.
 */
static int pppol2tp_ioctl(struct socket *sock, unsigned int cmd,
			    unsigned long arg)
{
	struct sock *sk = sock->sk;
	struct pppol2tp_session *session;
	struct pppol2tp_tunnel *tunnel;
	int err = 0;

	ENTER_FUNCTION;

	if (!sk)
		return 0;

	if (sock_flag(sk, SOCK_DEAD) != 0)
		return -EBADF;

	if ((sk->sk_user_data == NULL) ||
	    (!(sk->sk_state & (PPPOX_CONNECTED | PPPOX_BOUND)))) {
		err = -ENOTCONN;
		DPRINTK(-1, "ioctl: socket %p not connected.\n", sk);
		goto end;
	}

	SOCK_2_SESSION(sk, session, err, -EBADF, end, 0);
	SOCK_2_TUNNEL(session->tunnel_sock, tunnel, err, -EBADF, end, 1);

	/* Special case: if session's session_id is zero, treat ioctl as a
	 * tunnel ioctl
	 */
	if ((session->tunnel_addr.s_session == 0) &&
	    (session->tunnel_addr.d_session == 0)) {
		err = pppol2tp_tunnel_ioctl(tunnel, cmd, arg);
		goto end;
	}

	err = pppol2tp_session_ioctl(session, cmd, arg);

end:
	EXIT_FUNCTION;
	return err;
}

/*****************************************************************************
 * setsockopt() / getsockopt() support.
 *
 * The PPPoX socket is created for L2TP sessions: tunnels have their own UDP
 * sockets. In order to control kernel tunnel features, we allow userspace to
 * create a special "tunnel" PPPoX socket which is used for control only.
 * Tunnel PPPoX sockets have session_id == 0 and simply allow the user
 * application to issue L2TP setsockopt(), getsockopt() and ioctl() calls.
 *****************************************************************************/

/* Tunnel setsockopt() helper.
 */
static int pppol2tp_tunnel_setsockopt(struct sock *sk,
				      struct pppol2tp_tunnel *tunnel,
				      int optname, int val)
{
	int err = 0;

	switch (optname) {
	case PPPOL2TP_SO_DEBUG:
		tunnel->debug = val;
		PRINTK(tunnel->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set debug=%x\n", tunnel->name, tunnel->debug);
		break;

	default:
		err = -ENOPROTOOPT;
		break;
	}

	return err;
}

/* Session setsockopt helper.
 */
static int pppol2tp_session_setsockopt(struct sock *sk,
				       struct pppol2tp_session *session,
				       int optname, int val)
{
	int err = 0;

	switch (optname) {
	case PPPOL2TP_SO_RECVSEQ:
		if ((val != 0) && (val != 1)) {
			err = -EINVAL;
			break;
		}
		session->recv_seq = val ? -1 : 0;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set recv_seq=%d\n", session->name,
		       session->recv_seq);
		break;

	case PPPOL2TP_SO_SENDSEQ:
		if ((val != 0) && (val != 1)) {
			err = -EINVAL;
			break;
		}
		session->send_seq = val ? -1 : 0;
		{
			/* FIXME: is it safe to change the ppp channel's
			 * hdrlen on the fly?
			 */
			struct sock *sk	     = session->sock;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
			struct pppox_sock *po = pppox_sk(sk);
#else
			struct pppox_opt *po = pppox_sk(sk);
#endif
			po->chan.hdrlen = val ? PPPOL2TP_L2TP_HDR_SIZE_SEQ :
				PPPOL2TP_L2TP_HDR_SIZE_NOSEQ;
		}
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set send_seq=%d\n", session->name, session->send_seq);
		break;

	case PPPOL2TP_SO_LNSMODE:
		if ((val != 0) && (val != 1)) {
			err = -EINVAL;
			break;
		}
		session->lns_mode = val ? -1 : 0;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set lns_mode=%d\n", session->name,
		       session->lns_mode);
		break;

	case PPPOL2TP_SO_DEBUG:
		session->debug = val;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set debug=%x\n", session->name, session->debug);
		break;

	case PPPOL2TP_SO_REORDERTO:
		session->reorder_timeout = MS_TO_JIFFIES(val);
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: set reorder_timeout=%d\n", session->name,
		       session->reorder_timeout);
		break;

	default:
		err = -ENOPROTOOPT;
		break;
	}

	return err;
}

/* Main setsockopt() entry point.
 * Does API checks, then calls either the tunnel or session setsockopt
 * handler, according to whether the PPPoL2TP socket is a for a regular
 * session or the special tunnel type.
 */
static int pppol2tp_setsockopt(struct socket *sock, int level, int optname,
			       char *optval, int optlen)
{
	struct sock *sk = sock->sk;
	struct pppol2tp_session *session = sk->sk_user_data;
	struct pppol2tp_tunnel *tunnel;
	int val;
	int err = 0;

	if (level != SOL_PPPOL2TP)
        return -EINVAL;
		//return udp_prot.setsockopt(sk, level, optname, optval, optlen);

	if (optlen<sizeof(int))
		return -EINVAL;

	if (get_user(val, (int __user *)optval))
		return -EFAULT;

	if (sk->sk_user_data == NULL) {
		err = -ENOTCONN;
		DPRINTK(-1, "setsockopt: socket %p not connected.\n", sk);
		goto end;
	}

	SOCK_2_SESSION(sk, session, err, -EBADF, end, 0);
	SOCK_2_TUNNEL(session->tunnel_sock, tunnel, err, -EBADF, end, 1);

	lock_sock(sk);

	/* Special case: if session_id == 0x0000, treat as operation on tunnel
	 */
	if ((session->tunnel_addr.s_session == 0) &&
	    (session->tunnel_addr.d_session == 0))
		err = pppol2tp_tunnel_setsockopt(sk, tunnel, optname, val);
	else
		err = pppol2tp_session_setsockopt(sk, session, optname, val);

	release_sock(sk);
end:
	return err;
}

/* Tunnel getsockopt helper.
 */
static int pppol2tp_tunnel_getsockopt(struct sock *sk,
				      struct pppol2tp_tunnel *tunnel,
				      int optname, int *val)
{
	int err = 0;

	switch (optname) {
	case PPPOL2TP_SO_DEBUG:
		*val = tunnel->debug;
		PRINTK(tunnel->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get debug=%x\n", tunnel->name, tunnel->debug);
		break;

	default:
		err = -ENOPROTOOPT;
		break;
	}

	return err;
}

/* Session getsockopt helper.
 */
static int pppol2tp_session_getsockopt(struct sock *sk,
				       struct pppol2tp_session *session,
				       int optname, int *val)
{
	int err = 0;

	switch (optname) {
	case PPPOL2TP_SO_RECVSEQ:
		*val = session->recv_seq;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get recv_seq=%d\n", session->name, *val);
		break;

	case PPPOL2TP_SO_SENDSEQ:
		*val = session->send_seq;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get send_seq=%d\n", session->name, *val);
		break;

	case PPPOL2TP_SO_LNSMODE:
		*val = session->lns_mode;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get lns_mode=%d\n", session->name, *val);
		break;

	case PPPOL2TP_SO_DEBUG:
		*val = session->debug;
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get debug=%d\n", session->name, *val);
		break;

	case PPPOL2TP_SO_REORDERTO:
		*val = JIFFIES_TO_MS(session->reorder_timeout);
		PRINTK(session->debug, PPPOL2TP_MSG_CONTROL, KERN_INFO,
		       "%s: get reorder_timeout=%d\n", session->name, *val);
		break;

	default:
		err = -ENOPROTOOPT;
	}

	return err;
}

/* Main getsockopt() entry point.
 * Does API checks, then calls either the tunnel or session getsockopt
 * handler, according to whether the PPPoX socket is a for a regular session
 * or the special tunnel type.
 */
static int pppol2tp_getsockopt(struct socket *sock, int level,
			       int optname, char *optval, int *optlen)
{
	struct sock *sk = sock->sk;
	struct pppol2tp_session *session = sk->sk_user_data;
	struct pppol2tp_tunnel *tunnel;
	int val, len;
	int err = 0;

	if (level != SOL_PPPOL2TP)
        return -EINVAL;
		//return udp_prot.getsockopt(sk, level, optname, optval, optlen);

	if (get_user(len, (int __user *) optlen))
		return -EFAULT;

	len = min_t(unsigned int, len, sizeof(int));

	if (len < 0)
		return -EINVAL;

	if (sk->sk_user_data == NULL) {
		err = -ENOTCONN;
		DPRINTK(-1, "getsockopt: socket %p not connected.\n", sk);
		goto end;
	}

	/* Get the session and tunnel contexts */
	SOCK_2_SESSION(sk, session, err, -EBADF, end, 0);
	SOCK_2_TUNNEL(session->tunnel_sock, tunnel, err, -EBADF, end, 1);

	/* Special case: if session_id == 0x0000, treat as operation on tunnel */
	if ((session->tunnel_addr.s_session == 0) &&
	    (session->tunnel_addr.d_session == 0))
		err = pppol2tp_tunnel_getsockopt(sk, tunnel, optname, &val);
	else
		err = pppol2tp_session_getsockopt(sk, session, optname, &val);


	if (put_user(len, (int __user *) optlen))
		return -EFAULT;

	if (copy_to_user((void __user *) optval, &val, len))
		return -EFAULT;

end:
	return err;
}

/*****************************************************************************
 * /proc filesystem for debug
 *****************************************************************************/

#ifdef CONFIG_PROC_FS

#include <linux/seq_file.h>

static int pppol2tp_proc_open(struct inode *inode, struct file *file);
static void *pppol2tp_proc_start(struct seq_file *m, loff_t *_pos);
static void *pppol2tp_proc_next(struct seq_file *p, void *v, loff_t *pos);
static void pppol2tp_proc_stop(struct seq_file *p, void *v);
static int pppol2tp_proc_show(struct seq_file *m, void *v);

static struct proc_dir_entry *pppol2tp_proc;

static struct seq_operations pppol2tp_proc_ops = {
	.start		= pppol2tp_proc_start,
	.next		= pppol2tp_proc_next,
	.stop		= pppol2tp_proc_stop,
	.show		= pppol2tp_proc_show,
};

static struct file_operations pppol2tp_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= pppol2tp_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int pppol2tp_proc_open(struct inode *inode, struct file *file)
{
	struct seq_file *m;
	int ret = 0;

	ENTER_FUNCTION;
	ret = seq_open(file, &pppol2tp_proc_ops);
	if (ret < 0)
		goto out;

	m	   = file->private_data;
	m->private = PDE(inode)->data;

out:
	EXIT_FUNCTION;
	return ret;
}

static void *pppol2tp_proc_start(struct seq_file *m, loff_t *_pos)
{
	struct pppol2tp_tunnel *tunnel = NULL;
	loff_t pos = *_pos;
	struct list_head *walk;
	struct list_head *tmp;

	ENTER_FUNCTION;

	/* allow for the header line */
	if (!pos) {
		tunnel = (void *)1;
		goto out;
	}
	pos--;

	/* find the n'th element in the list */
	list_for_each_safe(walk, tmp, &pppol2tp_tunnel_list) {
		tunnel = list_entry(walk, struct pppol2tp_tunnel, list);
		if (!pos--) {
			sock_hold(tunnel->sock);
			goto out;
		}
	}
	tunnel = NULL;

out:
	EXIT_FUNCTION;

	return tunnel;
}

static void *pppol2tp_proc_next(struct seq_file *p, void *v, loff_t *pos)
{
	struct pppol2tp_tunnel *tunnel = v;
	struct list_head *tmp;
	struct list_head *list;

	ENTER_FUNCTION;

	(*pos)++;

	if (v == (void *)1)
		list = &pppol2tp_tunnel_list;
	else
		list = &tunnel->list;

	tmp = list->next;
	if (tmp == &pppol2tp_tunnel_list)
		tunnel = NULL;
	else
		tunnel = list_entry(tmp, struct pppol2tp_tunnel, list);

	EXIT_FUNCTION;

	return tunnel;
}

static void pppol2tp_proc_stop(struct seq_file *p, void *v)
{
	struct pppol2tp_tunnel *tunnel = v;

	ENTER_FUNCTION;

	if (tunnel != NULL)
		sock_put(tunnel->sock);

	EXIT_FUNCTION;
}

static int pppol2tp_proc_show(struct seq_file *m, void *v)
{
	struct pppol2tp_tunnel *tunnel = v;
	struct pppol2tp_session *session;
	struct hlist_node *walk;
	struct hlist_node *tmp;
	int i;

	ENTER_FUNCTION;

	/* display header on line 1 */
	if (v == (void *)1) {
		seq_puts(m, "PPPoL2TP driver info, " PPPOL2TP_DRV_VERSION "\n");
		seq_puts(m, "TUNNEL name, user-data-ok "
			 "session-count magic-ok\n");
		seq_puts(m, " debug tx-pkts/bytes/errs rx-pkts/bytes/errs\n");
		seq_puts(m, "  SESSION name, addr/port src-tid/sid "
			 "dest-tid/sid state user-data-ok magic-ok\n");
		seq_puts(m, "   mtu/mru/rcvseq/sendseq/lns debug reorderto\n");
		seq_puts(m, "   nr/ns tx-pkts/bytes/errs rx-pkts/bytes/errs\n");
		goto out;
	}

	seq_printf(m, "TUNNEL '%s', %c %d MAGIC %s\n",
		   tunnel->name,
		   (tunnel == tunnel->sock->sk_user_data) ? 'Y':'N',
		   atomic_read(&tunnel->session_count),
		   (tunnel->magic == L2TP_TUNNEL_MAGIC) ? "OK" : "BAD");
	seq_printf(m, " %08x %llu/%llu/%llu %llu/%llu/%llu\n",
		   tunnel->debug,
		   tunnel->stats.tx_packets, tunnel->stats.tx_bytes,
		   tunnel->stats.tx_errors,
		   tunnel->stats.rx_packets, tunnel->stats.rx_bytes,
		   tunnel->stats.rx_errors);

	if (tunnel->magic != L2TP_TUNNEL_MAGIC) {
		seq_puts(m, "*** Aborting ***\n");
		goto out;
	}

	for (i = 0; i < PPPOL2TP_HASH_SIZE; i++) {
		hlist_for_each_safe(walk, tmp, &tunnel->session_hlist[i]) {
			session = hlist_entry(walk, struct pppol2tp_session, hlist);
			seq_printf(m, "  SESSION '%s' %08X/%d %04X/%04X -> "
				   "%04X/%04X %d %c MAGIC %s\n",
				   session->name,
				   htonl(session->tunnel_addr.addr.sin_addr.s_addr),
				   htons(session->tunnel_addr.addr.sin_port),
				   session->tunnel_addr.s_tunnel,
				   session->tunnel_addr.s_session,
				   session->tunnel_addr.d_tunnel,
				   session->tunnel_addr.d_session,
				   session->sock->sk_state,
				   (session == session->sock->sk_user_data) ?
				   'Y' : 'N',
				   (session->magic == L2TP_SESSION_MAGIC) ?
				   "OK" : "BAD");

			seq_printf(m, "   %d/%d/%c/%c/%s %08x %d\n",
				   session->mtu, session->mru,
				   session->recv_seq ? 'R' : '-',
				   session->send_seq ? 'S' : '-',
				   session->lns_mode ? "LNS" : "LAC",
				   session->debug,
				   JIFFIES_TO_MS(session->reorder_timeout));
			seq_printf(m, "   %hu/%hu %llu/%llu/%llu %llu/%llu/%llu\n",
				   session->nr, session->ns,
				   session->stats.tx_packets,
				   session->stats.tx_bytes,
				   session->stats.tx_errors,
				   session->stats.rx_packets,
				   session->stats.rx_bytes,
				   session->stats.rx_errors);

			if (session->magic != L2TP_SESSION_MAGIC) {
				seq_puts(m, "*** Aborting ***\n");
				goto out;
			}
		}
	}
out:
	seq_puts(m, "\n");

	EXIT_FUNCTION;

	return 0;
}

#endif /* CONFIG_PROC_FS */

/*****************************************************************************
 * Init and cleanup
 *****************************************************************************/

static struct proto_ops pppol2tp_ops = {
	.family		= AF_PPPOX,
	.owner		= THIS_MODULE,
	.release	= pppol2tp_release,
	.bind		= sock_no_bind,
	.connect	= pppol2tp_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= pppol2tp_getname,
	.poll		= datagram_poll,
	.listen		= sock_no_listen,
	.shutdown	= sock_no_shutdown,
	.setsockopt	= pppol2tp_setsockopt,
	.getsockopt	= pppol2tp_getsockopt,
	.sendmsg	= pppol2tp_sendmsg,
	.recvmsg	= pppol2tp_recvmsg,
	.mmap		= sock_no_mmap,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16))
	.ioctl		= pppol2tp_ioctl,
#else
	.ioctl		= pppox_ioctl,
#endif
};

struct pppox_proto pppol2tp_proto = {
	.create		= pppol2tp_create,
	.ioctl		= pppol2tp_ioctl
};

int __init pppol2tp_init(void)
{
	int err;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
	err = proto_register(&pppol2tp_sk_proto, 0);
	if (err)
		goto out;
#endif
	err = register_pppox_proto(PX_PROTO_OL2TP, &pppol2tp_proto);
	if (err)
		goto out_unregister_pppol2tp_proto;

#ifdef CONFIG_PROC_FS
	pppol2tp_proc = create_proc_entry("pppol2tp", 0, proc_net);
	if (!pppol2tp_proc) {
		err = -ENOMEM;
		goto out_unregister_pppox_proto;
	}
	pppol2tp_proc->proc_fops = &pppol2tp_proc_fops;
#endif /* CONFIG_PROC_FS */
	printk(KERN_INFO "PPPoL2TP kernel driver, %s\n",
	       PPPOL2TP_DRV_VERSION);

out:
	return err;

out_unregister_pppox_proto:
	unregister_pppox_proto(PX_PROTO_OL2TP);
out_unregister_pppol2tp_proto:
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
	proto_unregister(&pppol2tp_sk_proto);
#endif
	goto out;
}

void __exit pppol2tp_exit(void)
{
	unregister_pppox_proto(PX_PROTO_OL2TP);

#ifdef CONFIG_PROC_FS
	remove_proc_entry("pppol2tp", proc_net);
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
	proto_unregister(&pppol2tp_sk_proto);
#endif
}

module_init(pppol2tp_init);
module_exit(pppol2tp_exit);

MODULE_AUTHOR("Martijn van Oosterhout <kleptog@svana.org>");
MODULE_DESCRIPTION("PPP over L2TP over UDP");
MODULE_LICENSE("GPL");
MODULE_VERSION(PPPOL2TP_DRV_VERSION);
