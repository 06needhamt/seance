/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * wininet.dll as a machine that is offline. Codecs and filters use WinINet
 * to fetch updates, subtitles or licences (PSI_V, mohd, scmpack, VSFilter,
 * the LEAD network filters), which the harness never wants to do: a
 * session opens, as it does offline on Windows, but every connection fails
 * with ERROR_INTERNET_DISCONNECTED, and InternetGetConnectedState says
 * offline, so callers take their no-network path.
 *
 * The URL helpers (InternetCrackUrl, InternetCanonicalizeUrl,
 * InternetCombineUrl, InternetCreateUrl) need no network and really work,
 * for the codecs that use them on file names and stream URLs.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/wininet.h"

#define ERROR_CANCELLED 1223
/* Open sessions, the only kind of handle there is; a list, so a stray
 * handle is never dereferenced. */
struct session {
	struct session *next;
};

static struct session *sessions;
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_session(HINTERNET h)
{
	int found = 0;

	pthread_mutex_lock(&sessions_lock);
	for (struct session *s = sessions; s && !found; s = s->next)
		found = (HINTERNET)s == h;
	pthread_mutex_unlock(&sessions_lock);
	return h && found;
}

static HINTERNET open_session(void)
{
	struct session *s = calloc(1, sizeof(*s));

	if (!s) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	pthread_mutex_lock(&sessions_lock);
	s->next = sessions;
	sessions = s;
	pthread_mutex_unlock(&sessions_lock);
	return s;
}

WINFN(HINTERNET) inet_InternetOpenA(LPCSTR agent, DWORD access, LPCSTR proxy,
				    LPCSTR bypass, DWORD flags)
{
	return open_session();
}

WINFN(HINTERNET) inet_InternetOpenW(LPCWSTR agent, DWORD access, LPCWSTR proxy,
				    LPCWSTR bypass, DWORD flags)
{
	return open_session();
}

WINFN(BOOL) inet_InternetCloseHandle(HINTERNET h)
{
	struct session *found = NULL;

	pthread_mutex_lock(&sessions_lock);
	for (struct session **p = &sessions; *p; p = &(*p)->next)
		if ((HINTERNET)*p == h) {
			found = *p;
			*p = found->next;
			break;
		}
	pthread_mutex_unlock(&sessions_lock);
	if (!found) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	free(found);
	return TRUE;
}

/* Any connection: a session's is refused, as offline; anything else is no
 * handle at all, since no other kind is ever made. */
static HINTERNET no_connection(HINTERNET h)
{
	win_set_error(is_session(h) ? ERROR_INTERNET_DISCONNECTED :
				      ERROR_INVALID_HANDLE);
	return NULL;
}

/* A call on a request or file handle, which never exist. */
static BOOL no_request(HINTERNET h)
{
	win_set_error(is_session(h) ? ERROR_INTERNET_INCORRECT_HANDLE_TYPE :
				      ERROR_INVALID_HANDLE);
	return FALSE;
}

WINFN(HINTERNET) inet_InternetConnectA(HINTERNET h, LPCSTR server, INTERNET_PORT port,
				       LPCSTR user, LPCSTR pass, DWORD service,
				       DWORD flags, DWORD_PTR ctx)
{
	return no_connection(h);
}

WINFN(HINTERNET) inet_InternetConnectW(HINTERNET h, LPCWSTR server, INTERNET_PORT port,
				       LPCWSTR user, LPCWSTR pass, DWORD service,
				       DWORD flags, DWORD_PTR ctx)
{
	return no_connection(h);
}

WINFN(HINTERNET) inet_InternetOpenUrlA(HINTERNET h, LPCSTR url, LPCSTR headers,
				       DWORD n, DWORD flags, DWORD_PTR ctx)
{
	return no_connection(h);
}

WINFN(HINTERNET) inet_InternetOpenUrlW(HINTERNET h, LPCWSTR url, LPCWSTR headers,
				       DWORD n, DWORD flags, DWORD_PTR ctx)
{
	return no_connection(h);
}

