/*
 * kernel32: version and system information, environment, command line,
 * startup info, debug output, error mode and FormatMessage.
 */
#include <cpuid.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"

#define WINDOWS_DIR "C:\\WINDOWS"
#define SYSTEM_DIR  "C:\\WINDOWS\\system32"
#define COMPUTER    "VFWHARNESS"

DWORD copy_out_a(const char *s, LPSTR buf, DWORD size)
{
	char *a = utf8_to_acp_dup(s);
	DWORD len = a ? strlen(a) : 0;

	if (!a) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	if (!buf || size <= len) {
		free(a);
		return len + 1;
	}
	memcpy(buf, a, len + 1);
	free(a);
	return len;
}

DWORD copy_out_w(const char *s, LPWSTR buf, DWORD size)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	DWORD len = w ? wstrlen(w) : 0;

	if (!w) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	if (!buf || size <= len) {
		free(w);
		return len + 1;
	}
	memcpy(buf, w, (len + 1) * sizeof(WCHAR));
	free(w);
	return len;
}

/* Version. */

WINFN(DWORD) k32_GetVersion(void)
{
	struct win_peb *peb = peb_get();
	return (peb->OSBuildNumber << 16) | (peb->OSMinorVersion << 8) |
	       peb->OSMajorVersion;
}

static BOOL version_a(OSVERSIONINFOA *v)
{
	struct win_peb *peb = peb_get();

	if (v->dwOSVersionInfoSize != sizeof(OSVERSIONINFOA) &&
	    v->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXA)) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	v->dwMajorVersion = peb->OSMajorVersion;
	v->dwMinorVersion = peb->OSMinorVersion;
	v->dwBuildNumber = peb->OSBuildNumber;
	v->dwPlatformId = VER_PLATFORM_WIN32_NT;
	snprintf(v->szCSDVersion, sizeof(v->szCSDVersion), "Service Pack %u",
		 peb->OSCSDVersion >> 8);
	if (v->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA)) {
		OSVERSIONINFOEXA *x = (OSVERSIONINFOEXA *)v;
		x->wServicePackMajor = peb->OSCSDVersion >> 8;
		x->wServicePackMinor = 0;
		x->wSuiteMask = 0x100;		/* VER_SUITE_SINGLEUSERTS */
		x->wProductType = 1;		/* VER_NT_WORKSTATION */
		x->wReserved = 0;
	}
	return TRUE;
}

WINFN(BOOL) k32_GetVersionExA(OSVERSIONINFOA *v)
{
	return version_a(v);
}

WINFN(BOOL) k32_GetVersionExW(OSVERSIONINFOW *v)
{
	OSVERSIONINFOEXA a;
	DWORD size = v->dwOSVersionInfoSize;

	if (size != sizeof(OSVERSIONINFOW) && size != sizeof(OSVERSIONINFOEXW)) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	a.dwOSVersionInfoSize = sizeof(a);
	version_a((OSVERSIONINFOA *)&a);
	v->dwMajorVersion = a.dwMajorVersion;
	v->dwMinorVersion = a.dwMinorVersion;
	v->dwBuildNumber = a.dwBuildNumber;
	v->dwPlatformId = a.dwPlatformId;
	for (int i = 0; i < 128; i++)
		v->szCSDVersion[i] = (BYTE)a.szCSDVersion[i];
	if (size == sizeof(OSVERSIONINFOEXW)) {
		OSVERSIONINFOEXW *x = (OSVERSIONINFOEXW *)v;
		x->wServicePackMajor = a.wServicePackMajor;
		x->wServicePackMinor = a.wServicePackMinor;
		x->wSuiteMask = a.wSuiteMask;
		x->wProductType = a.wProductType;
		x->wReserved = 0;
	}
	return TRUE;
}

/* System information. */

