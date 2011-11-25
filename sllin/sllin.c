/*
 * sllin.c - serial line LIN interface driver (using tty line discipline)
 *
 * This file is derived from linux/drivers/net/slip.c
 *
 * slip.c Authors  : Laurence Culhane <loz@holmes.demon.co.uk>
 *                   Fred N. van Kempen <waltje@uwalt.nl.mugnet.org>
 * sllin.c Author  : Oliver Hartkopp <socketcan@hartkopp.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307. You can also get it
 * at http://www.gnu.org/licenses/gpl.html
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <socketcan-users@lists.berlios.de>
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>

#include <asm/system.h>
#include <linux/uaccess.h>
#include <linux/bitops.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/can.h>
#include <linux/kthread.h>

/* Should be in include/linux/tty.h */
#define N_SLLIN         25

static __initdata const char banner[] =
	KERN_INFO "sllin: serial line LIN interface driver\n";

MODULE_ALIAS_LDISC(N_SLLIN);
MODULE_DESCRIPTION("serial line LIN interface");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Oliver Hartkopp <socketcan@hartkopp.net>");

#define SLLIN_MAGIC 0x53CA

static int maxdev = 10;		/* MAX number of SLLIN channels;
				   This can be overridden with
				   insmod sllin.ko maxdev=nnn	*/
module_param(maxdev, int, 0);
MODULE_PARM_DESC(maxdev, "Maximum number of sllin interfaces");

/* maximum buffer len to store whole LIN message*/
#define SLLIN_DATA_MAX	 8
#define SLLIN_BUFF_LEN	(1 /*break*/ + 1 /*sync*/ + 1 /*ID*/ + \
                         SLLIN_DATA_MAX + 1 /*checksum*/)
#define SLLIN_BUFF_BREAK 0
#define SLLIN_BUFF_SYNC	 1
#define SLLIN_BUFF_ID	 2
#define SLLIN_BUFF_DATA	 3

enum slstate {
	SLSTATE_IDLE = 0,
	SLSTATE_BREAK_SENT,
	SLSTATE_ID_SENT,
	SLSTATE_RESPONSE_WAIT,
	SLSTATE_RESPONSE_SENT,
};

struct sllin {
	int			magic;

	/* Various fields. */
	struct tty_struct	*tty;		/* ptr to TTY structure	     */
	struct net_device	*dev;		/* easy for intr handling    */
	spinlock_t		lock;

	/* LIN message buffer and actual processed data counts */
	unsigned char		rx_buff[SLLIN_BUFF_LEN]; /* LIN Rx buffer */
	unsigned char		tx_buff[SLLIN_BUFF_LEN]; /* LIN Tx buffer */
	int			rx_expect;      /* expected number of Rx chars */
	int			rx_lim;         /* maximum Rx chars for ID  */
	int			rx_cnt;         /* message buffer Rx fill level  */
	int			tx_lim;         /* actual limit of bytes to Tx */
	int			tx_cnt;         /* number of already Tx bytes */
	char			lin_master;	/* node is a master node */
	int			lin_baud;	/* LIN baudrate */
	int 			lin_state;	/* state */
	int 			id_to_send;	/* there is ID to be sent */

	unsigned long		flags;		/* Flag values/ mode etc     */
#define SLF_INUSE		0		/* Channel in use            */
#define SLF_ERROR		1               /* Parity, etc. error        */
#define SLF_RXEVENT		2               /* Rx wake event             */
#define SLF_TXEVENT		3               /* Tx wake event             */
#define SLF_MSGEVENT		4               /* CAN message to sent       */

	dev_t			line;
	struct task_struct	*kwthread;
	wait_queue_head_t	kwt_wq;
};

static struct net_device **sllin_devs;

const unsigned char sllin_id_parity_table[] = {
        0x80,0xc0,0x40,0x00,0xc0,0x80,0x00,0x40,
        0x00,0x40,0xc0,0x80,0x40,0x00,0x80,0xc0,
        0x40,0x00,0x80,0xc0,0x00,0x40,0xc0,0x80,
        0xc0,0x80,0x00,0x40,0x80,0xc0,0x40,0x00,
        0x00,0x40,0xc0,0x80,0x40,0x00,0x80,0xc0,
        0x80,0xc0,0x40,0x00,0xc0,0x80,0x00,0x40,
        0xc0,0x80,0x00,0x40,0x80,0xc0,0x40,0x00,
        0x40,0x00,0x80,0xc0,0x00,0x40,0xc0,0x80
};

