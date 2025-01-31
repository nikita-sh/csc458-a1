/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
        uint8_t * packet/* lent */,
        unsigned int len,
        char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("*** -> Received packet of length %d \n",len);
  
  printf("========Printing packet headers:==========\n");
  print_hdr_eth(packet);

  if (check_eth_packet(packet, len)) {

    if (ethertype(packet) == ethertype_arp) {
      print_hdr_arp(packet + sizeof(sr_ethernet_hdr_t));
      printf("Received ARP packet.\n");
      handle_arp(sr, packet, interface, len);

    } else if (ethertype(packet) == ethertype_ip) {
      print_hdr_ip(packet + sizeof(sr_ethernet_hdr_t));
      printf("Received IP packet.\n");
      handle_ip(sr, packet, len, interface);

    } else {

      printf("Unknown packet received. Dropping.\n");
      return;

    }
  } else {

    printf("Packet invalid.\n");

  }

}/* end sr_ForwardPacket */

/* ----------------------------------------------- */
/* Helpers for sr_handlepacket() and handle_arp(). */
int check_eth_packet(uint8_t *packet, unsigned int len) {
  if (len < sizeof(sr_ethernet_hdr_t)) {
    return 0;
  }
  return 1;
}

int check_arp_packet(uint8_t *pkt, unsigned int len) {
  return 1;
}
/* ----------------------------------------------- */

/*--------------------------------------------------------------------- 
 * handle_arp
 *
 * Given either an ARP request or ARP reply, handle the packet appropriately.
 * If ARP opcode is unrecognized, drop the packet.
 *---------------------------------------------------------------------*/
void handle_arp(struct sr_instance *sr, uint8_t *pkt, char *interface, unsigned int len) {
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)(pkt);
  sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(pkt + sizeof(sr_ethernet_hdr_t));

  if (!check_arp_packet(pkt, len)) {
    /* Send error? */
    return;
  }

  struct sr_if *my_if = sr_get_interface_by_IP(sr, arp_hdr->ar_tip);

  if (my_if) {
    if (htons(arp_hdr->ar_op) == arp_op_request) {
      printf("Received ARP request.\n");
      uint8_t *ret_pkt = malloc(len);
      memcpy(ret_pkt, pkt, len);

      struct sr_if *in_if = sr_get_interface(sr, interface);

      sr_ethernet_hdr_t *ret_eth_hdr = (sr_ethernet_hdr_t *)(ret_pkt);
      sr_arp_hdr_t *ret_arp_hdr = (sr_arp_hdr_t *)(ret_pkt + sizeof(sr_ethernet_hdr_t));

      memcpy(ret_eth_hdr->ether_dhost, eth_hdr->ether_shost, sizeof(uint8_t) * ETHER_ADDR_LEN);
      memcpy(ret_eth_hdr->ether_shost, in_if->addr, sizeof(uint8_t) * ETHER_ADDR_LEN);
      ret_eth_hdr->ether_type = ntohs(ethertype_arp);

      ret_arp_hdr->ar_op = ntohs(arp_op_reply);
      memcpy(ret_arp_hdr->ar_sha, my_if->addr, sizeof(uint8_t) * ETHER_ADDR_LEN);
      ret_arp_hdr->ar_sip = my_if->ip;
      memcpy(ret_arp_hdr->ar_tha, arp_hdr->ar_sha, sizeof(uint8_t) * ETHER_ADDR_LEN);
      ret_arp_hdr->ar_tip = arp_hdr->ar_sip;

      sr_send_packet(sr, ret_pkt, len, interface);
      free(ret_pkt);

    } else if (htons(arp_hdr->ar_op) == arp_op_reply) {
      printf("Received ARP reply.\n");
      struct sr_arpreq *req = sr_arpcache_insert(&sr->cache, arp_hdr->ar_sha, arp_hdr->ar_sip);

      if (req) {
        struct sr_packet *walker = req->packets;

        while (walker) {
          sr_ethernet_hdr_t *w_eth = (sr_ethernet_hdr_t *)(walker->buf);
          memcpy(w_eth, arp_hdr->ar_sha, sizeof(uint8_t) * ETHER_ADDR_LEN);

          sr_send_packet(sr, walker->buf, walker->len, walker->iface);
          walker = walker->next;
        }
        sr_arpreq_destroy(&sr->cache, req);
      }
    } else {
      printf("Unrecognized ARP Opcode. Dropping.\n");
      return;
    }
  }
}