WINFN(HINTERNET) inet_HttpOpenRequestA(HINTERNET h, LPCSTR verb, LPCSTR obj,
				       LPCSTR version, LPCSTR referrer,
				       LPCSTR *accept, DWORD flags, DWORD_PTR ctx)
{
	no_request(h);
	return NULL;
}

WINFN(HINTERNET) inet_HttpOpenRequestW(HINTERNET h, LPCWSTR verb, LPCWSTR obj,
				       LPCWSTR version, LPCWSTR referrer,
				       LPCWSTR *accept, DWORD flags, DWORD_PTR ctx)
{
	no_request(h);
	return NULL;
}

WINFN(BOOL) inet_HttpSendRequestA(HINTERNET h, LPCSTR headers, DWORD n,
				  LPVOID opt, DWORD optn)
{
	return no_request(h);
}

WINFN(BOOL) inet_HttpSendRequestW(HINTERNET h, LPCWSTR headers, DWORD n,
				  LPVOID opt, DWORD optn)
{
	return no_request(h);
}

WINFN(BOOL) inet_HttpSendRequestExA(HINTERNET h, void *in, void *out,
				    DWORD flags, DWORD_PTR ctx)
{
	return no_request(h);
}

WINFN(BOOL) inet_HttpSendRequestExW(HINTERNET h, void *in, void *out,
				    DWORD flags, DWORD_PTR ctx)
{
	return no_request(h);
}

WINFN(BOOL) inet_HttpEndRequestA(HINTERNET h, void *out, DWORD flags, DWORD_PTR ctx)
{
	return no_request(h);
}

WINFN(BOOL) inet_HttpEndRequestW(HINTERNET h, void *out, DWORD flags, DWORD_PTR ctx)
{
	return no_request(h);
}

WINFN(BOOL) inet_HttpAddRequestHeadersA(HINTERNET h, LPCSTR headers, DWORD n,
					DWORD mods)
{
	return no_request(h);
}

WINFN(BOOL) inet_HttpQueryInfoA(HINTERNET h, DWORD level, LPVOID buf,
				LPDWORD n, LPDWORD index)
{
	return no_request(h);
}

WINFN(BOOL) inet_HttpQueryInfoW(HINTERNET h, DWORD level, LPVOID buf,
				LPDWORD n, LPDWORD index)
{
	return no_request(h);
}

WINFN(BOOL) inet_InternetReadFile(HINTERNET h, LPVOID buf, DWORD n, LPDWORD done)
{
	if (done)
		*done = 0;
	return no_request(h);
}

WINFN(BOOL) inet_InternetReadFileExA(HINTERNET h, void *buffers, DWORD flags,
				     DWORD_PTR ctx)
{
	return no_request(h);
}

WINFN(BOOL) inet_InternetWriteFile(HINTERNET h, LPCVOID buf, DWORD n, LPDWORD done)
{
	if (done)
		*done = 0;
	return no_request(h);
}

WINFN(BOOL) inet_InternetQueryDataAvailable(HINTERNET h, LPDWORD n, DWORD flags,
					    DWORD_PTR ctx)
{
	if (n)
		*n = 0;
	return no_request(h);
}

WINFN(DWORD) inet_InternetSetFilePointer(HINTERNET h, LONG dist, PLONG high,
					 DWORD method, DWORD_PTR ctx)
{
	no_request(h);
	return (DWORD)-1;
}

WINFN(BOOL) inet_FtpCommandA(HINTERNET h, BOOL expect, DWORD flags, LPCSTR cmd,
			     DWORD_PTR ctx, HINTERNET *out)
{
	if (out)
		*out = NULL;
	return no_request(h);
}

WINFN(DWORD) inet_FtpGetFileSize(HINTERNET h, LPDWORD high)
{
	no_request(h);
	return (DWORD)-1;
}

/* Options are accepted and remembered nowhere; none can be read back. */
WINFN(BOOL) inet_InternetSetOptionA(HINTERNET h, DWORD opt, LPVOID buf, DWORD n)
{
	return TRUE;
}

WINFN(BOOL) inet_InternetSetOptionW(HINTERNET h, DWORD opt, LPVOID buf, DWORD n)
{
	return TRUE;
}