WINFN(void) k32_GetSystemInfo(LPSYSTEM_INFO si)
{
	unsigned a, b, c, d;

	memset(si, 0, sizeof(*si));
	si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
	si->dwPageSize = 0x1000;
	si->lpMinimumApplicationAddress = (LPVOID)0x10000;
	si->lpMaximumApplicationAddress = (LPVOID)0x7ffeffff;
	si->dwNumberOfProcessors = peb_get()->NumberOfProcessors;
	si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 32 ? 0xffffffff :
				    (1u << si->dwNumberOfProcessors) - 1;
	si->dwProcessorType = PROCESSOR_INTEL_PENTIUM;
	si->dwAllocationGranularity = 0x10000;
	si->wProcessorLevel = 6;
	if (__get_cpuid(1, &a, &b, &c, &d)) {
		unsigned family = (a >> 8) & 15, model = (a >> 4) & 15;
		if (family == 15)
			family += (a >> 20) & 0xff;
		if (family >= 6)
			model |= ((a >> 16) & 15) << 4;
		si->wProcessorLevel = family;
		si->wProcessorRevision = (model << 8) | (a & 15);
	}
}

WINFN(void) k32_GetNativeSystemInfo(LPSYSTEM_INFO si)
{
	k32_GetSystemInfo(si);
}

WINFN(BOOL) k32_IsProcessorFeaturePresent(DWORD feature)
{
	unsigned a, b, c, d;

	switch (feature) {
	case PF_COMPARE_EXCHANGE_DOUBLE:
	case PF_RDTSC_INSTRUCTION_AVAILABLE:
	case PF_PAE_ENABLED:
	case PF_NX_ENABLED:
		return TRUE;
	case PF_MMX_INSTRUCTIONS_AVAILABLE:
		return __builtin_cpu_supports("mmx") != 0;
	case PF_XMMI_INSTRUCTIONS_AVAILABLE:
		return __builtin_cpu_supports("sse") != 0;
	case PF_XMMI64_INSTRUCTIONS_AVAILABLE:
		return __builtin_cpu_supports("sse2") != 0;
	case PF_SSE3_INSTRUCTIONS_AVAILABLE:
		return __builtin_cpu_supports("sse3") != 0;
	case PF_3DNOW_INSTRUCTIONS_AVAILABLE:
		return __get_cpuid(0x80000001, &a, &b, &c, &d) && (d >> 31);
	default:
		return FALSE;
	}
}

/* Directories and names. */

WINFN(UINT) k32_GetWindowsDirectoryA(LPSTR buf, UINT size) { return copy_out_a(WINDOWS_DIR, buf, size); }
WINFN(UINT) k32_GetWindowsDirectoryW(LPWSTR buf, UINT size) { return copy_out_w(WINDOWS_DIR, buf, size); }
WINFN(UINT) k32_GetSystemWindowsDirectoryA(LPSTR buf, UINT size) { return copy_out_a(WINDOWS_DIR, buf, size); }
WINFN(UINT) k32_GetSystemWindowsDirectoryW(LPWSTR buf, UINT size) { return copy_out_w(WINDOWS_DIR, buf, size); }
WINFN(UINT) k32_GetSystemDirectoryA(LPSTR buf, UINT size) { return copy_out_a(SYSTEM_DIR, buf, size); }
WINFN(UINT) k32_GetSystemDirectoryW(LPWSTR buf, UINT size) { return copy_out_w(SYSTEM_DIR, buf, size); }

WINFN(BOOL) k32_GetComputerNameA(LPSTR buf, LPDWORD size)
{
	DWORD len = sizeof(COMPUTER) - 1;

	if (*size <= len) {
		*size = len + 1;
		win_set_error(ERROR_BUFFER_OVERFLOW);
		return FALSE;
	}
	memcpy(buf, COMPUTER, len + 1);
	*size = len;
	return TRUE;
}

WINFN(BOOL) k32_GetComputerNameW(LPWSTR buf, LPDWORD size)
{
	DWORD len = sizeof(COMPUTER) - 1;

	if (*size <= len) {
		*size = len + 1;
		win_set_error(ERROR_BUFFER_OVERFLOW);
		return FALSE;
	}
	for (DWORD i = 0; i <= len; i++)
		buf[i] = COMPUTER[i];
	*size = len;
	return TRUE;
}

