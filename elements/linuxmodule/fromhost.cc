// -*- mode: c++; c-basic-offset: 4 -*-
/*
 * fromhost.{cc,hh} -- receives packets from Linux
 * Max Poletto, Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 * Copyright (c) 2001-2003 International Computer Science Institute
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/router.hh>
#include "fromhost.hh"
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/standard/scheduleinfo.hh>
#include <clicknet/ip6.h>

#include <click/cxxprotect.h>
CLICK_CXX_PROTECT
#include <asm/types.h>
#include <asm/uaccess.h>
#include <linux/ip.h>
#include <linux/inetdevice.h>
#include <linux/if_arp.h>
#include <net/route.h>
CLICK_CXX_UNPROTECT
#include <click/cxxunprotect.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 0)
# define netif_start_queue(dev)	do { dev->start=1; dev->tbusy=0; } while (0)
# define netif_stop_queue(dev)	do { dev->tbusy=1; } while (0)
# define netif_wake_queue(dev)	do { dev->tbusy=0; } while (0)
#endif

static int fl_open(net_device *);
static int fl_close(net_device *);
static net_device_stats *fl_stats(net_device *);
static void fl_wakeup(Timer *, void *);

static int from_linux_count;
static AnyDeviceMap fromlinux_map;

void
FromHost::static_initialize()
{
    fromlinux_map.initialize();
}

FromHost::FromHost()
    : _macaddr((const unsigned char *)"\000\001\002\003\004\005"),
      _task(this), _wakeup_timer(fl_wakeup, this), _queue(0)
{
    memset(&_stats, 0, sizeof(_stats));
}

FromHost::~FromHost()
{
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
extern "C" {
static void fromhost_inet_setup(struct net_device *dev)
{
    dev->type = ARPHRD_NONE;
    dev->hard_header_len = 0;
    dev->addr_len = 0;
    dev->mtu = 1500;
    dev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
}
}
#endif

net_device *
FromHost::new_device(const char *name)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 0)
    read_lock(&dev_base_lock);
#endif
    void (*setup)(struct net_device *) = (_macaddr ? ether_setup : fromhost_inet_setup);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
    net_device *dev = alloc_netdev(0, name, setup);
#else
    int errcode;
    net_device *dev = dev_alloc(name, &errcode);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 0)
    read_unlock(&dev_base_lock);
#endif
    if (!dev)
	return 0;
    
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 0)
    // need to zero out the dev structure
    char *nameptr = dev->name;
    memset(dev, 0, sizeof(*dev));
    dev->name = nameptr;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
    setup(dev);
#endif
    dev->open = fl_open;
    dev->stop = fl_close;
    dev->hard_start_xmit = fl_tx;
    dev->get_stats = fl_stats;
    return dev;
}

int
FromHost::configure(Vector<String> &conf, ErrorHandler *errh)
{
    String type;
    if (cp_va_kparse(conf, this, errh,
		     "DEVNAME", cpkP+cpkM, cpString, &_devname,
		     "PREFIX", cpkP+cpkM, cpIPPrefix, &_destaddr, &_destmask,
		     "TYPE", 0, cpWord, &type,
		     "ETHER", 0, cpEthernetAddress, &_macaddr,
		     cpEnd) < 0)
	return -1;
    
    // check for duplicate element
    if (_devname.length() > IFNAMSIZ - 1)
	return errh->error("device name '%s' too long", _devname.c_str());
    void *&used = router()->force_attachment("FromHost_" + _devname);
    if (used)
	return errh->error("duplicate FromHost for device '%s'", _devname.c_str());
    used = this;
    
    // check for existing device
    _dev = dev_get_by_name(_devname.c_str());
    if (_dev) {
	if (_dev->open != fl_open) {
	    dev_put(_dev);
	    _dev = 0;
	    return errh->error("device '%s' already exists", _devname.c_str());
	} else {
	    fromlinux_map.insert(this, false);
	    return 0;
	}
    }

    // set type
    if (type == "IP")
	_macaddr = EtherAddress();
    else if (type != "ETHER" && type != "")
	return errh->error("bad TYPE");
    
    // if not found, create new device
    int res;
    _dev = new_device(_devname.c_str());
    if (!_dev)
	return errh->error("out of memory! registering device '%s'", _devname.c_str());
    else if ((res = register_netdev(_dev)) < 0) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
	free_netdev(_dev);
#else
	kfree(_dev);
#endif
	_dev = 0;
	return errh->error("error %d registering device '%s'", res, _devname.c_str());
    }

    dev_hold(_dev);
    fromlinux_map.insert(this, false);
    return 0;
}

#if 0 /* Why was this code here? */
static void
dev_locks(int up)
{
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 0)
    if (up > 0)
	rtnl_lock();
    else
	rtnl_unlock();