static int sltty_change_speed(struct tty_struct *tty, unsigned speed)
{
	struct ktermios old_termios;
	int cflag;

	mutex_lock(&tty->termios_mutex);

	old_termios = *(tty->termios);
	cflag = tty->termios->c_cflag;
	cflag &= ~(CBAUD | CIBAUD);
	cflag |= BOTHER;
	tty->termios->c_cflag = cflag;

	tty_encode_baud_rate(tty, speed, speed);

	if (tty->ops->set_termios)
		tty->ops->set_termios(tty, &old_termios);
	//priv->io.speed = speed;
	mutex_unlock(&tty->termios_mutex);

	return 0;
}


/* Send one completely decapsulated can_frame to the network layer */
static void sll_bump(struct sllin *sl)
{
//	struct sk_buff *skb;
//	struct can_frame cf;
//	int i, dlc_pos, tmp;
//	unsigned long ultmp;
//	char cmd = sl->rbuff[0];
//
//	if ((cmd != 't') && (cmd != 'T') && (cmd != 'r') && (cmd != 'R'))
//		return;
//
//	if (cmd & 0x20) /* tiny chars 'r' 't' => standard frame format */
//		dlc_pos = 4; /* dlc position tiiid */
//	else
//		dlc_pos = 9; /* dlc position Tiiiiiiiid */
//
//	if (!((sl->rbuff[dlc_pos] >= '0') && (sl->rbuff[dlc_pos] < '9')))
//		return;
//
//	cf.can_dlc = sl->rbuff[dlc_pos] - '0'; /* get can_dlc from ASCII val */
//
//	sl->rbuff[dlc_pos] = 0; /* terminate can_id string */
//
//	if (strict_strtoul(sl->rbuff+1, 16, &ultmp))
//		return;
//
//	cf.can_id = ultmp;
//
//	if (!(cmd & 0x20)) /* NO tiny chars => extended frame format */
//		cf.can_id |= CAN_EFF_FLAG;
//
//	if ((cmd | 0x20) == 'r') /* RTR frame */
//		cf.can_id |= CAN_RTR_FLAG;
//
//	*(u64 *) (&cf.data) = 0; /* clear payload */
//
//	for (i = 0, dlc_pos++; i < cf.can_dlc; i++) {
//
//		tmp = asc2nibble(sl->rbuff[dlc_pos++]);
//		if (tmp > 0x0F)
//			return;
//		cf.data[i] = (tmp << 4);
//		tmp = asc2nibble(sl->rbuff[dlc_pos++]);
//		if (tmp > 0x0F)
//			return;
//		cf.data[i] |= tmp;
//	}
//
//
//	skb = dev_alloc_skb(sizeof(struct can_frame));
//	if (!skb)
//		return;
//
//	skb->dev = sl->dev;
//	skb->protocol = htons(ETH_P_CAN);
//	skb->pkt_type = PACKET_BROADCAST;
//	skb->ip_summed = CHECKSUM_UNNECESSARY;
//	memcpy(skb_put(skb, sizeof(struct can_frame)),
//	       &cf, sizeof(struct can_frame));
//	netif_rx(skb);
//
//	sl->dev->stats.rx_packets++;
//	sl->dev->stats.rx_bytes += cf.can_dlc;
}


 /************************************************************************
  *			STANDARD SLLIN ENCAPSULATION			 *
  ************************************************************************/