/*--------------------------------------------------------------------- 
 * check_ip_packet
 *
 * Check length and checksum of an IP packet. Return 1 if valid, 0 if not.
 *---------------------------------------------------------------------*/
int check_ip_packet(uint8_t *pkt, unsigned int len) {
  if (len < sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)) {
    return 0;
  }

  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(pkt + sizeof(sr_ethernet_hdr_t));

  uint16_t old_cksm = ip_hdr->ip_sum;
  ip_hdr->ip_sum = 0;
  if (old_cksm != cksum(ip_hdr, sizeof(sr_ip_hdr_t))) {
    return 0;
  }
  ip_hdr->ip_sum = old_cksm;

  return 1;
}

/*--------------------------------------------------------------------- 
 * check_icmp_packet
 *
 * Check length and checksum of an ICMP packet. Return 1 if valid, 0 if not.
 *---------------------------------------------------------------------*/
int check_icmp_packet(uint8_t *pkt, int len) {
  if (len < sizeof(sr_icmp_hdr_t)) {
    return  0;
  }

  sr_icmp_hdr_t *icmp_hdr = (sr_icmp_hdr_t *)(pkt + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
  uint16_t old_cksm = icmp_hdr->icmp_sum;
  icmp_hdr->icmp_sum = 0;
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(pkt + sizeof(sr_ethernet_hdr_t));
  uint16_t new_cksm = cksum(icmp_hdr, ntohs(ip_hdr->ip_len) - sizeof(sr_ip_hdr_t));

  if (old_cksm != new_cksm) {
    icmp_hdr->icmp_sum = old_cksm;
    return 0;
  }
  icmp_hdr->icmp_sum = old_cksm;

  return 1;
}

/*--------------------------------------------------------------------- 
 * handle_ip
 *
 * Checks incoming packet. Reply to echo requests, send Port Unreachable
 * error for TCP/UDP, or forward the packet. Drop others.
 *---------------------------------------------------------------------*/
void handle_ip(struct sr_instance *sr, uint8_t *pkt, unsigned int len, char *interface) {
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(pkt + sizeof(sr_ethernet_hdr_t));
  
  if (!check_ip_packet(pkt, len)) {
    printf("Packet is not valid. Dropping.\n");
    return;
  }

  struct sr_if *my_int = sr_get_interface_by_IP(sr, ip_hdr->ip_dst);

  if (my_int) {
    if (ip_hdr->ip_p == 1) {
      printf("Received ICMP packet.\n");

      /* ICMP */
      sr_icmp_hdr_t *icmp_hdr = (sr_icmp_hdr_t *)(pkt + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
      if (icmp_hdr->icmp_type != 8 || !check_icmp_packet(pkt, len)) {
        /* Unsupported type, drop */
        printf("Dropping\n");
        printf("ICMP type: %d\n\n", icmp_hdr->icmp_type);
        return ;
      }
      printf("Sending ICMP echo reply.\n");
      send_icmp_echo_reply(sr, pkt, interface, len);
    } else if (ip_hdr->ip_p == 0x0006 || ip_hdr->ip_p == 0x0011) {
      /* TCP/UDP */
      printf("Received TCP/UDP packet, sending ICMP error (code: 3, type 3).\n");
      send_icmp3_error(3, 3, sr, pkt, interface);
      return;
    } else {
      /* Unsupported Protocol */
      printf("Received unsupported protocol, dropping.\n");
      return ;
    }
  } else {
    /* Destined elsewhere, forward */
    printf("Packet destined elsewhere, forwarding.\n");
    forward_ip(sr, pkt, len, interface);
  }
}

/*--------------------------------------------------------------------- 
 * forward_ip
 *
 * Given an IP packet, check the checksum decrement the TTL, and send an
 * ICMP time exceeded message if it is zero. Otherwise, find the longest
 * prefix match and forward the packet to that address.
 * 
 * Check the ARP cache for a match, send if found, or queue if not.
 * 
 * If no longest prefix match was found, send an ICMP Network Unreachable
 * message.
 *---------------------------------------------------------------------*/
void forward_ip(struct sr_instance *sr, uint8_t *pkt, unsigned int len, char *interface) {
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)(pkt);
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(pkt + sizeof(sr_ethernet_hdr_t));

  if (!check_ip_packet(pkt, len)) {
    printf("Received invalid packet. Dropping.\n");
    return;
  }

  printf("Checking IP checksum.\n");
  ip_hdr->ip_ttl--;
  if (ip_hdr->ip_ttl <= 0) {
    /* send icmp time exceeded */
    send_icmp3_error(11, 0, sr, pkt, interface);
  }
  ip_hdr->ip_sum = 0;
  ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));

  /* Find longest prefix match */
  printf("Looking for LPM match\n");
  struct sr_rt *my_int = longest_prefix_match(sr, ip_hdr->ip_dst);
  printf("check\n");
  if (my_int) {
    /* Check ARP cache for next-hop MAC */
    printf("LPM match found, beginning initialization.\n");
    struct sr_if *my_if = sr_get_interface(sr, my_int->interface);
    sr_print_if(my_if);
    memcpy(eth_hdr->ether_shost, my_if->addr, sizeof(uint8_t) * ETHER_ADDR_LEN);
    struct sr_arpentry *arp_entry = sr_arpcache_lookup(&sr->cache, ip_hdr->ip_dst);
    if (arp_entry) {
      /* Match found, reconfigure Ethernet frame and forward. */
      memcpy(eth_hdr->ether_dhost, arp_entry->mac, sizeof(uint8_t) * ETHER_ADDR_LEN);

      sr_send_packet(sr, pkt, len, my_if->name);
    } else {
      /* Queue packet for ARP */
      printf("queueing packet:\n");
      print_hdr_eth(pkt);
      struct sr_arpreq *req = sr_arpcache_queuereq(&sr->cache, ip_hdr->ip_dst, pkt, len, my_if->name);
      handle_arpreq(sr, req);
    }
  } else {
    /* Match not found, send icmp error */
    printf("LPM match not found, sending ICMP error (type 3, code 0).\n");
    send_icmp3_error(3, 0, sr, pkt, interface);
  }
}