WINFN(BOOL) inet_InternetQueryOptionA(HINTERNET h, DWORD opt, LPVOID buf, LPDWORD n)
{
	win_set_error(ERROR_INTERNET_INVALID_OPTION);
	return FALSE;
}

WINFN(BOOL) inet_InternetQueryOptionW(HINTERNET h, DWORD opt, LPVOID buf, LPDWORD n)
{
	win_set_error(ERROR_INTERNET_INVALID_OPTION);
	return FALSE;
}

/* No status ever happens; the previous callback was none. */
WINFN(void *) inet_InternetSetStatusCallback(HINTERNET h, void *cb) { return NULL; }
WINFN(void *) inet_InternetSetStatusCallbackW(HINTERNET h, void *cb) { return NULL; }

WINFN(BOOL) inet_InternetGetConnectedState(LPDWORD flags, DWORD reserved)
{
	if (flags)
		*flags = INTERNET_CONNECTION_OFFLINE;
	return FALSE;
}

WINFN(BOOL) inet_InternetAutodial(DWORD flags, HWND parent)
{
	win_set_error(ERROR_INTERNET_DISCONNECTED);
	return FALSE;
}

WINFN(DWORD) inet_InternetErrorDlg(HWND w, HINTERNET req, DWORD err, DWORD flags,
				   LPVOID *data)
{
	return ERROR_CANCELLED;
}

WINFN(BOOL) inet_InternetGetLastResponseInfoA(LPDWORD err, LPSTR buf, LPDWORD n)
{
	if (err)
		*err = 0;
	if (buf && n && *n)
		buf[0] = 0;
	if (n)
		*n = 0;
	return TRUE;
}

WINFN(BOOL) inet_InternetGetLastResponseInfoW(LPDWORD err, LPWSTR buf, LPDWORD n)
{
	if (err)
		*err = 0;
	if (buf && n && *n)
		buf[0] = 0;
	if (n)
		*n = 0;
	return TRUE;
}

/* URLs. Parsing works on a narrow copy, one char per character, so its
 * offsets hold for the caller's wide string too. */

static char *narrow(const void *s, int wide, DWORD len)
{
	size_t n = len;

	if (!s)
		return NULL;
	if (!n) {
		if (wide)
			while (((const WCHAR *)s)[n])
				n++;
		else
			n = strlen(s);
	}
	char *out = malloc(n + 1);
	if (!out)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		unsigned c = wide ? ((const WCHAR *)s)[i] : ((const BYTE *)s)[i];
		out[i] = c && c < 0x80 ? (char)c : (c ? '\x7f' : 0);
	}
	out[n] = 0;
	return out;
}

static const struct {
	const char *name;
	INTERNET_SCHEME scheme;
	INTERNET_PORT port;
	int authority;		/* written scheme://... */
} schemes[] = {
	{ "ftp", INTERNET_SCHEME_FTP, INTERNET_DEFAULT_FTP_PORT, 1 },
	{ "gopher", INTERNET_SCHEME_GOPHER, INTERNET_DEFAULT_GOPHER_PORT, 1 },
	{ "http", INTERNET_SCHEME_HTTP, INTERNET_DEFAULT_HTTP_PORT, 1 },
	{ "https", INTERNET_SCHEME_HTTPS, INTERNET_DEFAULT_HTTPS_PORT, 1 },
	{ "file", INTERNET_SCHEME_FILE, 0, 1 },
	{ "news", INTERNET_SCHEME_NEWS, 0, 0 },
	{ "mailto", INTERNET_SCHEME_MAILTO, 0, 0 },
	{ "socks", INTERNET_SCHEME_SOCKS, 0, 1 },
	{ "javascript", INTERNET_SCHEME_JAVASCRIPT, 0, 0 },
	{ "vbscript", INTERNET_SCHEME_VBSCRIPT, 0, 0 },
	{ "res", INTERNET_SCHEME_RES, 0, 1 },
};

static int scheme_index(const char *s, size_t n)
{
	for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++)
		if (strlen(schemes[i].name) == n && !strncasecmp(schemes[i].name, s, n))
			return (int)i;
	return -1;
}