/*
 * Environment: a synthetic Windows one, so codecs don't see Unix paths.
 * Host variables named VFW_ENV_<NAME> are passed through as <NAME>.
 */

static char **env;		/* "NAME=value", UTF-8 */
static size_t env_count;
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t env_once = PTHREAD_ONCE_INIT;

static int env_find(const char *name, size_t len)
{
	for (size_t i = 0; i < env_count; i++)
		if (!strncasecmp(env[i], name, len) && env[i][len] == '=')
			return (int)i;
	return -1;
}

/* Sets (value != NULL) or removes a variable. Call with env_lock held. */
static int env_set(const char *name, const char *value)
{
	size_t len = strlen(name);
	int i = env_find(name, len);

	if (!value) {
		if (i >= 0) {
			free(env[i]);
			env[i] = env[--env_count];
		}
		return 0;
	}
	char *entry = malloc(len + strlen(value) + 2);
	if (!entry)
		return -1;
	sprintf(entry, "%s=%s", name, value);
	if (i >= 0) {
		free(env[i]);
		env[i] = entry;
		return 0;
	}
	char **n = realloc(env, (env_count + 1) * sizeof(*n));
	if (!n) {
		free(entry);
		return -1;
	}
	env = n;
	env[env_count++] = entry;
	return 0;
}

static void env_init(void)
{
	extern char **environ;
	char cpus[16];

	snprintf(cpus, sizeof(cpus), "%u", peb_get()->NumberOfProcessors);
	static const char *const defaults[][2] = {
		{ "ComSpec", SYSTEM_DIR "\\cmd.exe" },
		{ "COMPUTERNAME", COMPUTER },
		{ "OS", "Windows_NT" },
		{ "PATH", SYSTEM_DIR ";" WINDOWS_DIR },
		{ "PATHEXT", ".COM;.EXE;.BAT;.CMD" },
		{ "PROCESSOR_ARCHITECTURE", "x86" },
		{ "SystemDrive", "C:" },
		{ "SystemRoot", WINDOWS_DIR },
		{ "TEMP", WINDOWS_DIR "\\TEMP" },
		{ "TMP", WINDOWS_DIR "\\TEMP" },
		{ "USERNAME", "user" },
		{ "USERPROFILE", WINDOWS_DIR "\\profiles\\user" },
		{ "windir", WINDOWS_DIR },
	};
	for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
		env_set(defaults[i][0], defaults[i][1]);
	env_set("NUMBER_OF_PROCESSORS", cpus);

	for (char **e = environ; e && *e; e++) {
		if (strncmp(*e, "VFW_ENV_", 8))
			continue;
		const char *eq = strchr(*e, '=');
		if (!eq || eq == *e + 8)
			continue;
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(eq - *e - 8), *e + 8);
		env_set(name, eq + 1);
	}
}

/* A copy of the value of name, or NULL. */
static char *env_get(const char *name)
{
	char *v = NULL;

	pthread_once(&env_once, env_init);
	pthread_mutex_lock(&env_lock);
	int i = env_find(name, strlen(name));
	if (i >= 0)
		v = strdup(env[i] + strlen(name) + 1);
	pthread_mutex_unlock(&env_lock);
	return v;
}

static DWORD getenv_common(char *name, char **value)
{
	*value = name ? env_get(name) : NULL;
	free(name);
	if (!*value) {
		win_set_error(ERROR_ENVVAR_NOT_FOUND);
		return 0;
	}
	return 1;
}

WINFN(DWORD) k32_GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
	char *value;
	DWORD r;

	if (!getenv_common(acp_to_utf8(name), &value))
		return 0;
	r = copy_out_a(value, buf, size);
	free(value);
	return r;
}

WINFN(DWORD) k32_GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size)
{
	char *value;
	DWORD r;

	if (!getenv_common(utf16_to_utf8_dup(name), &value))
		return 0;
	r = copy_out_w(value, buf, size);
	free(value);
	return r;
}