/*--------------------------------------------------------------------- 
 * send_icmp_echo_reply
 *
 * Prepares the complete ICMP, IP and ethernet headers to send an
 * ICMP type 0 echo reply message.
 * 
 * Check the ARP cache for a match, send if found, or queue if not.
 *---------------------------------------------------------------------*/
void send_icmp_echo_reply(struct sr_instance *sr, uint8_t *pkt, char *interface, int len) {
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)(pkt);
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(pkt + sizeof(sr_ethernet_hdr_t));
  sr_icmp_hdr_t *icmp_hdr = (sr_icmp_hdr_t *)(pkt + sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t));

  struct sr_if *my_int = sr_get_interface(sr, interface);

  /* Prepare Ethernet Frame */
  memcpy(eth_hdr->ether_shost, my_int->addr, sizeof(uint8_t) * ETHER_ADDR_LEN);
  memset(eth_hdr->ether_dhost, 0, sizeof(uint8_t) * ETHER_ADDR_LEN);

  /* Prepare IP header */
  uint32_t temp = ip_hdr->ip_dst;
  ip_hdr->ip_dst = ip_hdr->ip_src;
  ip_hdr->ip_src = temp;
  ip_hdr->ip_sum = 0;
  ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));

  /* Prepare ICMP header */
  icmp_hdr->icmp_type = 0;
  icmp_hdr->icmp_code = 0;
  icmp_hdr->icmp_sum = 0;
  icmp_hdr->icmp_sum = cksum(icmp_hdr, ntohs(ip_hdr->ip_len) - sizeof(sr_ip_hdr_t)); /* cksum(icmp_hdr, sizeof(sr_icmp_hdr_t)); */

  print_hdr_eth(pkt);
  print_hdr_ip(pkt + sizeof(sr_ethernet_hdr_t));
  print_hdr_icmp(pkt + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));

  struct sr_arpentry *entry = sr_arpcache_lookup(&sr->cache, ip_hdr->ip_dst);
  if (entry) {
    memcpy(eth_hdr->ether_dhost, entry->mac, sizeof(uint8_t) * ETHER_ADDR_LEN);
    sr_send_packet(sr, pkt, len, interface);
  } else {
    struct sr_arpreq *req = sr_arpcache_queuereq(&sr->cache, ip_hdr->ip_dst, pkt, len, my_int->name);
    handle_arpreq(sr, req);
  }
}

/*--------------------------------------------------------------------- 
 * send_icmp3_error
 *
 * Given a type of 3 and related code, constructs the ICMP packet
 * for a Destination Unreachable message (showing the source of the message
 * as the original destination for a Port Unreachable error, or the IP of
 * the given interface for other errors).
 * 
 * Check the ARP cache for a match, send if found, or queue if not.
 *---------------------------------------------------------------------*/
