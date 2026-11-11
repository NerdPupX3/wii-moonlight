/* Missing lwIP network symbols shims for the Wii.
 *
 * libogc's lwIP port declares (in headers) but never defines a few standard
 * socket symbols that moonlight-common-c's bundled enet relies on:
 *
 *   - ioctl():        the standard symbol is absent; net_ioctl() provides it.
 *   - in6addr_any:    declared in <netinet/in.h> but never defined. enet's
 *                     IPv6 wildcard-address path references it. moonlight
 *                     streams over IPv4, so this is never dereferenced at
 *                     runtime, but the linker still needs a definition.
 */
#include <stdarg.h>
#include <network.h>
#include <netinet/in.h>

int ioctl(int fd, int request, ...) {
    va_list args;
    void *argp;

    va_start(args, request);
    argp = va_arg(args, void *);
    va_end(args);

    return (int)net_ioctl((s32)fd, (u32)request, argp);
}

const struct in6_addr in6addr_any = { 0 };
