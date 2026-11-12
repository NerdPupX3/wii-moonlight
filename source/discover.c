// mDNS discovery of Sunshine hosts. See discover.h for the protocol choice.
//
// A DNS message is a 12-byte header, then question and resource records.
// Names are length-prefixed labels ("\x09_nvstream\x04_tcp\x05local\x00"), and
// can be shortened with 2-byte pointers back into the packet (top two bits 11).
// From a reply we use:
//   PTR  _nvstream._tcp.local -> "<instance>._nvstream._tcp.local"  (the name)
//   SRV  <instance>...        -> port + target host name           (the port)
//   A    <target>             -> IPv4 address                       (the IP)
//
// Logging (moonlight.log, "[discover]"): socket setup, every query sent and its
// result, every reply (source, what was parsed), and a summary on discover_end.

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>

#include "discover.h"

#define NVSTREAM_SERVICE "_nvstream._tcp.local"
#define DEFAULT_HTTP_PORT 47989

/* ------------------------------------------------------------------ parser */

static int rd16(const unsigned char *p) { return (p[0] << 8) | p[1]; }

/* Reads a (possibly compressed) name starting at off into out as dotted text.
 * Returns the offset just past the name in the ORIGINAL position, or -1. */
static int read_name(const unsigned char *pkt, int len, int off, char *out, int outsz)
{
    int pos = off, end = -1, jumps = 0, n = 0;
    if (outsz > 0) out[0] = 0;
    while (1) {
        if (pos >= len) return -1;
        int l = pkt[pos];
        if (l == 0) {
            if (end < 0) end = pos + 1;
            break;
        }
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++jumps > 16) return -1;
            if (end < 0) end = pos + 2;
            pos = ((l & 0x3F) << 8) | pkt[pos + 1];
            continue;
        }
        if ((l & 0xC0) != 0 || pos + 1 + l > len) return -1;
        if (n > 0 && n < outsz - 1) out[n++] = '.';
        for (int i = 0; i < l; i++) {
            if (n < outsz - 1) out[n++] = (char)pkt[pos + 1 + i];
        }
        pos += 1 + l;
    }
    if (outsz > 0) out[n < outsz ? n : outsz - 1] = 0;
    return end;
}