static BOOL setenv_common(char *name, char *value, int remove)
{
	int r = -1;

	pthread_once(&env_once, env_init);
	if (name && (value || remove)) {
		pthread_mutex_lock(&env_lock);
		r = env_set(name, remove ? NULL : value);
		pthread_mutex_unlock(&env_lock);
	}
	free(name);
	free(value);
	if (r) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	return TRUE;
}

WINFN(BOOL) k32_SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
	return setenv_common(acp_to_utf8(name), acp_to_utf8(value), !value);
}

WINFN(BOOL) k32_SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value)
{
	return setenv_common(utf16_to_utf8_dup(name), utf16_to_utf8_dup(value),
			     !value);
}

static int compare_env(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* The block of "NAME=value\0...\0\0" (UTF-8), sorted as Windows keeps it. */
static char *env_block_utf8(size_t *len)
{
	pthread_once(&env_once, env_init);
	pthread_mutex_lock(&env_lock);
	qsort(env, env_count, sizeof(*env), compare_env);
	size_t n = 1;
	for (size_t i = 0; i < env_count; i++)
		n += strlen(env[i]) + 1;
	char *block = malloc(n), *p = block;
	if (block) {
		for (size_t i = 0; i < env_count; i++) {
			size_t l = strlen(env[i]) + 1;
			memcpy(p, env[i], l);
			p += l;
		}
		*p = '\0';
	}
	pthread_mutex_unlock(&env_lock);
	*len = n;
	return block;
}

WINFN(LPSTR) k32_GetEnvironmentStringsA(void)
{
	size_t len;
	char *u = env_block_utf8(&len);
	char *out = NULL;

	if (u) {
		/* Convert each string; the NUL separators survive the conversion. */
		WCHAR *w = malloc(len * sizeof(WCHAR));
		if (w && cp_to_utf16(CP_UTF8, u, len, w, len, 0) >= 0) {
			int n = cp_from_utf16(CP_ACP, w, len, NULL, 0, '?', NULL);
			out = malloc(n);
			if (out)
				cp_from_utf16(CP_ACP, w, len, out, n, '?', NULL);
		}
		free(w);
		free(u);
	}
	if (!out)
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	return out;
}

WINFN(LPWSTR) k32_GetEnvironmentStringsW(void)
{
	size_t len;
	char *u = env_block_utf8(&len);
	WCHAR *out = NULL;

	if (u) {
		int n = cp_to_utf16(CP_UTF8, u, len, NULL, 0, 0);
		out = malloc(n * sizeof(WCHAR));
		if (out)
			cp_to_utf16(CP_UTF8, u, len, out, n, 0);
		free(u);
	}
	if (!out)
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	return out;
}

WINFN(BOOL) k32_FreeEnvironmentStringsA(LPSTR block)
{
	free(block);
	return TRUE;
}

WINFN(BOOL) k32_FreeEnvironmentStringsW(LPWSTR block)
{
	free(block);
	return TRUE;
}

/* Expands %NAME% references; unknown ones are left as they are. */
static char *expand(const char *s)
{
	size_t cap = strlen(s) + 64, n = 0;
	char *out = malloc(cap);

	while (out && *s) {
		const char *end = s[0] == '%' ? strchr(s + 1, '%') : NULL;
		char *value = NULL;
		const char *piece = s;
		size_t plen = 1;

		if (end && end > s + 1) {
			char name[256];
			snprintf(name, sizeof(name), "%.*s", (int)(end - s - 1), s + 1);
			value = env_get(name);
			if (value) {
				piece = value;
				plen = strlen(value);
				s = end + 1;
			}
		}
		if (!value)
			s++;
		if (n + plen + 1 > cap) {
			cap = (n + plen + 1) * 2;
			char *g = realloc(out, cap);
			if (!g) {
				free(out);
				out = NULL;
				free(value);
				break;
			}
			out = g;
		}
		memcpy(out + n, piece, plen);
		n += plen;
		free(value);
	}
	if (out)
		out[n] = '\0';
	return out;
}

WINFN(DWORD) k32_ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD size)
{
	char *u = acp_to_utf8(src);
	char *e = u ? expand(u) : NULL;
	DWORD r = 0;

	if (e) {
		/* This one counts the NUL even when it fits. */
		r = copy_out_a(e, dst, size);
		if (dst && r < size)
			r++;
	} else {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	}
	free(e);
	free(u);
	return r;
}