void send_icmp3_error(int type, int code, struct sr_instance *sr, uint8_t *orig_pkt, char *interface) {
  unsigned int plen = sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t);
  uint8_t *ret_pkt = malloc(plen);

  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(ret_pkt + sizeof(sr_ethernet_hdr_t));
  sr_icmp_t3_hdr_t *icmp_hdr = (sr_icmp_t3_hdr_t *)(ret_pkt + sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t));
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)(ret_pkt);

  struct sr_if *in_if = sr_get_interface(sr, interface);

  /* Init Ethernet Header */
  memcpy(eth_hdr->ether_shost, in_if->addr, sizeof(uint8_t) * ETHER_ADDR_LEN);
  memset(eth_hdr->ether_dhost, 0, sizeof(uint8_t) * ETHER_ADDR_LEN);
  eth_hdr->ether_type = htons(ethertype_ip);

  memcpy(eth_hdr->ether_dhost, ((sr_ethernet_hdr_t *)(orig_pkt))->ether_shost, sizeof(uint8_t) * ETHER_ADDR_LEN); 

  /* Construct IP Header */
  ip_hdr->ip_v = 4;
  ip_hdr->ip_hl = sizeof(sr_ip_hdr_t) / 4;
  ip_hdr->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
  ip_hdr->ip_tos = 0;
  ip_hdr->ip_id = 0;
  ip_hdr->ip_off = htons(IP_DF);
  ip_hdr->ip_ttl = 64;
  ip_hdr->ip_p = ip_protocol_icmp;
  ip_hdr->ip_sum = 0;
  ip_hdr->ip_dst = ((sr_ip_hdr_t *)(orig_pkt + sizeof(sr_ethernet_hdr_t)))->ip_src; /* Already net. byte order */
    
  /* code field (8 bits): 3 for port unreachable, 1 for host unreachable, 0 for network */
  if (code == 3) {
    ip_hdr->ip_src = ((sr_ip_hdr_t *)(orig_pkt + sizeof(sr_ethernet_hdr_t)))->ip_dst;
  } else {
    ip_hdr->ip_src = in_if->ip;
  }

  ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));

  /* Construct ICMP Header */
  icmp_hdr->icmp_type = type;
  icmp_hdr->icmp_code = code;
  icmp_hdr->next_mtu = 0;
  icmp_hdr->unused = 0;
  memcpy(icmp_hdr->data, orig_pkt + sizeof(sr_ethernet_hdr_t), sizeof(uint8_t) * ICMP_DATA_SIZE);
  icmp_hdr->icmp_sum = 0;
  icmp_hdr->icmp_sum = cksum(icmp_hdr, sizeof(sr_icmp_t3_hdr_t));

  struct sr_arpentry *entry = sr_arpcache_lookup(&sr->cache, ip_hdr->ip_dst);
  if (entry) {
    printf("ARP Cache entry found for ICMP3\n");
    memcpy(eth_hdr->ether_dhost, entry->mac, sizeof(uint8_t) * ETHER_ADDR_LEN);
    sr_send_packet(sr, ret_pkt, plen, in_if->name);
    free(ret_pkt);
  } else {
    printf("ARP Cache entry not found for ICMP3, adding to queue\n");
    struct sr_arpreq *req = sr_arpcache_queuereq(&sr->cache, ip_hdr->ip_dst, ret_pkt, plen, in_if->name);
    handle_arpreq(sr, req);
  }
  
}

/*--------------------------------------------------------------------- 
 * longest_prefix_match
 *
 * Given an instance of SR and the 32 bit destination address, searches
 * SR's routing table for the longest prefix match (if one exists) and
 * returns that entry of the routing table.
 *---------------------------------------------------------------------*/
struct sr_rt *longest_prefix_match(struct sr_instance *sr, uint32_t dest_addr) {
  struct sr_rt *walker = 0;

  /* REQUIRES */
  assert(sr);
  assert(dest_addr);

  walker = sr->routing_table;
  struct sr_rt *longest = 0;
  uint32_t len = 0;
  while (walker) {
    if ((walker->dest.s_addr & walker->mask.s_addr) == (dest_addr & walker->mask.s_addr)) {
      if ((walker->mask.s_addr & dest_addr) > len) {
        len = walker->mask.s_addr & dest_addr;
        longest = walker;
      }
    }
    walker = walker->next;
  }
  return longest;
}