static int ends_with_nocase(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    const char *a = s + ls - lf;
    for (size_t i = 0; i < lf; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

/* The first label of "Instance Name._nvstream._tcp.local", made printable for
 * our 7-bit font. */
static void instance_label(const char *full, char *out, int outsz)
{
    size_t lf = strlen(NVSTREAM_SERVICE);
    size_t ls = strlen(full);
    size_t n = ls > lf + 1 ? ls - lf - 1 : ls;
    int j = 0;
    for (size_t i = 0; i < n && j < outsz - 1; i++) {
        unsigned char c = (unsigned char)full[i];
        out[j++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[j] = 0;
}

static void ip_to_text(unsigned int ip, char *out)
{
    sprintf(out, "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

int mdns_parse_response(const unsigned char *pkt, int len, unsigned int src_ip, DISCOVERED_HOST *out)
{
    if (len < 12) return 0;
    int flags = rd16(pkt + 2);
    if (!(flags & 0x8000)) return 0;                 /* a query, not a response */
    int qd = rd16(pkt + 4);
    int rr = rd16(pkt + 6) + rd16(pkt + 8) + rd16(pkt + 10);
    int off = 12;
    char name[256], target[256], srv_target[256] = "";
    int found = 0, port = 0;
    unsigned int a_ip = 0;
    char inst[64] = "";

    for (int i = 0; i < qd; i++) {                    /* skip the echoed questions */
        off = read_name(pkt, len, off, name, sizeof(name));
        if (off < 0 || off + 4 > len) return 0;
        off += 4;
    }

    /* Two passes: the A record for the SRV target may come before the SRV. */
    unsigned int a_ips[8];
    char a_names[8][256];
    int na = 0;

    for (int i = 0; i < rr; i++) {
        off = read_name(pkt, len, off, name, sizeof(name));
        if (off < 0 || off + 10 > len) break;
        int type = rd16(pkt + off);
        int rdlen = rd16(pkt + off + 8);
        int rdata = off + 10;
        if (rdata + rdlen > len) break;

        if (type == 12 && ends_with_nocase(name, NVSTREAM_SERVICE)) {          /* PTR */
            if (read_name(pkt, len, rdata, target, sizeof(target)) > 0 &&
                ends_with_nocase(target, NVSTREAM_SERVICE)) {
                instance_label(target, inst, sizeof(inst));
                found = 1;
            }
        } else if (type == 33 && ends_with_nocase(name, NVSTREAM_SERVICE) && rdlen >= 7) { /* SRV */
            port = rd16(pkt + rdata + 4);
            read_name(pkt, len, rdata + 6, srv_target, sizeof(srv_target));
            if (!inst[0]) instance_label(name, inst, sizeof(inst));
            found = 1;
        } else if (type == 1 && rdlen == 4 && na < 8) {                       /* A */
            a_ips[na] = ((unsigned)pkt[rdata] << 24) | (pkt[rdata + 1] << 16) |
                        (pkt[rdata + 2] << 8) | pkt[rdata + 3];
            snprintf(a_names[na], sizeof(a_names[na]), "%s", name);
            na++;
        }
        off = rdata + rdlen;
    }
    if (!found)
        return 0;

    /* Pick the address. A PC with several network interfaces lists several A
     * records; the packet's sender is the one that reached us, so prefer it
     * when it is among them (or when there are none). */
    for (int i = 0; i < na; i++)
        if (src_ip && a_ips[i] == src_ip) a_ip = src_ip;
    if (!a_ip && srv_target[0])
        for (int i = 0; i < na && !a_ip; i++)
            if (strcasecmp(a_names[i], srv_target) == 0) a_ip = a_ips[i];
    if (!a_ip && src_ip) a_ip = src_ip;
    if (!a_ip && na > 0) a_ip = a_ips[0];
    if (!a_ip)
        return 0;

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", inst[0] ? inst : "Sunshine");
    ip_to_text(a_ip, out->ip);
    out->port = port ? (unsigned short)port : DEFAULT_HTTP_PORT;
    return 1;
}

int mdns_build_query(unsigned char *buf, int cap)
{
    static const char *labels[] = { "_nvstream", "_tcp", "local" };
    if (cap < 64) return 0;
    memset(buf, 0, 12);
    buf[5] = 1;                                     /* one question */
    int off = 12;
    for (int i = 0; i < 3; i++) {
        int l = (int)strlen(labels[i]);
        buf[off++] = (unsigned char)l;
        memcpy(buf + off, labels[i], l);
        off += l;
    }
    buf[off++] = 0;
    buf[off++] = 0; buf[off++] = 12;                /* QTYPE PTR */
    buf[off++] = 0x80; buf[off++] = 0x01;           /* QCLASS IN, "unicast response" bit */
    return off;
}

/* ---------------------------------------------------------- Wii transport */
#ifndef DISCOVER_HOST_TEST

#include <gccore.h>
#include <network.h>
#include <poll.h>
#include <ogc/lwp_watchdog.h>
#include "applog.h"

/* Query times after discover_begin (ms); afterwards, one every REQUERY_MS. */
static const int k_schedule_ms[] = { 0, 300, 1000, 2500 };
#define SCHEDULE_LEN ((int)(sizeof(k_schedule_ms) / sizeof(k_schedule_ms[0])))
#define SEARCH_WINDOW_MS 4000
#define REQUERY_MS 10000
#define REPLY_LOG_LIMIT 40

static s32 g_sock = -1;
static u64 g_start_ms, g_last_query_ms;
static int g_queries, g_replies, g_ignored, g_next_sched;
static DISCOVERED_HOST g_hosts[DISCOVER_MAX_HOSTS];
static int g_count;

/* The Wii's IOS network stack uses an 8-byte sockaddr_in: length, family,
 * port, address, with no sin_zero padding. libogc's struct has the 8 padding
 * bytes, so sizeof() is 16, and IOS rejects that with -EINVAL (-22). Both
 * sin_len and the length argument must say 8. The stream code has the same fix
 * in resolveHostName (patches/moonlight-common-c.patch). */
#define WII_SOCKADDR_LEN 8

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

static void send_query(void)
{
    unsigned char q[64];
    int ql = mdns_build_query(q, sizeof(q));
    struct sockaddr_in dst;

    memset(&dst, 0, sizeof(dst));
    dst.sin_len = WII_SOCKADDR_LEN;
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    dst.sin_addr.s_addr = htonl(0xE00000FB);        /* 224.0.0.251 */
    s32 rm = net_sendto(g_sock, q, ql, 0, (struct sockaddr *)&dst, WII_SOCKADDR_LEN);

    /* Also to the broadcast address: on some networks/IOS versions multicast
     * sends fail, and responders bound to port 5353 receive broadcasts too. */
    dst.sin_addr.s_addr = htonl(0xFFFFFFFF);
    s32 rb = net_sendto(g_sock, q, ql, 0, (struct sockaddr *)&dst, WII_SOCKADDR_LEN);

    g_queries++;
    g_last_query_ms = now_ms();
    app_log("[discover] query %d sent (%d bytes): multicast=%d broadcast=%d\n", g_queries, ql, rm, rb);
}

int discover_begin(void)
{
    discover_end();
    g_count = g_queries = g_replies = g_ignored = g_next_sched = 0;

    /* Protocol MUST be 0 (IPPROTO_IP, "the default for this type", which is
     * UDP for SOCK_DGRAM). libogc's net_socket refuses IPPROTO_UDP with
     * -EPROTONOSUPPORT (-123), which silently disabled the whole search. */
    g_sock = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (g_sock < 0) {
        app_log("[discover] ERROR: net_socket failed (%d)\n", g_sock);
        g_sock = -1;
        return -1;
    }
    int yes = 1;
    s32 rbc = net_setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_len = WII_SOCKADDR_LEN;
    local.sin_family = AF_INET;
    local.sin_port = 0;                             /* any port except 5353 = legacy unicast */
    local.sin_addr.s_addr = INADDR_ANY;
    s32 rbind = net_bind(g_sock, (struct sockaddr *)&local, WII_SOCKADDR_LEN);

    u32 me = net_gethostip();
    app_log("[discover] start: socket=%d SO_BROADCAST=%d bind=%d, this Wii is %u.%u.%u.%u\n",
            g_sock, rbc, rbind, (me >> 24) & 255, (me >> 16) & 255, (me >> 8) & 255, me & 255);
    g_start_ms = now_ms();
    return 0;
}

static void add_host(const DISCOVERED_HOST *h)
{
    /* A PC on the network twice (cable + Wi-Fi) answers from both addresses
     * under the same name: list it once, at the first address that answered. */
    for (int i = 0; i < g_count; i++) {
        if (strcmp(h->name, "Sunshine") != 0 && strcmp(g_hosts[i].name, h->name) == 0 &&
            strcmp(g_hosts[i].ip, h->ip) != 0) {
            if (g_replies <= REPLY_LOG_LIMIT)
                app_log("[discover] \"%s\" also answered from %s; keeping %s\n",
                        h->name, h->ip, g_hosts[i].ip);
            return;
        }
    }
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_hosts[i].ip, h->ip) == 0) {
            if (strcmp(g_hosts[i].name, h->name) != 0 && strcmp(h->name, "Sunshine") != 0)
                g_hosts[i] = *h;
            return;
        }
    }
    if (g_count < DISCOVER_MAX_HOSTS) {
        g_hosts[g_count++] = *h;
        app_log("[discover] host %d: \"%s\" at %s port %u\n", g_count, h->name, h->ip, h->port);
    }
}

void discover_poll(void)
{
    if (g_sock < 0)
        return;
    u64 t = now_ms() - g_start_ms;
    if (g_next_sched < SCHEDULE_LEN && t >= (u64)k_schedule_ms[g_next_sched]) {
        g_next_sched++;
        send_query();
    } else if (g_next_sched >= SCHEDULE_LEN && now_ms() - g_last_query_ms >= REQUERY_MS) {
        send_query();
    }

    static unsigned char buf[1500];
    for (int k = 0; k < 8; k++) {                   /* at most 8 packets per frame */
        /* The Wii's recvfrom can block even on a non-blocking socket, so poll
         * first (same workaround as the ENet port). */
        struct pollsd p;
        p.socket = g_sock;
        p.events = POLLIN;
        p.revents = 0;
        if (net_poll(&p, 1, 0) <= 0)
            return;

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);           /* IOS rejects larger lengths */
        memset(&from, 0, sizeof(from));
        s32 n = net_recvfrom(g_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            app_log("[discover] recvfrom returned %d\n", n);
            return;
        }
        g_replies++;
        /* The Wii sometimes reports a garbage sender (see the ENet fix); only
         * trust it when it looks like IPv4. */
        unsigned int src = (from.sin_family == AF_INET) ? ntohl(from.sin_addr.s_addr) : 0;

        DISCOVERED_HOST h;
        if (mdns_parse_response(buf, n, src, &h)) {
            if (g_replies <= REPLY_LOG_LIMIT)
                app_log("[discover] reply %d from %u.%u.%u.%u (%d bytes): \"%s\" %s:%u\n", g_replies,
                        (src >> 24) & 255, (src >> 16) & 255, (src >> 8) & 255, src & 255, n,
                        h.name, h.ip, h.port);
            add_host(&h);
        } else {
            g_ignored++;
            if (g_ignored <= REPLY_LOG_LIMIT)
                app_log("[discover] reply %d from %u.%u.%u.%u (%d bytes) ignored: no Sunshine records "
                        "(family=%d)\n", g_replies, (src >> 24) & 255, (src >> 16) & 255,
                        (src >> 8) & 255, src & 255, n, from.sin_family);
        }
    }
}

void discover_end(void)
{
    if (g_sock < 0)
        return;
    net_close(g_sock);
    g_sock = -1;
    app_log("[discover] stop: %d queries, %d replies (%d ignored), %d hosts, %d ms\n",
            g_queries, g_replies, g_ignored, g_count, (int)(now_ms() - g_start_ms));
}

int discover_searching(void)
{
    return g_sock >= 0 && now_ms() - g_start_ms < SEARCH_WINDOW_MS;
}

int discover_count(void) { return g_count; }

const DISCOVERED_HOST *discover_host(int i)
{
    return (i >= 0 && i < g_count) ? &g_hosts[i] : NULL;
}

#endif /* DISCOVER_HOST_TEST */