# endif
}
#endif

int
FromHost::set_device_addresses(ErrorHandler *errh)
{
    int res = 0;
    struct ifreq ifr;
    strncpy(ifr.ifr_name, _dev->name, IFNAMSIZ);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;

    mm_segment_t oldfs = get_fs();
    set_fs(get_ds());

    if (_macaddr) {
	ifr.ifr_hwaddr.sa_family = _dev->type;
	memcpy(ifr.ifr_hwaddr.sa_data, _macaddr.data(), 6);
	if ((res = dev_ioctl(SIOCSIFHWADDR, &ifr)) < 0)
	    errh->error("error %d setting hardware address for device '%s'", res, _devname.c_str());
    }

    sin->sin_family = AF_INET;
    sin->sin_addr = _destaddr;
    if (res >= 0 && (res = devinet_ioctl(SIOCSIFADDR, &ifr)) < 0)
	errh->error("error %d setting address for device '%s'", res, _devname.c_str());

    sin->sin_addr = _destmask;
    if (res >= 0 && (res = devinet_ioctl(SIOCSIFNETMASK, &ifr)) < 0)
	errh->error("error %d setting netmask for device '%s'", res, _devname.c_str());

    set_fs(oldfs);
    return res;
}

static int
dev_updown(net_device *dev, int up, ErrorHandler *errh)
{
    struct ifreq ifr;
    strncpy(ifr.ifr_name, dev->name, IFNAMSIZ);
    uint32_t flags = IFF_UP | IFF_RUNNING;
    int res;

    mm_segment_t oldfs = get_fs();
    set_fs(get_ds());

    (void) dev_ioctl(SIOCGIFFLAGS, &ifr);
    ifr.ifr_flags = (up > 0 ? ifr.ifr_flags | flags : ifr.ifr_flags & ~flags);
    if ((res = dev_ioctl(SIOCSIFFLAGS, &ifr)) < 0 && errh)
	errh->error("error %d bringing %s device '%s'", res, (up > 0 ? "up" : "down"), dev->name);

    set_fs(oldfs);
    return res;
}

int
FromHost::initialize(ErrorHandler *errh)
{
    ScheduleInfo::initialize_task(this, &_task, _dev != 0, errh);
    _nonfull_signal = Notifier::downstream_full_signal(this, 0, &_task);
    if (_dev->flags & IFF_UP) {
	_wakeup_timer.initialize(this);
	_wakeup_timer.schedule_now();
	return 0;
    } else if (set_device_addresses(errh) < 0)
	return -1;
    else
	return dev_updown(_dev, 1, errh);
}

