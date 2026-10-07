/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ws2_32.dll (and wsock32.dll): Winsock for a machine with no network.
 *
 * Starting it up works, the byte-order and address-text functions work, and
 * numeric addresses resolve, but no socket can be made: socket() fails as it
 * does when the network is down, and every call on a socket handle reports
 * that the handle isn't one. Names don't resolve. That is enough for
 * libraries that link a network stack (FFmpeg's protocols) to load and to
 * find that nothing is reachable, which is true to what the harness is.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "win32/codepage.h"
#include "win32/internal.h"

typedef UINT_PTR SOCKET;
#define INVALID_SOCKET (~(SOCKET)0)
#define SOCKET_ERROR   (-1)

#define WSAEINVAL           10022
#define WSAENOTSOCK         10038
#define WSAEAFNOSUPPORT     10047
#define WSAENETDOWN         10050
#define WSAVERNOTSUPPORTED  10092
#define WSANOTINITIALISED   10093
#define WSAHOST_NOT_FOUND   11001
#define WSATYPE_NOT_FOUND   10109

#define WIN_AF_INET   2
#define WIN_AF_INET6  23

typedef struct {
	WORD wVersion, wHighVersion;
	char szDescription[257];
	char szSystemStatus[129];
	unsigned short iMaxSockets, iMaxUdpDg;
	char *lpVendorInfo;
} WSADATA;

typedef struct {
	UINT fd_count;
	SOCKET fd_array[64];
} win_fd_set;

/* struct addrinfo, struct sockaddr_in and sockaddr_in6 as Windows lays them out. */
struct win_addrinfo {
	int ai_flags, ai_family, ai_socktype, ai_protocol;
	SIZE_T ai_addrlen;
	char *ai_canonname;
	void *ai_addr;
	struct win_addrinfo *ai_next;
};

struct win_sockaddr_in {
	short family;
	unsigned short port;
	BYTE addr[4];
	BYTE zero[8];
};

struct win_sockaddr_in6 {
	short family;
	unsigned short port;
	DWORD flowinfo;
	BYTE addr[16];
	DWORD scope_id;
};

static int started;	/* WSAStartup calls not yet matched by WSACleanup */

static int fail(int err)
{
	win_set_error(err);
	return SOCKET_ERROR;
}

WINFN(int) ws_WSAStartup(WORD version, WSADATA *data)
{
	if ((version & 0xff) == 0 || (version & 0xff) > 2)
		return WSAVERNOTSUPPORTED;
	if (!data)
		return WSAEINVAL;
	memset(data, 0, sizeof(*data));
	/* The version asked for, down to the highest there is: 2.2. */
	data->wVersion = (version & 0xff) == 2 && (version >> 8) > 2 ? 0x0202 : version;
	data->wHighVersion = 0x0202;
	strcpy(data->szDescription, "WinSock 2.0 (seance, no network)");
	strcpy(data->szSystemStatus, "Running");
	data->iMaxSockets = 0;
	data->iMaxUdpDg = 0;
	__atomic_add_fetch(&started, 1, __ATOMIC_SEQ_CST);
	return 0;
}

WINFN(int) ws_WSACleanup(void)
{
	int n = __atomic_load_n(&started, __ATOMIC_SEQ_CST);

	do {
		if (!n)
			return fail(WSANOTINITIALISED);
	} while (!__atomic_compare_exchange_n(&started, &n, n - 1, 0,
					      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	return 0;
}

WINFN(int) ws_WSAGetLastError(void)
{
	return (int)win_get_error();
}

WINFN(void) ws_WSASetLastError(int err)
{
	win_set_error((DWORD)err);
}

/* Byte order: the host is little-endian, the network big. */
WINFN(unsigned short) ws_htons(unsigned short v) { return htons(v); }
WINFN(unsigned short) ws_ntohs(unsigned short v) { return ntohs(v); }
WINFN(DWORD) ws_ntohl(DWORD v) { return ntohl(v); }

/* No sockets. */

WINFN(SOCKET) ws_socket(int af, int type, int proto)
{
	win_set_error(!__atomic_load_n(&started, __ATOMIC_SEQ_CST) ? WSANOTINITIALISED :
		      af == WIN_AF_INET || af == WIN_AF_INET6 ? WSAENETDOWN :
		      WSAEAFNOSUPPORT);
	return INVALID_SOCKET;
}

#define NOT_A_SOCKET(name, ...) \
	WINFN(int) ws_##name(__VA_ARGS__) { return fail(WSAENOTSOCK); }

NOT_A_SOCKET(closesocket, SOCKET s)
NOT_A_SOCKET(shutdown, SOCKET s, int how)
NOT_A_SOCKET(bind, SOCKET s, const void *addr, int len)
NOT_A_SOCKET(connect, SOCKET s, const void *addr, int len)
NOT_A_SOCKET(listen, SOCKET s, int backlog)
NOT_A_SOCKET(send, SOCKET s, const char *buf, int len, int flags)
NOT_A_SOCKET(recv, SOCKET s, char *buf, int len, int flags)
NOT_A_SOCKET(sendto, SOCKET s, const char *buf, int len, int flags, const void *to, int tolen)
NOT_A_SOCKET(recvfrom, SOCKET s, char *buf, int len, int flags, void *from, int *fromlen)
NOT_A_SOCKET(getsockopt, SOCKET s, int level, int name, char *val, int *len)
NOT_A_SOCKET(setsockopt, SOCKET s, int level, int name, const char *val, int len)
NOT_A_SOCKET(ioctlsocket, SOCKET s, long cmd, DWORD *arg)
NOT_A_SOCKET(getpeername, SOCKET s, void *addr, int *len)
NOT_A_SOCKET(getsockname, SOCKET s, void *addr, int *len)
NOT_A_SOCKET(WSASend, SOCKET s, void *bufs, DWORD n, DWORD *sent, DWORD flags, void *ov, void *cb)

WINFN(SOCKET) ws_accept(SOCKET s, void *addr, int *len)
{
	win_set_error(WSAENOTSOCK);
	return INVALID_SOCKET;
}

WINFN(int) ws_select(int nfds, win_fd_set *r, win_fd_set *w, win_fd_set *e,
		     const void *timeout)
{
	/* With nothing to wait on, Windows refuses; with sockets, they're not. */
	if ((!r || !r->fd_count) && (!w || !w->fd_count) && (!e || !e->fd_count))
		return fail(WSAEINVAL);
	return fail(WSAENOTSOCK);
}

WINFN(int) ws___WSAFDIsSet(SOCKET s, win_fd_set *set)
{
	if (set)
		for (UINT i = 0; i < set->fd_count; i++)
			if (set->fd_array[i] == s)
				return 1;
	return 0;
}

/* Address text. */

static int host_family(int af)
{
	return af == WIN_AF_INET ? AF_INET : af == WIN_AF_INET6 ? AF_INET6 : -1;
}

WINFN(int) ws_inet_pton(int af, const char *src, void *dst)
{
	int family = host_family(af);

	if (family < 0) {
		win_set_error(WSAEAFNOSUPPORT);
		return -1;
	}
	if (!src || !dst) {
		win_set_error(WSAEINVAL);
		return -1;
	}
	return inet_pton(family, src, dst);
}

WINFN(const char *) ws_inet_ntop(int af, const void *src, char *dst, SIZE_T size)
{
	int family = host_family(af);

	if (family < 0) {
		win_set_error(WSAEAFNOSUPPORT);
		return NULL;
	}
	if (!src || !dst) {
		win_set_error(WSAEINVAL);
		return NULL;
	}
	if (!inet_ntop(family, src, dst, (socklen_t)size)) {
		win_set_error(WSAEINVAL);
		return NULL;
	}
	return dst;
}

/*
 * Names: only the numeric addresses and port numbers resolve. Everything
 * else is "host not found", as on a machine with no resolver to ask.
 */

WINFN(int) ws_gethostname(char *name, int len)
{
	static const char host[] = "seance";

	if (!name || len < (int)sizeof(host))
		return fail(WSAEINVAL);
	memcpy(name, host, sizeof(host));
	return 0;
}

WINFN(int) ws_getaddrinfo(const char *node, const char *service,
			  const struct win_addrinfo *hints, struct win_addrinfo **res)
{
	struct win_addrinfo *ai;
	struct win_sockaddr_in6 sa6;
	struct win_sockaddr_in sa4;
	int family = hints ? hints->ai_family : 0;
	unsigned short port = 0;
	char *end;
	BYTE addr[16];

	if (res)
		*res = NULL;
	if (!res || (!node && !service))
		return WSAHOST_NOT_FOUND;
	if (service) {
		unsigned long p = strtoul(service, &end, 10);

		if (!*service || *end || p > 65535)
			return WSATYPE_NOT_FOUND;
		port = htons((unsigned short)p);
	}
	memset(&sa4, 0, sizeof(sa4));
	memset(&sa6, 0, sizeof(sa6));
	if (!node) {
		/* The wildcard or loopback address, by whether it's for a server. */
		sa4.family = WIN_AF_INET;
		sa4.port = port;
		if (hints && (hints->ai_flags & 1 /* AI_PASSIVE */))
			memset(sa4.addr, 0, 4);
		else
			memcpy(sa4.addr, "\x7f\0\0\x01", 4);
		family = WIN_AF_INET;
	} else if ((family == 0 || family == WIN_AF_INET) &&
		   inet_pton(AF_INET, node, addr) == 1) {
		sa4.family = WIN_AF_INET;
		sa4.port = port;
		memcpy(sa4.addr, addr, 4);
		family = WIN_AF_INET;
	} else if ((family == 0 || family == WIN_AF_INET6) &&
		   inet_pton(AF_INET6, node, addr) == 1) {
		sa6.family = WIN_AF_INET6;
		sa6.port = port;
		memcpy(sa6.addr, addr, 16);
		family = WIN_AF_INET6;
	} else
		return WSAHOST_NOT_FOUND;

	ai = calloc(1, sizeof(*ai));
	if (!ai)
		return WSAEINVAL;
	ai->ai_family = family;
	ai->ai_socktype = hints ? hints->ai_socktype : 0;
	ai->ai_protocol = hints ? hints->ai_protocol : 0;
	ai->ai_addrlen = family == WIN_AF_INET ? sizeof(sa4) : sizeof(sa6);
	ai->ai_addr = malloc(ai->ai_addrlen);
	if (!ai->ai_addr) {
		free(ai);
		return WSAEINVAL;
	}
	memcpy(ai->ai_addr, family == WIN_AF_INET ? (void *)&sa4 : (void *)&sa6,
	       ai->ai_addrlen);
	*res = ai;
	return 0;
}

WINFN(void) ws_freeaddrinfo(struct win_addrinfo *ai)
{
	while (ai) {
		struct win_addrinfo *next = ai->ai_next;

		free(ai->ai_addr);
		free(ai->ai_canonname);
		free(ai);
		ai = next;
	}
}

WINFN(int) ws_getnameinfo(const void *sa, int salen, char *host, DWORD hostlen,
			  char *serv, DWORD servlen, int flags)
{
	const short *family = sa;
	char text[64];

	if (!sa || salen < (int)sizeof(short))
		return WSAEINVAL;
	if (*family == WIN_AF_INET && salen >= (int)sizeof(struct win_sockaddr_in)) {
		const struct win_sockaddr_in *a = sa;

		if (host && hostlen && !inet_ntop(AF_INET, a->addr, text, sizeof(text)))
			return WSAEINVAL;
		if (host && hostlen) {
			if (strlen(text) >= hostlen)
				return WSAEINVAL;
			strcpy(host, text);
		}
		if (serv && servlen) {
			snprintf(text, sizeof(text), "%u", ntohs(a->port));
			if (strlen(text) >= servlen)
				return WSAEINVAL;
			strcpy(serv, text);
		}
		return 0;
	}
	if (*family == WIN_AF_INET6 && salen >= (int)sizeof(struct win_sockaddr_in6)) {
		const struct win_sockaddr_in6 *a = sa;

		if (host && hostlen) {
			if (!inet_ntop(AF_INET6, a->addr, text, sizeof(text)) ||
			    strlen(text) >= hostlen)
				return WSAEINVAL;
			strcpy(host, text);
		}
		if (serv && servlen) {
			snprintf(text, sizeof(text), "%u", ntohs(a->port));
			if (strlen(text) >= servlen)
				return WSAEINVAL;
			strcpy(serv, text);
		}
		return 0;
	}
	return WSAEAFNOSUPPORT;
}

#define E(name) { #name, 0, (void *)ws_##name }

static const struct builtin_export exports[] = {
	E(WSACleanup),
	E(WSAGetLastError),
	E(WSASend),
	E(WSASetLastError),
	E(WSAStartup),
	E(__WSAFDIsSet),
	E(accept),
	E(bind),
	E(closesocket),
	E(connect),
	E(freeaddrinfo),
	E(getaddrinfo),
	E(gethostname),
	E(getnameinfo),
	E(getpeername),
	E(getsockname),
	E(getsockopt),
	E(htons),
	E(inet_ntop),
	E(inet_pton),
	E(ioctlsocket),
	E(listen),
	E(ntohl),
	E(ntohs),
	E(recv),
	E(recvfrom),
	E(select),
	E(send),
	E(sendto),
	E(setsockopt),
	E(shutdown),
	E(socket),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };
static const char *const aliases[] = { "wsock32.dll", NULL };

const struct builtin_dll builtin_ws2_32 = { "ws2_32.dll", aliases, tables, 0 };