/* A component, as offset and length in the URL. */
struct part {
	size_t off, len;
};

struct parsed {
	struct part scheme, user, pass, host, path, extra;
	int has_port;
	unsigned port;
};

static int parse_url(const char *u, struct parsed *p)
{
	size_t i = 0, n = strlen(u);

	memset(p, 0, sizeof(*p));
	while (i < n && (isalnum((unsigned char)u[i]) || strchr("+-.", u[i])))
		i++;
	if (!i || i >= n || u[i] != ':')
		return 0;
	p->scheme = (struct part){ 0, i };
	i++;
	size_t rest = i;
	if (n - i >= 2 && u[i] == '/' && u[i + 1] == '/') {
		i += 2;
		size_t a = i;
		while (i < n && !strchr("/?#", u[i]))
			i++;
		size_t ae = i, at = ae;
		for (size_t j = a; j < ae; j++)
			if (u[j] == '@')
				at = j;
		size_t h = a;
		if (at < ae) {
			size_t c = a;
			while (c < at && u[c] != ':')
				c++;
			p->user = (struct part){ a, c - a };
			if (c < at)
				p->pass = (struct part){ c + 1, at - c - 1 };
			h = at + 1;
		}
		size_t c = h;
		while (c < ae && u[c] != ':')
			c++;
		p->host = (struct part){ h, c - h };
		if (c < ae) {
			p->has_port = 1;
			p->port = (unsigned)strtoul(u + c + 1, NULL, 10);
		}
		rest = ae;
	}
	i = rest;
	while (i < n && u[i] != '?' && u[i] != '#')
		i++;
	p->path = (struct part){ rest, i - rest };
	p->extra = (struct part){ i, n - i };
	return 1;
}

/* Fills one component of a URL_COMPONENTS: a pointer into the URL when the
 * caller gave a length and no buffer, a copy when it gave both. */
static int set_part(const void *url, int wide, struct part pt, void *field,
		    DWORD *len)
{
	void **ptr = field;

	if (!*len)
		return 1;
	if (!*ptr) {
		*ptr = (BYTE *)url + pt.off * (wide ? sizeof(WCHAR) : 1);
		*len = (DWORD)pt.len;
		return 1;
	}
	if (pt.len + 1 > *len) {
		*len = (DWORD)pt.len + 1;
		return 0;
	}
	if (wide) {
		memcpy(*ptr, (const WCHAR *)url + pt.off, pt.len * sizeof(WCHAR));
		((WCHAR *)*ptr)[pt.len] = 0;
	} else {
		memcpy(*ptr, (const char *)url + pt.off, pt.len);
		((char *)*ptr)[pt.len] = 0;
	}
	*len = (DWORD)pt.len;
	return 1;
}

static BOOL crack_url(const void *url, DWORD len, DWORD flags, void *comps,
		      int wide)
{
	/* The A and W structures agree but for the pointers' targets. */
	URL_COMPONENTSA *c = comps;
	struct parsed p;
	char *u = narrow(url, wide, len);

	if (!url || !c || c->dwStructSize < sizeof(*c)) {
		free(u);
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!u || !parse_url(u, &p)) {
		free(u);
		win_set_error(ERROR_INTERNET_UNRECOGNIZED_SCHEME);
		return FALSE;
	}
	int si = scheme_index(u + p.scheme.off, p.scheme.len);
	c->nScheme = si < 0 ? INTERNET_SCHEME_UNKNOWN : schemes[si].scheme;
	c->nPort = (INTERNET_PORT)(p.has_port ? p.port : si < 0 ? 0 : schemes[si].port);
	int ok = set_part(url, wide, p.scheme, &c->lpszScheme, &c->dwSchemeLength);
	ok &= set_part(url, wide, p.host, &c->lpszHostName, &c->dwHostNameLength);
	ok &= set_part(url, wide, p.user, &c->lpszUserName, &c->dwUserNameLength);
	ok &= set_part(url, wide, p.pass, &c->lpszPassword, &c->dwPasswordLength);
	ok &= set_part(url, wide, p.path, &c->lpszUrlPath, &c->dwUrlPathLength);
	ok &= set_part(url, wide, p.extra, &c->lpszExtraInfo, &c->dwExtraInfoLength);
	free(u);
	if (!ok) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	return TRUE;
}