WINFN(DWORD) k32_ExpandEnvironmentStringsW(LPCWSTR src, LPWSTR dst, DWORD size)
{
	char *u = utf16_to_utf8_dup(src);
	char *e = u ? expand(u) : NULL;
	DWORD r = 0;

	if (e) {
		r = copy_out_w(e, dst, size);
		if (dst && r < size)
			r++;
	} else {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	}
	free(e);
	free(u);
	return r;
}

/* Command line and startup info. */

static char cmdline_a[] = "vfw-harness.exe";
static WCHAR cmdline_w[] = { 'v', 'f', 'w', '-', 'h', 'a', 'r', 'n', 'e', 's',
			     's', '.', 'e', 'x', 'e', 0 };

WINFN(LPSTR) k32_GetCommandLineA(void) { return cmdline_a; }
WINFN(LPWSTR) k32_GetCommandLineW(void) { return cmdline_w; }

WINFN(void) k32_GetStartupInfoA(LPSTARTUPINFOA si)
{
	static char empty[1];

	memset(si, 0, sizeof(*si));
	si->cb = sizeof(*si);
	si->lpDesktop = empty;
	si->lpTitle = cmdline_a;
	si->hStdInput = file_std_handle(STD_INPUT_HANDLE);
	si->hStdOutput = file_std_handle(STD_OUTPUT_HANDLE);
	si->hStdError = file_std_handle(STD_ERROR_HANDLE);
}

WINFN(void) k32_GetStartupInfoW(LPSTARTUPINFOW si)
{
	static WCHAR empty[1];

	memset(si, 0, sizeof(*si));
	si->cb = sizeof(*si);
	si->lpDesktop = empty;
	si->lpTitle = cmdline_w;
	si->hStdInput = file_std_handle(STD_INPUT_HANDLE);
	si->hStdOutput = file_std_handle(STD_OUTPUT_HANDLE);
	si->hStdError = file_std_handle(STD_ERROR_HANDLE);
}

/* Debugging and error mode. */

WINFN(void) k32_OutputDebugStringA(LPCSTR s)
{
	if (vfw_trace_enabled && s) {
		char *u = acp_to_utf8(s);
		vfw_trace("debug: %s", u ? u : s);
		free(u);
	}
}

WINFN(void) k32_OutputDebugStringW(LPCWSTR s)
{
	if (vfw_trace_enabled && s) {
		char *u = utf16_to_utf8_dup(s);
		if (u)
			vfw_trace("debug: %s", u);
		free(u);
	}
}

WINFN(BOOL) k32_IsDebuggerPresent(void) { return FALSE; }

static UINT error_mode;

WINFN(UINT) k32_SetErrorMode(UINT mode)
{
	return __atomic_exchange_n(&error_mode, mode, __ATOMIC_SEQ_CST);
}

WINFN(UINT) k32_GetErrorMode(void)
{
	return error_mode;
}

/* FormatMessage. */

