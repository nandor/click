#ifndef CLICK_FLOOD_HH
#define CLICK_FLOOD_HH
#include <click/element.hh>
#include <click/glue.hh>
#include <click/timer.hh>
#include <click/ipaddress.hh>
#include <click/etheraddress.hh>
#include <click/vector.hh>
#include <click/hashmap.hh>
#include <click/dequeue.hh>
#include <elements/grid/linktable.hh>
#include <elements/grid/arptable.hh>
#include <elements/grid/sr/path.hh>
#include "flood.hh"
#include <elements/wifi/rxstats.hh>
CLICK_DECLS

/*
 * =c
 * Flood(IP, ETH, ETHERTYPE, Flood element, LinkTable element, ARPtable element, 
 *    [METRIC GridGenericMetric], [WARMUP period in seconds])
 * =d
 * DSR-inspired end-to-end ad-hoc routing protocol.
 * Input 0: ethernet packets 
 * Input 1: ethernet data packets from device 
 * Input 2: IP packets from higher layer, w/ ip addr anno.
 * Input 3: IP packets from higher layer for gw, w/ ip addr anno.
 * Output 0: ethernet packets to device (protocol)
 * Output 1: ethernet packets to device (data)
 *
 */


class Flood : public Element {
 public:
  
  Flood();
  ~Flood();
  
  const char *class_name() const		{ return "Flood"; }
  const char *processing() const		{ return PUSH; }
  int initialize(ErrorHandler *);
  Flood *clone() const;
  int configure(Vector<String> &conf, ErrorHandler *errh);


  static String static_print_debug(Element *f, void *);
  static int static_write_debug(const String &arg, Element *e,
				void *, ErrorHandler *errh); 

  static String static_print_stats(Element *e, void *);
  String print_stats();

  void push(int, Packet *);
  void run_timer();

  void add_handlers();
private:

  void send(WritablePacket *p);
  void flood_assert_(const char *file, int line, const char *expr) const;
  u_long _seq;      // Next query sequence number to use.
  Timer _timer;
  IPAddress _ip;    // My IP address.
  EtherAddress _en; // My ethernet address.
  uint32_t _et;     // This protocol's ethertype
  IPAddress _bcast_ip;

  EtherAddress _bcast;

  bool _debug;


  int _packets_originated;
  int _packets_tx;
  int _packets_rx;
};


CLICK_ENDDECLS
#endif