/* Convert particular CAN frame into LIN frame and send it to TTY queue. */
static void sll_encaps(struct sllin *sl, struct can_frame *cf)
{
//	int actual, idx, i;
//	char lframe[16] = {0x00, 0x55}; /* Fake break, Sync byte */
//	struct tty_struct *tty = sl->tty;
//
//	pr_debug("sllin: %s() invoked\n", __FUNCTION__);
//
//	/* We do care only about SFF frames */
//	if (cf->can_id & CAN_EFF_FLAG)
//		return;
//
//	/* Send only header */
//	if (cf->can_id & CAN_RTR_FLAG) {
//		pr_debug("sllin: %s() RTR CAN frame\n", __FUNCTION__);
//		lframe[2] = (u8)cf->can_id; /* Get one byte LIN ID */
//
//		sltty_change_speed(tty, sl->lin_baud * 2 / 3);
//		tty->ops->write(tty, &lframe[0], 1);
//		sltty_change_speed(tty, sl->lin_baud);
//		tty->ops->write(tty, &lframe[1], 1);
//		tty->ops->write(tty, &lframe[2], 1);
//	} else {
//		pr_debug("sllin: %s() non-RTR CAN frame\n", __FUNCTION__);
//		/*	idx = strlen(sl->xbuff);
//
//			for (i = 0; i < cf->can_dlc; i++)
//			sprintf(&sl->xbuff[idx + 2*i], "%02X", cf->data[i]);
//
//		 * Order of next two lines is *very* important.
//		 * When we are sending a little amount of data,
//		 * the transfer may be completed inside the ops->write()
//		 * routine, because it's running with interrupts enabled.
//		 * In this case we *never* got WRITE_WAKEUP event,
//		 * if we did not request it before write operation.
//		 *       14 Oct 1994  Dmitry Gorodchanin.
//
//		 set_bit(TTY_DO_WRITE_WAKEUP, &sl->tty->flags);
//		 actual = sl->tty->ops->write(sl->tty, sl->xbuff, strlen(sl->xbuff));
//		 sl->xleft = strlen(sl->xbuff) - actual;
//		 sl->xhead = sl->xbuff + actual;
//		 sl->dev->stats.tx_bytes += cf->can_dlc;
//		 */
//	}
//
}

/*
 * Called by the driver when there's room for more data.  If we have
 * more packets to send, we send them here.
 */
static void sllin_write_wakeup(struct tty_struct *tty)
{
	int actual;
	int remains;
	struct sllin *sl = (struct sllin *) tty->disc_data;

	/* First make sure we're connected. */
	//if (!sl || sl->magic != SLLIN_MAGIC || !netif_running(sl->dev))
	//	return;

	if (sl->lin_state != SLSTATE_BREAK_SENT)
		remains = sl->tx_lim - sl->tx_cnt;
	else
		remains = SLLIN_BUFF_BREAK + 1 - sl->tx_cnt;

	if (remains > 0) {
		actual = tty->ops->write(tty, sl->tx_buff + sl->tx_cnt, sl->tx_cnt - sl->tx_lim);
		sl->tx_cnt += actual;

		if (sl->tx_cnt < sl->tx_lim) {
			printk(KERN_INFO "sllin_write_wakeup sent %d, remains %d, waiting\n",
				sl->tx_cnt, sl->tx_lim - sl->tx_cnt);
			return;
		}
	}

	clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
	set_bit(SLF_TXEVENT, &sl->flags);
	wake_up(&sl->kwt_wq);

	printk(KERN_INFO "sllin_write_wakeup sent %d, wakeup\n", sl->tx_cnt);
}

/* Send a can_frame to a TTY queue. */
static netdev_tx_t sll_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct sllin *sl = netdev_priv(dev);

	if (skb->len != sizeof(struct can_frame))
		goto out;

	spin_lock(&sl->lock);
	if (!netif_running(dev))  {
		spin_unlock(&sl->lock);
		printk(KERN_WARNING "%s: xmit: iface is down\n", dev->name);
		goto out;
	}
	if (sl->tty == NULL) {
		spin_unlock(&sl->lock);
		goto out;
	}

	netif_stop_queue(sl->dev);
	sll_encaps(sl, (struct can_frame *) skb->data); /* encaps & send */
	spin_unlock(&sl->lock);

out:
	kfree_skb(skb);
	return NETDEV_TX_OK;
}


/******************************************
 *   Routines looking at netdevice side.
 ******************************************/

/* Netdevice UP -> DOWN routine */
static int sll_close(struct net_device *dev)
{
	struct sllin *sl = netdev_priv(dev);

	spin_lock_bh(&sl->lock);
	if (sl->tty) {
		/* TTY discipline is running. */
		clear_bit(TTY_DO_WRITE_WAKEUP, &sl->tty->flags);
	}
	netif_stop_queue(dev);
	sl->rx_expect = 0;
	sl->tx_lim    = 0;
	spin_unlock_bh(&sl->lock);

	return 0;
}