static const struct {
	DWORD code;
	const char *text;
} messages[] = {
	{ ERROR_SUCCESS, "The operation completed successfully." },
	{ ERROR_INVALID_FUNCTION, "Incorrect function." },
	{ ERROR_FILE_NOT_FOUND, "The system cannot find the file specified." },
	{ ERROR_PATH_NOT_FOUND, "The system cannot find the path specified." },
	{ ERROR_ACCESS_DENIED, "Access is denied." },
	{ ERROR_INVALID_HANDLE, "The handle is invalid." },
	{ ERROR_NOT_ENOUGH_MEMORY, "Not enough storage is available to process this command." },
	{ ERROR_OUTOFMEMORY, "Not enough storage is available to complete this operation." },
	{ ERROR_INVALID_PARAMETER, "The parameter is incorrect." },
	{ ERROR_CALL_NOT_IMPLEMENTED, "This function is not supported on this system." },
	{ ERROR_INSUFFICIENT_BUFFER, "The data area passed to a system call is too small." },
	{ ERROR_MOD_NOT_FOUND, "The specified module could not be found." },
	{ ERROR_PROC_NOT_FOUND, "The specified procedure could not be found." },
	{ ERROR_ALREADY_EXISTS, "Cannot create a file when that file already exists." },
	{ ERROR_NOT_SUPPORTED, "The request is not supported." },
};

/* Expands %1..%99 inserts, %n, %% and friends from a DWORD argument array. */
static char *format_inserts(const char *fmt, const DWORD_PTR *args,
			    int ignore_inserts, int wide_args)
{
	size_t cap = strlen(fmt) * 2 + 64, n = 0;
	char *out = malloc(cap);

#define PUT(str, len) do { \
		size_t l_ = (len); \
		if (n + l_ + 1 > cap) { \
			cap = (n + l_ + 1) * 2; \
			char *g_ = realloc(out, cap); \
			if (!g_) { free(out); return NULL; } \
			out = g_; \
		} \
		memcpy(out + n, (str), l_); \
		n += l_; \
	} while (0)

	for (const char *p = fmt; out && *p;) {
		if (*p != '%') {
			PUT(p, 1);
			p++;
			continue;
		}
		p++;
		if (*p == '0')
			break;
		if (*p == 'n') {
			PUT("\r\n", 2);
			p++;
		} else if (*p >= '1' && *p <= '9') {
			int idx = *p++ - '0';
			if (*p >= '0' && *p <= '9')
				idx = idx * 10 + (*p++ - '0');
			char spec[32] = "s";
			const char *start = p;
			if (*p == '!') {
				const char *end = strchr(p + 1, '!');
				if (end && end - p - 1 < (int)sizeof(spec) - 1) {
					snprintf(spec, sizeof(spec), "%.*s",
						 (int)(end - p - 1), p + 1);
					p = end + 1;
				}
			}
			if (ignore_inserts || !args) {
				char raw[64];
				int l = snprintf(raw, sizeof(raw), "%%%d", idx);
				PUT(raw, l);
				PUT(start, p - start);
				continue;
			}
			DWORD_PTR v = args[idx - 1];
			char f[40], buf[1024];
			if (!strcmp(spec, "s") || !strcmp(spec, "S")) {
				char *s = wide_args ? utf16_to_utf8_dup((const WCHAR *)v) :
					  acp_to_utf8((const char *)v);
				if (s)
					PUT(s, strlen(s));
				free(s);
				continue;
			}
			snprintf(f, sizeof(f), "%%%s", spec);
			int l = snprintf(buf, sizeof(buf), f, (unsigned)v);
			if (l > 0)
				PUT(buf, (size_t)l < sizeof(buf) ? (size_t)l : sizeof(buf) - 1);
		} else if (*p) {
			/* %%, %. %! and other escapes stand for the character. */
			PUT(p, 1);
			p++;
		}
	}
	if (out)
		out[n] = '\0';
	return out;
#undef PUT
}