WINFN(BOOL) inet_InternetCrackUrlA(LPCSTR url, DWORD len, DWORD flags,
				   LPURL_COMPONENTSA c)
{
	return crack_url(url, len, flags, c, 0);
}

WINFN(BOOL) inet_InternetCrackUrlW(LPCWSTR url, DWORD len, DWORD flags,
				   LPURL_COMPONENTSW c)
{
	return crack_url(url, len, flags, c, 1);
}

/* Strings in and out: UTF-8 inside, the caller's A or W outside. */

static char *in_str(const void *s, int wide)
{
	if (!s)
		return NULL;
	return wide ? utf16_to_utf8_dup(s) : acp_to_utf8(s);
}

/* Copies out with *len in characters: the length without the NUL when it
 * fits, else ERROR_INSUFFICIENT_BUFFER and the size needed with it. */
static BOOL out_str(const char *s, void *buf, LPDWORD len, int wide)
{
	if (!len) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (wide) {
		WCHAR *w = utf8_to_utf16_dup(s);
		size_t n = 0;
		while (w && w[n])
			n++;
		if (!w || !buf || n + 1 > *len) {
			*len = (DWORD)n + 1;
			free(w);
			win_set_error(w ? ERROR_INSUFFICIENT_BUFFER : ERROR_NOT_ENOUGH_MEMORY);
			return FALSE;
		}
		memcpy(buf, w, (n + 1) * sizeof(WCHAR));
		*len = (DWORD)n;
		free(w);
		return TRUE;
	}
	char *a = utf8_to_acp_dup(s);
	size_t n = a ? strlen(a) : 0;
	if (!a || !buf || n + 1 > *len) {
		*len = (DWORD)n + 1;
		free(a);
		win_set_error(a ? ERROR_INSUFFICIENT_BUFFER : ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	memcpy(buf, a, n + 1);
	*len = (DWORD)n;
	free(a);
	return TRUE;
}

static int hexval(char c)
{
	return isdigit((unsigned char)c) ? c - '0' :
	       isxdigit((unsigned char)c) ? (tolower((unsigned char)c) - 'a' + 10) : -1;
}

static int unsafe(unsigned char c, DWORD flags)
{
	if (flags & ICU_ENCODE_SPACES_ONLY)
		return c == ' ';
	return c <= ' ' || c >= 0x7f || strchr("<>\"{}|\\^`", c);
}

/* InternetCanonicalizeUrl's work (malloc()ed). */
static char *canonicalize(const char *u, DWORD flags)
{
	size_t n = strlen(u);
	char *out = malloc(n * 3 + 1), *o = out;
	int in_extra = 0;

	if (!out)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)u[i];
		if (c == '?' || c == '#')
			in_extra = 1;
		if ((flags & ICU_DECODE) && c == '%' && i + 2 < n + 0 &&
		    hexval(u[i + 1]) >= 0 && hexval(u[i + 2]) >= 0) {
			*o++ = (char)(hexval(u[i + 1]) * 16 + hexval(u[i + 2]));
			i += 2;
			continue;
		}
		if (!(flags & ICU_NO_ENCODE) && !(in_extra && (flags & ICU_BROWSER_MODE)) &&
		    unsafe(c, flags)) {
			o += sprintf(o, "%%%02X", c);
			continue;
		}
		*o++ = (char)c;
	}
	*o = 0;
	return out;
}

static BOOL canonicalize_url(const void *url, void *buf, LPDWORD len, DWORD flags,
			     int wide)
{
	char *u = in_str(url, wide), *c = u ? canonicalize(u, flags) : NULL;
	BOOL r = FALSE;

	if (!url)
		win_set_error(ERROR_INVALID_PARAMETER);
	else if (!c)
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	else
		r = out_str(c, buf, len, wide);
	free(u);
	free(c);
	return r;
}

WINFN(BOOL) inet_InternetCanonicalizeUrlA(LPCSTR url, LPSTR buf, LPDWORD len,
					  DWORD flags)
{
	return canonicalize_url(url, buf, len, flags, 0);
}