/* Netdevice DOWN -> UP routine */
static int sll_open(struct net_device *dev)
{
	struct sllin *sl = netdev_priv(dev);

	pr_debug("sllin: %s() invoked\n", __FUNCTION__);

	if (sl->tty == NULL)
		return -ENODEV;

	sl->flags &= (1 << SLF_INUSE);
	netif_start_queue(dev);
	return 0;
}

/* Hook the destructor so we can free sllin devs at the right point in time */
static void sll_free_netdev(struct net_device *dev)
{
	int i = dev->base_addr;
	free_netdev(dev);
	sllin_devs[i] = NULL;
}

static const struct net_device_ops sll_netdev_ops = {
	.ndo_open               = sll_open,
	.ndo_stop               = sll_close,
	.ndo_start_xmit         = sll_xmit,
};

static void sll_setup(struct net_device *dev)
{
	dev->netdev_ops		= &sll_netdev_ops;
	dev->destructor		= sll_free_netdev;

	dev->hard_header_len	= 0;
	dev->addr_len		= 0;
	dev->tx_queue_len	= 10;

	dev->mtu		= sizeof(struct can_frame);
	dev->type		= ARPHRD_CAN;

	/* New-style flags. */
	dev->flags		= IFF_NOARP;
	dev->features           = NETIF_F_NO_CSUM;
}

/******************************************
  Routines looking at TTY side.
 ******************************************/

/*
 * Handle the 'receiver data ready' interrupt.
 * This function is called by the 'tty_io' module in the kernel when
 * a block of SLLIN data has been received, which can now be decapsulated
 * and sent on to some IP layer for further processing. This will not
 * be re-entered while running but other ldisc functions may be called
 * in parallel
 */

static void sllin_receive_buf(struct tty_struct *tty,
			      const unsigned char *cp, char *fp, int count)
{
	struct sllin *sl = (struct sllin *) tty->disc_data;

	printk(KERN_INFO "sllin_receive_buf invoked\n");

	//if (!sl || sl->magic != SLLIN_MAGIC || !netif_running(sl->dev))
	//	return;

	/* Read the characters out of the buffer */
	while (count--) {
		if (fp && *fp++) {
			if (!test_and_set_bit(SLF_ERROR, &sl->flags))
				sl->dev->stats.rx_errors++;
			printk(KERN_INFO "sllin_receive_buf char 0x%02x ignored due marker 0x%02x, flags 0x%lx\n",
				*cp, *(fp-1), sl->flags);
			cp++;
			continue;
		}

		if (sl->rx_cnt < SLLIN_BUFF_LEN)  {
			sl->rx_buff[sl->rx_cnt++] = *cp++;
		}
	}

	if(sl->rx_cnt >= sl->rx_expect) {
		set_bit(SLF_RXEVENT, &sl->flags);
		wake_up(&sl->kwt_wq);
		printk(KERN_INFO "sllin_receive_buf count %d, wakeup\n", sl->rx_cnt);
	} else {
		printk(KERN_INFO "sllin_receive_buf count %d, waiting\n", sl->rx_cnt);
	}
}

/*****************************************
 *  sllin message helper routines
 *****************************************/

int sllin_setup_msg(struct sllin *sl, int mode, int id,
		unsigned char *data, int len)
{
	if (id > 0x3f)
		return -1;

	sl->rx_cnt = 0;
	sl->tx_cnt = 0;
	sl->rx_expect = 0;

	sl->tx_buff[SLLIN_BUFF_BREAK] = 0;
	sl->tx_buff[SLLIN_BUFF_SYNC]  = 0x55;
	sl->tx_buff[SLLIN_BUFF_ID]    = id | sllin_id_parity_table[id];
	sl->tx_lim = SLLIN_BUFF_DATA;

	if ((data != NULL) && len) {
		int i;
		unsigned csum  = 0;

		sl->tx_lim += len;
		memcpy(sl->tx_buff + SLLIN_BUFF_DATA, data, len);
		/* compute data parity there */
		for (i = SLLIN_BUFF_DATA; i < sl->tx_lim; i++) {
			csum += sl->tx_buff[i];
			if (csum > 255)
				csum -= 255;
		}

		sl->tx_buff[sl->tx_lim++] = csum;
	}
	if (len != 0)
		sl->rx_lim += len + 1;

	return 0;
}


