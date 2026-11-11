#ifndef DISCOVER_H
#define DISCOVER_H

// Finds Sunshine hosts on the local network with mDNS (multicast DNS, the
// "Bonjour"/Avahi protocol). Sunshine advertises the service _nvstream._tcp.
//
// We send a "legacy unicast" query: an ordinary DNS question sent to the mDNS
// group address 224.0.0.251:5353 from a normal (non-5353) port. RFC 6762
// section 6.7 says responders answer such queries directly to the sender, so
// the Wii never has to join the multicast group (which libogc can't do).
//
// Everything is non-blocking: call discover_poll() once per frame.

#define DISCOVER_MAX_HOSTS 16

typedef struct {
    char name[64];          /* instance name Sunshine advertises (usually the PC's name) */
    char ip[16];            /* dotted IPv4 */
    unsigned short port;    /* HTTP port from the SRV record (47989 by default) */
} DISCOVERED_HOST;

int  discover_begin(void);          /* opens the socket and sends the first query; 0 = ok */
void discover_poll(void);           /* reads replies, re-sends queries on a schedule */
void discover_end(void);            /* closes the socket and logs a summary */
int  discover_searching(void);      /* 1 during the initial burst of queries */
int  discover_count(void);
const DISCOVERED_HOST *discover_host(int i);

/* Pure parser (no Wii APIs; unit-tested on the PC). pkt is one DNS response.
 * src_ip is the sender's IPv4 in host order (0 if unknown). Returns 1 and
 * fills *out when the packet describes a Sunshine/GameStream host. */
int mdns_parse_response(const unsigned char *pkt, int len, unsigned int src_ip, DISCOVERED_HOST *out);

/* Builds the PTR query for _nvstream._tcp.local. Returns its length. */
int mdns_build_query(unsigned char *buf, int cap);

#endif