WINFN(BOOL) inet_InternetCanonicalizeUrlW(LPCWSTR url, LPWSTR buf, LPDWORD len,
					  DWORD flags)
{
	return canonicalize_url(url, buf, len, flags, 1);
}

/* rel against base: whole if it has a scheme, from the host's root if it
 * starts with '/', else beside the base's last path segment. */
static char *combine(const char *base, const char *rel)
{
	struct parsed pb, pr;
	char *out;

	if (parse_url(rel, &pr) || !parse_url(base, &pb))
		return strdup(rel);
	if (rel[0] == '/' && rel[1] == '/') {
		out = malloc(pb.scheme.len + 1 + strlen(rel) + 1);
		if (out)
			sprintf(out, "%.*s:%s", (int)pb.scheme.len, base, rel);
		return out;
	}
	size_t keep;
	if (rel[0] == '/') {
		keep = pb.path.off;
	} else if (rel[0] == '?' || rel[0] == '#' || !rel[0]) {
		keep = rel[0] == '#' && base[pb.extra.off] == '?' ?
		       pb.extra.off + strcspn(base + pb.extra.off, "#") :
		       pb.extra.off;
	} else {
		keep = pb.path.off;
		for (size_t i = pb.path.off; i < pb.path.off + pb.path.len; i++)
			if (base[i] == '/')
				keep = i + 1;
		if (keep == pb.path.off && pb.host.len) {
			/* "http://host" has an empty path: root it. */
			out = malloc(keep + 1 + strlen(rel) + 1);
			if (out)
				sprintf(out, "%.*s/%s", (int)keep, base, rel);
			return out;
		}
	}
	out = malloc(keep + strlen(rel) + 1);
	if (out)
		sprintf(out, "%.*s%s", (int)keep, base, rel);
	return out;
}

static BOOL combine_url(const void *base, const void *rel, void *buf,
			LPDWORD len, DWORD flags, int wide)
{
	char *b = in_str(base, wide), *r = in_str(rel, wide);
	char *c = b && r ? combine(b, r) : NULL;
	char *f = c ? canonicalize(c, flags) : NULL;
	BOOL ok = FALSE;

	if (!base || !rel)
		win_set_error(ERROR_INVALID_PARAMETER);
	else if (!f)
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	else
		ok = out_str(f, buf, len, wide);
	free(b);
	free(r);
	free(c);
	free(f);
	return ok;
}

WINFN(BOOL) inet_InternetCombineUrlA(LPCSTR base, LPCSTR rel, LPSTR buf,
				     LPDWORD len, DWORD flags)
{
	return combine_url(base, rel, buf, len, flags, 0);
}

WINFN(BOOL) inet_InternetCombineUrlW(LPCWSTR base, LPCWSTR rel, LPWSTR buf,
				     LPDWORD len, DWORD flags)
{
	return combine_url(base, rel, buf, len, flags, 1);
}

/* A component of a URL_COMPONENTS as UTF-8 (malloc()ed; "" if absent). */
static char *comp_str(const void *s, DWORD n, int wide)
{
	if (!s)
		return strdup("");
	if (!n)
		return in_str(s, wide);
	if (wide) {
		WCHAR *w = malloc(((size_t)n + 1) * sizeof(WCHAR));
		if (!w)
			return NULL;
		memcpy(w, s, n * sizeof(WCHAR));
		w[n] = 0;
		char *u = utf16_to_utf8_dup(w);
		free(w);
		return u;
	}
	char *a = strndup(s, n), *u = a ? acp_to_utf8(a) : NULL;
	free(a);
	return u;
}