int sllin_send_tx_buff(struct sllin *sl)
{
	struct tty_struct *tty = sl->tty;
	int remains;
	int res;

	if (sl->lin_state != SLSTATE_BREAK_SENT)
		remains = sl->tx_lim - sl->tx_cnt;
	else
		remains = 1;


	res = tty->ops->write(tty, sl->tx_buff + sl->tx_cnt, remains);
	if (res < 0)
		return -1;

	remains -= res;
	sl->tx_cnt += res;

	if (remains > 0) {
		set_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
		res = tty->ops->write(tty, sl->tx_buff + sl->tx_cnt, remains);
		if (res < 0) {
			clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
			return -1;
		}
		
		remains -= res;
		sl->tx_cnt += res;
	}

	printk(KERN_INFO "sllin_send_tx_buff sent %d, remains %d\n",
			sl->tx_cnt, remains);

	return 0;
}

int sllin_send_break(struct sllin *sl)
{
	struct tty_struct *tty = sl->tty;
	unsigned long break_baud = sl->lin_baud;
	int res;

	//break_baud = (break_baud * 8) / 14;
	break_baud /= 2;

	sltty_change_speed(tty, break_baud);

	sl->rx_expect = SLLIN_BUFF_BREAK + 1;

	sl->lin_state = SLSTATE_BREAK_SENT;

	res = sllin_send_tx_buff(sl);
	if (res < 0) {
		sl->lin_state = SLSTATE_IDLE;
		return res;
	}

	return 0;
}

/*****************************************
 *  sllin_kwthread - kernel worker thread
 *****************************************/

int sllin_kwthread(void *ptr)
{
	struct sllin *sl = (struct sllin *)ptr;
	struct tty_struct *tty = sl->tty;
	int res;

	printk(KERN_INFO "sllin: sllin_kwthread started.\n");

	clear_bit(SLF_ERROR, &sl->flags);

	sltty_change_speed(tty, sl->lin_baud);

	sllin_setup_msg(sl, 0, 0x33, NULL, 0);
	sl->id_to_send = 1;

	while (!kthread_should_stop()) {

		if ((sl->lin_state == SLSTATE_IDLE) && sl->lin_master &&
			sl->id_to_send) {
			if(sllin_send_break(sl)<0) {
				/* error processing */
			}

		}

		wait_event_killable(sl->kwt_wq, kthread_should_stop() ||
			test_bit(SLF_RXEVENT, &sl->flags) ||
			test_bit(SLF_TXEVENT, &sl->flags));

		if (test_and_clear_bit(SLF_RXEVENT, &sl->flags)) {
			printk(KERN_INFO "sllin_kthread RXEVENT \n");
		}

		if (test_and_clear_bit(SLF_TXEVENT, &sl->flags)) {
			printk(KERN_INFO "sllin_kthread TXEVENT \n");
		}

		switch (sl->lin_state) {
			case SLSTATE_BREAK_SENT:
				if (sl->rx_cnt <= SLLIN_BUFF_BREAK)
					continue;

				res = sltty_change_speed(tty, sl->lin_baud);

				sllin_send_tx_buff(sl);

				sl->lin_state = SLSTATE_ID_SENT;

				break;
			case SLSTATE_ID_SENT:
				sl->id_to_send = 0;
				sl->lin_state = SLSTATE_IDLE;
				break;
		}



		/* sll_bump(sl); send packet to the network layer */

		/* sl->dev->stats.tx_packets++; send frames statistic */
		/* netif_wake_queue(sl->dev); allow next Tx packet arrival */
	}


	printk(KERN_INFO "sllin: sllin_kwthread stopped.\n");

	return 0;
}


/************************************
 *  sllin_open helper routines.
 ************************************/

/* Collect hanged up channels */
static void sll_sync(void)
{
	int i;
	struct net_device *dev;
	struct sllin	  *sl;

	for (i = 0; i < maxdev; i++) {
		dev = sllin_devs[i];
		if (dev == NULL)
			break;

		sl = netdev_priv(dev);
		if (sl->tty)
			continue;
		if (dev->flags & IFF_UP)
			dev_close(dev);
	}
}