static char *format_message(DWORD flags, LPCVOID source, DWORD id,
			    void *args, int wide)
{
	const char *fmt = NULL;
	char *fmt_owned = NULL;

	if (flags & FORMAT_MESSAGE_FROM_STRING) {
		fmt = fmt_owned = wide ? utf16_to_utf8_dup(source) :
				  acp_to_utf8(source);
	} else if (flags & FORMAT_MESSAGE_FROM_SYSTEM) {
		for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); i++)
			if (messages[i].code == id)
				fmt = messages[i].text;
		if (fmt) {
			fmt_owned = malloc(strlen(fmt) + 3);
			if (fmt_owned)
				sprintf(fmt_owned, "%s\r\n", fmt);
			fmt = fmt_owned;
		}
	}
	if (!fmt) {
		win_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
		return NULL;
	}

	/* On x86 a va_list* points at the argument words, like an array. */
	const DWORD_PTR *argv = NULL;
	if (args)
		argv = (flags & FORMAT_MESSAGE_ARGUMENT_ARRAY) ?
		       (const DWORD_PTR *)args : *(DWORD_PTR *const *)args;
	char *r = format_inserts(fmt, argv,
				 (flags & FORMAT_MESSAGE_IGNORE_INSERTS) != 0, wide);
	free(fmt_owned);
	return r;
}

WINFN(DWORD) k32_FormatMessageA(DWORD flags, LPCVOID source, DWORD id,
				DWORD lang, LPSTR buf, DWORD size, void *args)
{
	char *u = format_message(flags, source, id, args, 0);
	char *a = u ? utf8_to_acp_dup(u) : NULL;
	DWORD len;

	free(u);
	if (!a)
		return 0;
	len = strlen(a);
	if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
		HLOCAL p = heap_alloc(heap_process(), 0,
				      len + 1 > size ? len + 1 : size);
		if (!p) {
			free(a);
			return 0;
		}
		memcpy(p, a, len + 1);
		*(LPSTR *)buf = p;
	} else if (len + 1 > size) {
		free(a);
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	} else {
		memcpy(buf, a, len + 1);
	}
	free(a);
	return len;
}

WINFN(DWORD) k32_FormatMessageW(DWORD flags, LPCVOID source, DWORD id,
				DWORD lang, LPWSTR buf, DWORD size, void *args)
{
	char *u = format_message(flags, source, id, args, 1);
	WCHAR *w = u ? utf8_to_utf16_dup(u) : NULL;
	DWORD len;

	free(u);
	if (!w)
		return 0;
	len = wstrlen(w);
	if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
		DWORD n = len + 1 > size ? len + 1 : size;
		HLOCAL p = heap_alloc(heap_process(), 0, n * sizeof(WCHAR));
		if (!p) {
			free(w);
			return 0;
		}
		memcpy(p, w, (len + 1) * sizeof(WCHAR));
		*(LPWSTR *)buf = p;
	} else if (len + 1 > size) {
		free(w);
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	} else {
		memcpy(buf, w, (len + 1) * sizeof(WCHAR));
	}
	free(w);
	return len;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_system_exports[] = {
	E(ExpandEnvironmentStringsA),
	E(ExpandEnvironmentStringsW),
	E(FormatMessageA),
	E(FormatMessageW),
	E(FreeEnvironmentStringsA),
	E(FreeEnvironmentStringsW),
	E(GetCommandLineA),
	E(GetCommandLineW),
	E(GetComputerNameA),
	E(GetComputerNameW),
	{ "GetEnvironmentStrings", 0, (void *)k32_GetEnvironmentStringsA },
	E(GetEnvironmentStringsA),
	E(GetEnvironmentStringsW),
	E(GetEnvironmentVariableA),
	E(GetEnvironmentVariableW),
	E(GetErrorMode),
	E(GetNativeSystemInfo),
	E(GetStartupInfoA),
	E(GetStartupInfoW),
	E(GetSystemDirectoryA),
	E(GetSystemDirectoryW),
	E(GetSystemInfo),
	E(GetSystemWindowsDirectoryA),
	E(GetSystemWindowsDirectoryW),
	E(GetVersion),
	E(GetVersionExA),
	E(GetVersionExW),
	E(GetWindowsDirectoryA),
	E(GetWindowsDirectoryW),
	E(IsDebuggerPresent),
	E(IsProcessorFeaturePresent),
	E(OutputDebugStringA),
	E(OutputDebugStringW),
	E(SetEnvironmentVariableA),
	E(SetEnvironmentVariableW),
	E(SetErrorMode),
	BUILTIN_END
};