static BOOL create_url(const void *comps, DWORD flags, void *buf, LPDWORD len,
		       int wide)
{
	const URL_COMPONENTSA *c = comps;

	if (!c || c->dwStructSize < sizeof(*c) || !len) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char *scheme = comp_str(c->lpszScheme, c->dwSchemeLength, wide);
	char *host = comp_str(c->lpszHostName, c->dwHostNameLength, wide);
	char *user = comp_str(c->lpszUserName, c->dwUserNameLength, wide);
	char *pass = comp_str(c->lpszPassword, c->dwPasswordLength, wide);
	char *path = comp_str(c->lpszUrlPath, c->dwUrlPathLength, wide);
	char *extra = comp_str(c->lpszExtraInfo, c->dwExtraInfoLength, wide);
	char *out = NULL;
	BOOL ok = FALSE;

	if (!scheme || !host || !user || !pass || !path || !extra) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		goto done;
	}
	int si = *scheme ? scheme_index(scheme, strlen(scheme)) : -1;
	if (!*scheme) {
		for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++)
			if (schemes[i].scheme == c->nScheme)
				si = (int)i;
		if (si < 0) {
			win_set_error(ERROR_INTERNET_UNRECOGNIZED_SCHEME);
			goto done;
		}
	}
	const char *sname = *scheme ? scheme : schemes[si].name;
	int authority = si < 0 ? *host != 0 : schemes[si].authority;
	size_t size = strlen(sname) + strlen(host) + strlen(user) + strlen(pass) +
		      strlen(path) + strlen(extra) + 32;
	if (!(out = malloc(size))) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		goto done;
	}
	char *o = out;
	o += sprintf(o, "%s:%s", sname, authority ? "//" : "");
	if (*user)
		o += sprintf(o, "%s%s%s@", user, *pass ? ":" : "", pass);
	o += sprintf(o, "%s", host);
	if (c->nPort && (si < 0 || c->nPort != schemes[si].port))
		o += sprintf(o, ":%u", c->nPort);
	sprintf(o, "%s%s", path, extra);
	ok = out_str(out, buf, len, wide);
done:
	free(scheme);
	free(host);
	free(user);
	free(pass);
	free(path);
	free(extra);
	free(out);
	return ok;
}

WINFN(BOOL) inet_InternetCreateUrlA(LPURL_COMPONENTSA c, DWORD flags, LPSTR buf,
				    LPDWORD len)
{
	return create_url(c, flags, buf, len, 0);
}

WINFN(BOOL) inet_InternetCreateUrlW(LPURL_COMPONENTSW c, DWORD flags, LPWSTR buf,
				    LPDWORD len)
{
	return create_url(c, flags, buf, len, 1);
}

#define E(name) { #name, 0, (void *)inet_##name }

static const struct builtin_export exports[] = {
	E(FtpCommandA),
	E(FtpGetFileSize),
	E(HttpAddRequestHeadersA),
	E(HttpEndRequestA),
	E(HttpEndRequestW),
	E(HttpOpenRequestA),
	E(HttpOpenRequestW),
	E(HttpQueryInfoA),
	E(HttpQueryInfoW),
	E(HttpSendRequestA),
	E(HttpSendRequestExA),
	E(HttpSendRequestExW),
	E(HttpSendRequestW),
	E(InternetAutodial),
	E(InternetCanonicalizeUrlA),
	E(InternetCanonicalizeUrlW),
	E(InternetCloseHandle),
	E(InternetCombineUrlA),
	E(InternetCombineUrlW),
	E(InternetConnectA),
	E(InternetConnectW),
	E(InternetCrackUrlA),
	E(InternetCrackUrlW),
	E(InternetCreateUrlA),
	E(InternetCreateUrlW),
	E(InternetErrorDlg),
	E(InternetGetConnectedState),
	E(InternetGetLastResponseInfoA),
	E(InternetGetLastResponseInfoW),
	E(InternetOpenA),
	E(InternetOpenUrlA),
	E(InternetOpenUrlW),
	E(InternetOpenW),
	E(InternetQueryDataAvailable),
	E(InternetQueryOptionA),
	E(InternetQueryOptionW),
	E(InternetReadFile),
	E(InternetReadFileExA),
	E(InternetSetFilePointer),
	E(InternetSetOptionA),
	E(InternetSetOptionW),
	E(InternetSetStatusCallback),
	{ "InternetSetStatusCallbackA", 0, (void *)inet_InternetSetStatusCallback },
	E(InternetSetStatusCallbackW),
	E(InternetWriteFile),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_wininet = { "wininet.dll", NULL, tables, 0 };