/* Find a free SLLIN channel, and link in this `tty' line. */
static struct sllin *sll_alloc(dev_t line)
{
	int i;
	struct net_device *dev = NULL;
	struct sllin       *sl;

	if (sllin_devs == NULL)
		return NULL;	/* Master array missing ! */

	for (i = 0; i < maxdev; i++) {
		dev = sllin_devs[i];
		if (dev == NULL)
			break;

	}

	/* Sorry, too many, all slots in use */
	if (i >= maxdev)
		return NULL;

	if (dev) {
		sl = netdev_priv(dev);
		if (test_bit(SLF_INUSE, &sl->flags)) {
			unregister_netdevice(dev);
			dev = NULL;
			sllin_devs[i] = NULL;
		}
	}

	if (!dev) {
		char name[IFNAMSIZ];
		sprintf(name, "sllin%d", i);

		dev = alloc_netdev(sizeof(*sl), name, sll_setup);
		if (!dev)
			return NULL;
		dev->base_addr  = i;
	}

	sl = netdev_priv(dev);

	/* Initialize channel control data */
	sl->magic = SLLIN_MAGIC;
	sl->dev	= dev;
	spin_lock_init(&sl->lock);
	sllin_devs[i] = dev;

	return sl;
}

/*
 * Open the high-level part of the SLLIN channel.
 * This function is called by the TTY module when the
 * SLLIN line discipline is called for.  Because we are
 * sure the tty line exists, we only have to link it to
 * a free SLLIN channel...
 *
 * Called in process context serialized from other ldisc calls.
 */

static int sllin_open(struct tty_struct *tty)
{
	struct sllin *sl;
	int err;
	pr_debug("sllin: %s() invoked\n", __FUNCTION__);

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (tty->ops->write == NULL)
		return -EOPNOTSUPP;

	/* RTnetlink lock is misused here to serialize concurrent
	   opens of sllin channels. There are better ways, but it is
	   the simplest one.
	 */
	rtnl_lock();

	/* Collect hanged up channels. */
	sll_sync();

	sl = tty->disc_data;

	err = -EEXIST;
	/* First make sure we're not already connected. */
	if (sl && sl->magic == SLLIN_MAGIC)
		goto err_exit;

	/* OK.  Find a free SLLIN channel to use. */
	err = -ENFILE;
	sl = sll_alloc(tty_devnum(tty));
	if (sl == NULL)
		goto err_exit;

	sl->tty = tty;
	tty->disc_data = sl;
	sl->line = tty_devnum(tty);

	if (!test_bit(SLF_INUSE, &sl->flags)) {
		/* Perform the low-level SLLIN initialization. */
		sl->rx_cnt    = 0;
		sl->rx_expect = 0;
		sl->tx_cnt    = 0;
		sl->tx_lim    = 0;

		sl->lin_baud  = 2400;

		sl->lin_master = 1;
		sl->lin_state = SLSTATE_IDLE;

		set_bit(SLF_INUSE, &sl->flags);

		init_waitqueue_head(&sl->kwt_wq);
		sl->kwthread = kthread_run(sllin_kwthread, sl, "sllin");
		if (sl->kwthread == NULL)
			goto err_free_chan;

		err = register_netdevice(sl->dev);
		if (err)
			goto err_free_chan_and_thread;
	}

	/* Done.  We have linked the TTY line to a channel. */
	rtnl_unlock();
	tty->receive_room = SLLIN_BUFF_LEN * 40;	/* We don't flow control */

	/* TTY layer expects 0 on success */
	return 0;

err_free_chan_and_thread:
	kthread_stop(sl->kwthread);
	sl->kwthread = NULL;

err_free_chan:
	sl->tty = NULL;
	tty->disc_data = NULL;
	clear_bit(SLF_INUSE, &sl->flags);

err_exit:
	rtnl_unlock();

	/* Count references from TTY module */
	return err;
}

/*
 * Close down a SLLIN channel.
 * This means flushing out any pending queues, and then returning. This
 * call is serialized against other ldisc functions.
 *
 * We also use this method for a hangup event.
 */