void
FromHost::cleanup(CleanupStage)
{
    fromlinux_map.remove(this, false);

    if (_queue) {
	_queue->kill();
	_queue = 0;
    }
    
    if (_dev) {
	dev_put(_dev);
	fromlinux_map.lock(false);
	if (fromlinux_map.lookup(_dev, 0))
	    // do not free device if it is in use
	    _dev = 0;
	fromlinux_map.unlock(false);
	if (_dev) {
	    if (_dev->flags & IFF_UP)
		dev_updown(_dev, -1, 0);
	    unregister_netdev(_dev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
	    free_netdev(_dev);
#else
	    kfree(_dev);
#endif
	    _dev = 0;
	}
    }
}

static void
fl_wakeup(Timer *, void *thunk)
{
    FromHost *fl = (FromHost *)thunk;
    PrefixErrorHandler errh(ErrorHandler::default_handler(), fl->declaration() + ": ");
    net_device *dev = fl->device();

    if (dev->flags & IFF_UP)
	dev_updown(dev, -1, &errh);
    
    fl->set_device_addresses(&errh);
    
    dev_updown(dev, 1, &errh);
}

/*
 * Device callbacks
 */

static int
fl_open(net_device *dev)
{
    netif_start_queue(dev);
    return 0;
}

static int
fl_close(net_device *dev)
{
    netif_stop_queue(dev);
    return 0;
}

int
FromHost::fl_tx(struct sk_buff *skb, net_device *dev)
{
    /* 8.May.2003 - Doug and company had crashes with FromHost configurations.
         We eventually figured out this was because fl_tx was called at
         interrupt time -- at bottom-half time, to be exact -- and then pushed
         a packet through the configuration. Whoops: if Click was interrupted,
         and during the bottom-half FromHost emitted a packet into Click,
         DISASTER -- we assume that, when running single-threaded, at most one
         Click thread is active at a time; so there were race conditions,
         particularly with the task list. The solution is a single-packet-long
         queue in FromHost. fl_tx puts a packet onto the queue, a regular
         Click Task takes the packet off the queue. We could have implemented
         a larger queue, but why bother? Linux already maintains a queue for
         the device. */
    fromlinux_map.lock(false);
    if (FromHost *fl = (FromHost *)fromlinux_map.lookup(dev, 0))
	if (!fl->_queue) {
	    fl->_queue = Packet::make(skb);
	    fl->_stats.tx_packets++;
	    fl->_stats.tx_bytes += fl->_queue->length();
	    fl->_task.reschedule();
	    fromlinux_map.unlock(false);
	    netif_stop_queue(dev);
    	    return 0;
	}
    fromlinux_map.unlock(false);
    return -1;
}

static net_device_stats *
fl_stats(net_device *dev)
{
    net_device_stats *stats = 0;
    fromlinux_map.lock(false);
    if (FromHost *fl = (FromHost *)fromlinux_map.lookup(dev, 0))
	stats = fl->stats();
    fromlinux_map.unlock(false);
    return stats;
}

bool
FromHost::run_task(Task *)
{
    if (!_nonfull_signal)
	return false;
    else if (Packet *p = _queue) {
	_queue = 0;
	netif_wake_queue(_dev);

	// Convenience for TYPE IP: set the IP header and destination address.
	if (_dev->type == ARPHRD_NONE && p->length() >= 1) {
	    const click_ip *iph = (const click_ip *) p->data();
	    if (iph->ip_v == 4) {
		if (iph->ip_hl >= 5
		    && reinterpret_cast<const uint8_t *>(iph) + (iph->ip_hl << 2) <= p->end_data()) {
		    p->set_ip_header(iph, iph->ip_hl << 2);
		    p->set_dst_ip_anno(iph->ip_dst);
		} else
		    goto bad;
	    } else if (iph->ip_v == 6) {
		if (reinterpret_cast<const uint8_t *>(iph) + sizeof(click_ip6) <= p->end_data())
		    p->set_ip6_header(reinterpret_cast<const click_ip6 *>(iph));
		else
		    goto bad;
	    } else
		goto bad;
	}

	output(0).push(p);
	return true;

      bad:
	checked_output_push(1, p);
	return true;
    } else
	return false;
}

ELEMENT_REQUIRES(AnyDevice linuxmodule)
EXPORT_ELEMENT(FromHost)