static void sllin_close(struct tty_struct *tty)
{
	struct sllin *sl = (struct sllin *) tty->disc_data;

	/* First make sure we're connected. */
	if (!sl || sl->magic != SLLIN_MAGIC || sl->tty != tty)
		return;

	kthread_stop(sl->kwthread);
	sl->kwthread = NULL;

	tty->disc_data = NULL;
	sl->tty = NULL;

	/* Flush network side */
	unregister_netdev(sl->dev);
	/* This will complete via sl_free_netdev */
}

static int sllin_hangup(struct tty_struct *tty)
{
	sllin_close(tty);
	return 0;
}

/* Perform I/O control on an active SLLIN channel. */
static int sllin_ioctl(struct tty_struct *tty, struct file *file,
		       unsigned int cmd, unsigned long arg)
{
	struct sllin *sl = (struct sllin *) tty->disc_data;
	unsigned int tmp;

	/* First make sure we're connected. */
	if (!sl || sl->magic != SLLIN_MAGIC)
		return -EINVAL;

	switch (cmd) {
	case SIOCGIFNAME:
		tmp = strlen(sl->dev->name) + 1;
		if (copy_to_user((void __user *)arg, sl->dev->name, tmp))
			return -EFAULT;
		return 0;

	case SIOCSIFHWADDR:
		return -EINVAL;

	default:
		return tty_mode_ioctl(tty, file, cmd, arg);
	}
}

static struct tty_ldisc_ops sll_ldisc = {
	.owner		= THIS_MODULE,
	.magic		= TTY_LDISC_MAGIC,
	.name		= "sllin",
	.open		= sllin_open,
	.close		= sllin_close,
	.hangup		= sllin_hangup,
	.ioctl		= sllin_ioctl,
	.receive_buf	= sllin_receive_buf,
	.write_wakeup	= sllin_write_wakeup,
};

static int __init sllin_init(void)
{
	int status;

	if (maxdev < 4)
		maxdev = 4; /* Sanity */

	printk(banner);
	printk(KERN_INFO "sllin: %d dynamic interface channels.\n", maxdev);

	sllin_devs = kzalloc(sizeof(struct net_device *)*maxdev, GFP_KERNEL);
	if (!sllin_devs) {
		printk(KERN_ERR "sllin: can't allocate sllin device array!\n");
		return -ENOMEM;
	}

	/* Fill in our line protocol discipline, and register it */
	status = tty_register_ldisc(N_SLLIN, &sll_ldisc);
	if (status)  {
		printk(KERN_ERR "sllin: can't register line discipline\n");
		kfree(sllin_devs);
	}
	return status;
}

static void __exit sllin_exit(void)
{
	int i;
	struct net_device *dev;
	struct sllin *sl;
	unsigned long timeout = jiffies + HZ;
	int busy = 0;

	if (sllin_devs == NULL)
		return;

	/* First of all: check for active disciplines and hangup them.
	 */
	do {
		if (busy)
			msleep_interruptible(100);

		busy = 0;
		for (i = 0; i < maxdev; i++) {
			dev = sllin_devs[i];
			if (!dev)
				continue;
			sl = netdev_priv(dev);
			spin_lock_bh(&sl->lock);
			if (sl->tty) {
				busy++;
				tty_hangup(sl->tty);
			}
			spin_unlock_bh(&sl->lock);
		}
	} while (busy && time_before(jiffies, timeout));

	/* FIXME: hangup is async so we should wait when doing this second
	   phase */

	for (i = 0; i < maxdev; i++) {
		dev = sllin_devs[i];
		if (!dev)
			continue;
		sllin_devs[i] = NULL;

		sl = netdev_priv(dev);
		if (sl->tty) {
			printk(KERN_ERR "%s: tty discipline still running\n",
			       dev->name);
			/* Intentionally leak the control block. */
			dev->destructor = NULL;
		}

		unregister_netdev(dev);
	}

	kfree(sllin_devs);
	sllin_devs = NULL;

	i = tty_unregister_ldisc(N_SLLIN);
	if (i)
		printk(KERN_ERR "sllin: can't unregister ldisc (err %d)\n", i);
}

module_init(sllin_init);
module_exit(sllin_exit);