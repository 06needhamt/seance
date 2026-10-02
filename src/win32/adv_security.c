/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * advapi32's security functions, for one pretend machine with one user:
 * the Unix user, as GetUserName names them, an administrator on Windows
 * XP. Code asking who it runs as, whether it is an administrator, or for
 * a privilege gets the answers such a user gets; nothing is enforced, as
 * the harness has no access control of its own.
 *
 * - Tokens: the process has one (OpenProcessToken), a thread one only
 *   while impersonating (ImpersonateSelf). The user is
 *   <computer>\<user> (S-1-5-21-<machine>-1000), in None, Everyone,
 *   Administrators (the default owner), Users, INTERACTIVE, Authenticated
 *   Users and LOCAL, with XP's administrator privileges, of which
 *   SeChangeNotify, SeImpersonate and SeCreateGlobal start enabled.
 *   AdjustTokenPrivileges changes them. The token reads as elevated, at
 *   high integrity.
 * - SIDs, well-known SIDs, their string forms, and account names for the
 *   well-known, builtin and local accounts (LookupAccountSid/Name).
 * - Privileges by name and LUID, with Windows' LUIDs.
 * - ACLs and security descriptors, absolute and self-relative, built and
 *   read as Windows lays them out.
 * - Files and registry keys (GetNamedSecurityInfo, GetFileSecurity, ...)
 *   report one descriptor (sec_default_descriptor), and setting one
 *   succeeds without changing anything.
 * - CryptAcquireContext / CryptGenRandom / RtlGenRandom (SystemFunction036),
 *   from the kernel's random numbers; no keys or hashes.
 */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <unistd.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/reg.h"
#include "win32/security.h"

extern const struct builtin_dll builtin_kernel32;

#define PSEUDO_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_THREAD  ((HANDLE)(LONG_PTR)-2)

/* SIDs. */

struct sid {
	BYTE rev, n;
	BYTE auth[6];
	DWORD sub[SID_MAX_SUB_AUTHORITIES];
};

/* The machine's own domain, S-1-5-21-a-b-c: fixed, there being one. */
static const DWORD machine[3] = { 1004336348, 1177238915, 682003330 };

#define USER_RID 1000

static struct sid mksid(BYTE auth, int n, ...)
{
	struct sid s = { SID_REVISION, (BYTE)n, { 0, 0, 0, 0, 0, auth }, { 0 } };
	__builtin_va_list ap;

	__builtin_va_start(ap, n);
	for (int i = 0; i < n; i++)
		s.sub[i] = __builtin_va_arg(ap, DWORD);
	__builtin_va_end(ap);
	return s;
}

/* A SID in the machine's domain (rid < 0: the domain itself). */
static struct sid machine_sid(long rid)
{
	struct sid s = mksid(5, 4, SECURITY_NT_NON_UNIQUE, machine[0],
			     machine[1], machine[2]);
	if (rid >= 0)
		s.sub[s.n++] = (DWORD)rid;
	return s;
}

static DWORD sid_size(const void *p)
{
	return 8 + 4 * ((const SID *)p)->SubAuthorityCount;
}

static int sid_ok(const void *p)
{
	const SID *s = p;
	return s && s->Revision == SID_REVISION &&
	       s->SubAuthorityCount <= SID_MAX_SUB_AUTHORITIES;
}

static int sid_eq(const void *a, const void *b)
{
	return sid_ok(a) && sid_ok(b) && sid_size(a) == sid_size(b) &&
	       !memcmp(a, b, sid_size(a));
}

/* Names. */

static const char *user_name(void)
{
	const char *u = getenv("USER");
	return u && *u ? u : "user";
}

static char computer[64];
static pthread_once_t computer_once = PTHREAD_ONCE_INIT;

static void computer_init(void)
{
	BOOL (WINAPI *get)(LPSTR, LPDWORD) =
		builtin_export(&builtin_kernel32, "GetComputerNameA");
	DWORD n = sizeof(computer);

	if (!get || !get(computer, &n))
		strcpy(computer, "SEANCE");
}

/* As GetComputerName reports it. */
static const char *computer_name(void)
{
	pthread_once(&computer_once, computer_init);
	return computer;
}

/* The accounts LookupAccountSid and LookupAccountName know. */
struct account {
	const char *domain;	/* NULL: the computer's name */
	const char *name;	/* NULL: the computer's name, or the user's */
	SID_NAME_USE use;
	BYTE auth;
	BYTE n;
	DWORD sub[2];
	long rid;		/* >= -1: in the machine's domain */
};

#define WK SidTypeWellKnownGroup
#define NOT_LOCAL -2

static const struct account accounts[] = {
	{ "", "NULL SID", WK, 0, 1, { 0 }, NOT_LOCAL },
	{ "", "Everyone", WK, 1, 1, { 0 }, NOT_LOCAL },
	{ "", "LOCAL", WK, 2, 1, { 0 }, NOT_LOCAL },
	{ "", "CREATOR OWNER", WK, 3, 1, { 0 }, NOT_LOCAL },
	{ "", "CREATOR GROUP", WK, 3, 1, { 1 }, NOT_LOCAL },
	{ "NT AUTHORITY", "DIALUP", WK, 5, 1, { 1 }, NOT_LOCAL },
	{ "NT AUTHORITY", "NETWORK", WK, 5, 1, { 2 }, NOT_LOCAL },
	{ "NT AUTHORITY", "BATCH", WK, 5, 1, { 3 }, NOT_LOCAL },
	{ "NT AUTHORITY", "INTERACTIVE", WK, 5, 1, { 4 }, NOT_LOCAL },
	{ "NT AUTHORITY", "SERVICE", WK, 5, 1, { 6 }, NOT_LOCAL },
	{ "NT AUTHORITY", "ANONYMOUS LOGON", WK, 5, 1, { 7 }, NOT_LOCAL },
	{ "NT AUTHORITY", "SELF", WK, 5, 1, { 10 }, NOT_LOCAL },
	{ "NT AUTHORITY", "Authenticated Users", WK, 5, 1, { 11 }, NOT_LOCAL },
	{ "NT AUTHORITY", "SYSTEM", WK, 5, 1, { 18 }, NOT_LOCAL },
	{ "NT AUTHORITY", "LOCAL SERVICE", WK, 5, 1, { 19 }, NOT_LOCAL },
	{ "NT AUTHORITY", "NETWORK SERVICE", WK, 5, 1, { 20 }, NOT_LOCAL },
	{ "BUILTIN", "BUILTIN", SidTypeDomain, 5, 1, { 32 }, NOT_LOCAL },
	{ "BUILTIN", "Administrators", SidTypeAlias, 5, 2, { 32, 544 }, NOT_LOCAL },
	{ "BUILTIN", "Users", SidTypeAlias, 5, 2, { 32, 545 }, NOT_LOCAL },
	{ "BUILTIN", "Guests", SidTypeAlias, 5, 2, { 32, 546 }, NOT_LOCAL },
	{ "BUILTIN", "Power Users", SidTypeAlias, 5, 2, { 32, 547 }, NOT_LOCAL },
	{ "Mandatory Label", "Low Mandatory Level", SidTypeLabel, 16, 1,
	  { 0x1000 }, NOT_LOCAL },
	{ "Mandatory Label", "Medium Mandatory Level", SidTypeLabel, 16, 1,
	  { 0x2000 }, NOT_LOCAL },
	{ "Mandatory Label", "High Mandatory Level", SidTypeLabel, 16, 1,
	  { 0x3000 }, NOT_LOCAL },
	{ "Mandatory Label", "System Mandatory Level", SidTypeLabel, 16, 1,
	  { 0x4000 }, NOT_LOCAL },
	{ NULL, NULL, SidTypeUser, 0, 0, { 0 }, USER_RID },
	{ NULL, "Administrator", SidTypeUser, 0, 0, { 0 }, DOMAIN_USER_RID_ADMIN },
	{ NULL, "Guest", SidTypeUser, 0, 0, { 0 }, DOMAIN_USER_RID_GUEST },
	{ NULL, "None", SidTypeGroup, 0, 0, { 0 }, DOMAIN_GROUP_RID_USERS },
	{ NULL, NULL, SidTypeDomain, 0, 0, { 0 }, -1 },
};

static struct sid account_sid(const struct account *a)
{
	if (a->rid != NOT_LOCAL)
		return machine_sid(a->rid);
	struct sid s = mksid(a->auth, 0);
	s.n = a->n;
	memcpy(s.sub, a->sub, sizeof(a->sub));
	return s;
}

static const char *account_domain(const struct account *a)
{
	return a->domain ? a->domain : computer_name();
}

static const char *account_name(const struct account *a)
{
	if (a->name)
		return a->name;
	return a->rid == -1 ? computer_name() : user_name();
}

/* The SIDs a token is built from. */
static struct sid user_sid(void) { return machine_sid(USER_RID); }

/* The logon session's SID, S-1-5-5-0-<id>. */
static struct sid logon_sid(void) { return mksid(5, 3, 5, 0, 0x3e7a1); }

/* The token's groups, in the order XP lists them. */
struct group {
	struct sid sid;
	DWORD attrs;
};

#define GRP (SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED)

static size_t token_groups(struct group g[8])
{
	size_t n = 0;

	g[n++] = (struct group){ machine_sid(DOMAIN_GROUP_RID_USERS), GRP };
	g[n++] = (struct group){ mksid(1, 1, 0), GRP };
	g[n++] = (struct group){ mksid(5, 2, 32, 544), GRP | SE_GROUP_OWNER };
	g[n++] = (struct group){ mksid(5, 2, 32, 545), GRP };
	g[n++] = (struct group){ mksid(5, 1, 4), GRP };
	g[n++] = (struct group){ mksid(5, 1, 11), GRP };
	g[n++] = (struct group){ logon_sid(), GRP | SE_GROUP_LOGON_ID };
	g[n++] = (struct group){ mksid(2, 1, 0), GRP };
	return n;
}

/* Privileges, by LUID as Windows numbers them. */

#define NPRIV 36

static const char *const privileges[NPRIV] = {
	[2] = "SeCreateTokenPrivilege",
	[3] = "SeAssignPrimaryTokenPrivilege",
	[4] = "SeLockMemoryPrivilege",
	[5] = "SeIncreaseQuotaPrivilege",
	[6] = "SeMachineAccountPrivilege",
	[7] = "SeTcbPrivilege",
	[8] = "SeSecurityPrivilege",
	[9] = "SeTakeOwnershipPrivilege",
	[10] = "SeLoadDriverPrivilege",
	[11] = "SeSystemProfilePrivilege",
	[12] = "SeSystemtimePrivilege",
	[13] = "SeProfileSingleProcessPrivilege",
	[14] = "SeIncreaseBasePriorityPrivilege",
	[15] = "SeCreatePagefilePrivilege",
	[16] = "SeCreatePermanentPrivilege",
	[17] = "SeBackupPrivilege",
	[18] = "SeRestorePrivilege",
	[19] = "SeShutdownPrivilege",
	[20] = "SeDebugPrivilege",
	[21] = "SeAuditPrivilege",
	[22] = "SeSystemEnvironmentPrivilege",
	[23] = "SeChangeNotifyPrivilege",
	[24] = "SeRemoteShutdownPrivilege",
	[25] = "SeUndockPrivilege",
	[26] = "SeSyncAgentPrivilege",
	[27] = "SeEnableDelegationPrivilege",
	[28] = "SeManageVolumePrivilege",
	[29] = "SeImpersonatePrivilege",
	[30] = "SeCreateGlobalPrivilege",
	[31] = "SeTrustedCredManAccessPrivilege",
	[32] = "SeRelabelPrivilege",
	[33] = "SeIncreaseWorkingSetPrivilege",
	[34] = "SeTimeZonePrivilege",
	[35] = "SeCreateSymbolicLinkPrivilege",
};

/* An XP administrator's, and which start enabled. */
static const BYTE held_privileges[] = {
	5, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 22, 23, 24, 25, 28,
	29, 30,
};

static int enabled_by_default(int i)
{
	return i == 23 || i == 29 || i == 30;
}

/* Tokens. */

struct token {
	struct kobject obj;
	TOKEN_TYPE type;
	SECURITY_IMPERSONATION_LEVEL level;
	LUID id, modified;
	BYTE held[NPRIV];
	DWORD attrs[NPRIV];
};

static pthread_mutex_t token_lock = PTHREAD_MUTEX_INITIALIZER;
static struct token *process_token;
static _Thread_local struct token *thread_token;
static DWORD next_luid = 0x10000;

static LUID new_luid(void)
{
	LUID l = { __atomic_add_fetch(&next_luid, 1, __ATOMIC_RELAXED), 0 };
	return l;
}

static void token_destroy(struct kobject *o)
{
	free(o);
}

static struct token *token_new(const struct token *from, TOKEN_TYPE type,
			       SECURITY_IMPERSONATION_LEVEL level)
{
	struct token *t = calloc(1, sizeof(*t));

	if (!t)
		return NULL;
	kobj_init(&t->obj, KOBJ_TOKEN, token_destroy);
	t->type = type;
	t->level = level;
	t->id = t->modified = new_luid();
	if (from) {
		pthread_mutex_lock(&token_lock);
		memcpy(t->held, from->held, sizeof(t->held));
		memcpy(t->attrs, from->attrs, sizeof(t->attrs));
		pthread_mutex_unlock(&token_lock);
	} else {
		for (size_t i = 0; i < sizeof(held_privileges); i++) {
			int p = held_privileges[i];
			t->held[p] = 1;
			t->attrs[p] = enabled_by_default(p) ?
				      SE_PRIVILEGE_ENABLED |
				      SE_PRIVILEGE_ENABLED_BY_DEFAULT : 0;
		}
	}
	return t;
}

static pthread_once_t process_once = PTHREAD_ONCE_INIT;

static void process_init(void)
{
	process_token = token_new(NULL, TokenPrimary, SecurityAnonymous);
}

static struct token *the_process_token(void)
{
	pthread_once(&process_once, process_init);
	return process_token;
}

/* The token behind h, with a reference; NULL with the last error set. */
static struct token *token_ref(HANDLE h)
{
	struct kobject *o = handle_ref(h, KOBJ_TOKEN);

	if (!o)
		win_set_error(ERROR_INVALID_HANDLE);
	return (struct token *)o;
}

/* A handle to t, taking over one reference (which a failure drops). */
static BOOL give_handle(struct token *t, PHANDLE out)
{
	HANDLE h = handle_new(&t->obj);

	if (!h)
		return FALSE;	/* handle_new set the error */
	*out = h;
	return TRUE;
}

WINFN(BOOL) adv_OpenProcessToken(HANDLE process, DWORD access, PHANDLE out)
{
	struct token *t = the_process_token();

	if (!out) {
		win_set_error(ERROR_NOACCESS);
		return FALSE;
	}
	if (!t) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	kobj_ref(&t->obj);
	return give_handle(t, out);
}

/* Only the calling thread's impersonation token is known. */
WINFN(BOOL) adv_OpenThreadToken(HANDLE thread, DWORD access, BOOL as_self,
				PHANDLE out)
{
	if (!out) {
		win_set_error(ERROR_NOACCESS);
		return FALSE;
	}
	*out = NULL;
	if (!thread_token) {
		win_set_error(ERROR_NO_TOKEN);
		return FALSE;
	}
	kobj_ref(&thread_token->obj);
	return give_handle(thread_token, out);
}

static void set_thread_token(struct token *t)
{
	if (thread_token)
		kobj_unref(&thread_token->obj);
	thread_token = t;
}

WINFN(BOOL) adv_ImpersonateSelf(SECURITY_IMPERSONATION_LEVEL level)
{
	struct token *p = the_process_token();
	struct token *t = p ? token_new(p, TokenImpersonation, level) : NULL;

	if (!t) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	set_thread_token(t);
	return TRUE;
}

WINFN(BOOL) adv_RevertToSelf(void)
{
	set_thread_token(NULL);
	return TRUE;
}

WINFN(BOOL) adv_SetThreadToken(PHANDLE thread, HANDLE token)
{
	if (!token) {
		set_thread_token(NULL);
		return TRUE;
	}
	struct token *t = token_ref(token);
	if (!t)
		return FALSE;
	if (t->type != TokenImpersonation) {
		kobj_unref(&t->obj);
		win_set_error(ERROR_BAD_TOKEN_TYPE);
		return FALSE;
	}
	set_thread_token(t);
	return TRUE;
}

WINFN(BOOL) adv_DuplicateTokenEx(HANDLE from, DWORD access,
				 LPSECURITY_ATTRIBUTES sa,
				 SECURITY_IMPERSONATION_LEVEL level,
				 TOKEN_TYPE type, PHANDLE out)
{
	if (!out || (type != TokenPrimary && type != TokenImpersonation)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	struct token *f = token_ref(from);
	if (!f)
		return FALSE;
	struct token *t = token_new(f, type,
				    type == TokenPrimary ? SecurityAnonymous :
				    level);
	kobj_unref(&f->obj);
	if (!t) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	return give_handle(t, out);
}

WINFN(BOOL) adv_DuplicateToken(HANDLE from, SECURITY_IMPERSONATION_LEVEL level,
			       PHANDLE out)
{
	return adv_DuplicateTokenEx(from, 0, NULL, level, TokenImpersonation,
				    out);
}

/* GetTokenInformation: a structure, then the SIDs it points to. */
struct out {
	BYTE *base;	/* NULL while only sizing */
	DWORD used;
};

static void *reserve(struct out *o, DWORD n)
{
	DWORD at = (o->used + 3) & ~3u;

	o->used = at + n;
	return o->base ? o->base + at : NULL;
}

static PSID put_sid(struct out *o, const struct sid *s)
{
	void *p = reserve(o, sid_size(s));

	if (p)
		memcpy(p, s, sid_size(s));
	return p;
}

/* Builds class c of t at o; FALSE if the class isn't known. */
static BOOL token_info(struct token *t, TOKEN_INFORMATION_CLASS c,
		       struct out *o)
{
	struct group g[8];
	size_t ng = token_groups(g);
	struct sid s;

	switch (c) {
	case TokenUser: {
		TOKEN_USER *u = reserve(o, sizeof(*u));
		s = user_sid();
		PSID p = put_sid(o, &s);
		if (u) {
			u->User.Sid = p;
			u->User.Attributes = 0;
		}
		return TRUE;
	}
	case TokenGroups: {
		TOKEN_GROUPS *tg = reserve(o, offsetof(TOKEN_GROUPS, Groups) +
					   ng * sizeof(SID_AND_ATTRIBUTES));
		if (tg)
			tg->GroupCount = ng;
		for (size_t i = 0; i < ng; i++) {
			PSID p = put_sid(o, &g[i].sid);
			if (tg) {
				tg->Groups[i].Sid = p;
				tg->Groups[i].Attributes = g[i].attrs;
			}
		}
		return TRUE;
	}
	case TokenPrivileges: {
		DWORD n = 0;
		for (int i = 0; i < NPRIV; i++)
			n += t->held[i];
		TOKEN_PRIVILEGES *tp = reserve(o,
			offsetof(TOKEN_PRIVILEGES, Privileges) +
			n * sizeof(LUID_AND_ATTRIBUTES));
		if (tp) {
			tp->PrivilegeCount = 0;
			pthread_mutex_lock(&token_lock);
			for (int i = 0; i < NPRIV; i++)
				if (t->held[i]) {
					LUID_AND_ATTRIBUTES *la =
						&tp->Privileges[tp->PrivilegeCount++];
					la->Luid.LowPart = i;
					la->Luid.HighPart = 0;
					la->Attributes = t->attrs[i];
				}
			pthread_mutex_unlock(&token_lock);
		}
		return TRUE;
	}
	case TokenOwner: {
		TOKEN_OWNER *ow = reserve(o, sizeof(*ow));
		s = mksid(5, 2, 32, 544);
		PSID p = put_sid(o, &s);
		if (ow)
			ow->Owner = p;
		return TRUE;
	}
	case TokenPrimaryGroup: {
		TOKEN_PRIMARY_GROUP *pg = reserve(o, sizeof(*pg));
		s = machine_sid(DOMAIN_GROUP_RID_USERS);
		PSID p = put_sid(o, &s);
		if (pg)
			pg->PrimaryGroup = p;
		return TRUE;
	}
	case TokenType: {
		TOKEN_TYPE *tt = reserve(o, sizeof(*tt));
		if (tt)
			*tt = t->type;
		return TRUE;
	}
	case TokenImpersonationLevel: {
		if (t->type != TokenImpersonation)
			return FALSE;
		SECURITY_IMPERSONATION_LEVEL *l = reserve(o, sizeof(*l));
		if (l)
			*l = t->level;
		return TRUE;
	}
	case TokenStatistics: {
		TOKEN_STATISTICS *st = reserve(o, sizeof(*st));
		if (st) {
			DWORD np = 0;
			for (int i = 0; i < NPRIV; i++)
				np += t->held[i];
			memset(st, 0, sizeof(*st));
			st->TokenId = t->id;
			st->AuthenticationId.LowPart = 0x3e7a1;
			st->ExpirationTime.QuadPart = 0x7fffffffffffffffLL;
			st->TokenType = t->type;
			st->ImpersonationLevel = t->level;
			st->DynamicCharged = 500;
			st->DynamicAvailable = 500;
			st->GroupCount = ng;
			st->PrivilegeCount = np;
			st->ModifiedId = t->modified;
		}
		return TRUE;
	}
	case TokenSessionId: {
		DWORD *id = reserve(o, sizeof(*id));
		if (id)
			*id = 0;
		return TRUE;
	}
	case TokenElevationType: {
		TOKEN_ELEVATION_TYPE *e = reserve(o, sizeof(*e));
		if (e)
			*e = TokenElevationTypeDefault;
		return TRUE;
	}
	case TokenElevation: {
		TOKEN_ELEVATION *e = reserve(o, sizeof(*e));
		if (e)
			e->TokenIsElevated = 1;
		return TRUE;
	}
	case TokenIntegrityLevel: {
		TOKEN_MANDATORY_LABEL *l = reserve(o, sizeof(*l));
		s = mksid(16, 1, SECURITY_MANDATORY_HIGH_RID);
		PSID p = put_sid(o, &s);
		if (l) {
			l->Label.Sid = p;
			l->Label.Attributes = SE_GROUP_INTEGRITY |
					      SE_GROUP_INTEGRITY_ENABLED;
		}
		return TRUE;
	}
	default:
		return FALSE;
	}
}

WINFN(BOOL) adv_GetTokenInformation(HANDLE h, TOKEN_INFORMATION_CLASS c,
				    LPVOID buf, DWORD len, PDWORD ret)
{
	if (!ret) {
		win_set_error(ERROR_NOACCESS);
		return FALSE;
	}
	struct token *t = token_ref(h);
	if (!t)
		return FALSE;
	struct out size = { NULL, 0 };
	BOOL ok = token_info(t, c, &size);
	if (!ok) {
		TRACE("GetTokenInformation: class %d not supported", c);
		win_set_error(ERROR_INVALID_PARAMETER);
	} else {
		*ret = size.used;
		if (!buf || len < size.used) {
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			ok = FALSE;
		} else {
			struct out o = { buf, 0 };
			token_info(t, c, &o);
		}
	}
	kobj_unref(&t->obj);
	return ok;
}

/* Setting a token's owner, group, default DACL or session is accepted. */
WINFN(BOOL) adv_SetTokenInformation(HANDLE h, TOKEN_INFORMATION_CLASS c,
				    LPVOID buf, DWORD len)
{
	struct token *t = token_ref(h);

	if (!t)
		return FALSE;
	kobj_unref(&t->obj);
	switch (c) {
	case TokenOwner:
	case TokenPrimaryGroup:
	case TokenDefaultDacl:
	case TokenSessionId:
		return TRUE;
	default:
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
}

static int luid_index(const LUID *l)
{
	return !l->HighPart && l->LowPart < NPRIV && privileges[l->LowPart] ?
	       (int)l->LowPart : -1;
}

WINFN(BOOL) adv_AdjustTokenPrivileges(HANDLE h, BOOL disable_all,
				      PTOKEN_PRIVILEGES privs, DWORD buf_len,
				      PTOKEN_PRIVILEGES prev, PDWORD ret_len)
{
	if (!disable_all && !privs) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	struct token *t = token_ref(h);
	if (!t)
		return FALSE;

	/* What each one becomes, and which of them change. */
	DWORD want[NPRIV], changed = 0, missing = 0;
	pthread_mutex_lock(&token_lock);
	memcpy(want, t->attrs, sizeof(want));
	if (disable_all) {
		for (int i = 0; i < NPRIV; i++)
			want[i] &= ~SE_PRIVILEGE_ENABLED;
	} else {
		for (DWORD k = 0; k < privs->PrivilegeCount; k++) {
			int i = luid_index(&privs->Privileges[k].Luid);
			if (i < 0 || !t->held[i]) {
				missing++;
				continue;
			}
			DWORD a = privs->Privileges[k].Attributes;
			if (a & SE_PRIVILEGE_ENABLED)
				want[i] |= SE_PRIVILEGE_ENABLED;
			else
				want[i] &= ~SE_PRIVILEGE_ENABLED;
		}
	}
	for (int i = 0; i < NPRIV; i++)
		changed += t->held[i] && want[i] != t->attrs[i];

	DWORD need = offsetof(TOKEN_PRIVILEGES, Privileges) +
		     changed * sizeof(LUID_AND_ATTRIBUTES);
	if (prev && buf_len < need) {
		pthread_mutex_unlock(&token_lock);
		kobj_unref(&t->obj);
		if (ret_len)
			*ret_len = need;
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (prev)
		prev->PrivilegeCount = 0;
	for (int i = 0; i < NPRIV; i++) {
		if (!t->held[i] || want[i] == t->attrs[i])
			continue;
		if (prev) {
			LUID_AND_ATTRIBUTES *la =
				&prev->Privileges[prev->PrivilegeCount++];
			la->Luid.LowPart = i;
			la->Luid.HighPart = 0;
			la->Attributes = t->attrs[i];
		}
		t->attrs[i] = want[i];
	}
	if (changed)
		t->modified = new_luid();
	pthread_mutex_unlock(&token_lock);
	kobj_unref(&t->obj);
	if (ret_len && prev)
		*ret_len = need;
	win_set_error(missing ? ERROR_NOT_ALL_ASSIGNED : ERROR_SUCCESS);
	return TRUE;
}

WINFN(BOOL) adv_PrivilegeCheck(HANDLE h, PPRIVILEGE_SET set, LPBOOL result)
{
	if (!set || !result) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	struct token *t = token_ref(h);
	if (!t)
		return FALSE;
	DWORD have = 0;
	pthread_mutex_lock(&token_lock);
	for (DWORD k = 0; k < set->PrivilegeCount; k++) {
		int i = luid_index(&set->Privilege[k].Luid);
		int on = i >= 0 && t->held[i] &&
			 (t->attrs[i] & SE_PRIVILEGE_ENABLED);
		if (on) {
			set->Privilege[k].Attributes |=
				SE_PRIVILEGE_USED_FOR_ACCESS;
			have++;
		}
	}
	pthread_mutex_unlock(&token_lock);
	kobj_unref(&t->obj);
	*result = set->Control & PRIVILEGE_SET_ALL_NECESSARY ?
		  have == set->PrivilegeCount : have > 0;
	return TRUE;
}

WINFN(BOOL) adv_CheckTokenMembership(HANDLE h, PSID sid, PBOOL member)
{
	struct token *t;

	if (!member || !sid_ok(sid)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*member = FALSE;
	if (h) {
		if (!(t = token_ref(h)))
			return FALSE;
		TOKEN_TYPE type = t->type;
		kobj_unref(&t->obj);
		if (type != TokenImpersonation) {
			win_set_error(ERROR_NO_IMPERSONATION_TOKEN);
			return FALSE;
		}
	}
	struct sid u = user_sid();
	struct group g[8];
	size_t ng = token_groups(g);
	if (sid_eq(sid, &u))
		*member = TRUE;
	for (size_t i = 0; i < ng && !*member; i++)
		if (sid_eq(sid, &g[i].sid) && (g[i].attrs & SE_GROUP_ENABLED) &&
		    !(g[i].attrs & SE_GROUP_USE_FOR_DENY_ONLY))
			*member = TRUE;
	return TRUE;
}

/* Privilege names. */

static BOOL priv_value(const char *name, PLUID luid)
{
	if (!name || !luid) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	for (int i = 0; i < NPRIV; i++)
		if (privileges[i] && !strcasecmp(privileges[i], name)) {
			luid->LowPart = i;
			luid->HighPart = 0;
			return TRUE;
		}
	win_set_error(ERROR_NO_SUCH_PRIVILEGE);
	return FALSE;
}

WINFN(BOOL) adv_LookupPrivilegeValueA(LPCSTR system, LPCSTR name, PLUID luid)
{
	char *n = name ? acp_to_utf8(name) : NULL;
	BOOL r = priv_value(n, luid);
	free(n);
	return r;
}

WINFN(BOOL) adv_LookupPrivilegeValueW(LPCWSTR system, LPCWSTR name, PLUID luid)
{
	char *n = name ? utf16_to_utf8_dup(name) : NULL;
	BOOL r = priv_value(n, luid);
	free(n);
	return r;
}

/*
 * Copies UTF-8 s out as ANSI or UTF-16 into a buffer of *cch characters:
 * TRUE with *cch its length if it fits with its NUL, else FALSE with
 * *cch the size needed, NUL included.
 */
static BOOL put_name(const char *s, int wide, void *buf, LPDWORD cch)
{
	void *conv = wide ? (void *)utf8_to_utf16_dup(s) : utf8_to_acp_dup(s);
	DWORD len;

	if (!conv)
		return FALSE;
	len = wide ? wstrlen(conv) : strlen(conv);
	BOOL ok = buf && *cch > len;
	if (ok) {
		memcpy(buf, conv, (len + 1) * (wide ? sizeof(WCHAR) : 1));
		*cch = len;
	} else {
		*cch = len + 1;
	}
	free(conv);
	return ok;
}

static BOOL priv_name(PLUID luid, int wide, void *buf, LPDWORD cch)
{
	int i = luid && cch ? luid_index(luid) : -1;

	if (!cch || !luid) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (i < 0) {
		win_set_error(ERROR_NO_SUCH_PRIVILEGE);
		return FALSE;
	}
	if (!put_name(privileges[i], wide, buf, cch)) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	return TRUE;
}

WINFN(BOOL) adv_LookupPrivilegeNameA(LPCSTR system, PLUID luid, LPSTR name,
				     LPDWORD cch)
{
	return priv_name(luid, 0, name, cch);
}

WINFN(BOOL) adv_LookupPrivilegeNameW(LPCWSTR system, PLUID luid, LPWSTR name,
				     LPDWORD cch)
{
	return priv_name(luid, 1, name, cch);
}

/* SID functions. */

WINFN(BOOL) adv_AllocateAndInitializeSid(PSID_IDENTIFIER_AUTHORITY auth,
					 BYTE n, DWORD s0, DWORD s1, DWORD s2,
					 DWORD s3, DWORD s4, DWORD s5,
					 DWORD s6, DWORD s7, PSID *out)
{
	const DWORD sub[8] = { s0, s1, s2, s3, s4, s5, s6, s7 };

	if (!auth || !out || n > 8) {
		win_set_error(ERROR_INVALID_SID);
		return FALSE;
	}
	SID *s = heap_alloc(heap_process(), 0, 8 + 4 * n);
	if (!s) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	s->Revision = SID_REVISION;
	s->SubAuthorityCount = n;
	s->IdentifierAuthority = *auth;
	memcpy(s->SubAuthority, sub, 4 * n);
	*out = s;
	return TRUE;
}

WINFN(PVOID) adv_FreeSid(PSID sid)
{
	if (sid)
		heap_free(heap_process(), 0, sid);
	return NULL;
}

WINFN(BOOL) adv_InitializeSid(PSID sid, PSID_IDENTIFIER_AUTHORITY auth, BYTE n)
{
	if (!sid || !auth || n > SID_MAX_SUB_AUTHORITIES) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	SID *s = sid;
	s->Revision = SID_REVISION;
	s->SubAuthorityCount = n;
	s->IdentifierAuthority = *auth;
	memset(s->SubAuthority, 0, 4 * n);
	return TRUE;
}

WINFN(DWORD) adv_GetSidLengthRequired(UCHAR n)
{
	return 8 + 4 * (DWORD)n;
}

WINFN(DWORD) adv_GetLengthSid(PSID sid)
{
	return sid_ok(sid) ? sid_size(sid) : 0;
}

WINFN(BOOL) adv_IsValidSid(PSID sid)
{
	return sid_ok(sid);
}

WINFN(BOOL) adv_EqualSid(PSID a, PSID b)
{
	win_set_error(ERROR_SUCCESS);
	return sid_eq(a, b);
}

/* Equal but for the last sub-authority. */
WINFN(BOOL) adv_EqualPrefixSid(PSID a, PSID b)
{
	const SID *x = a, *y = b;

	if (!sid_ok(a) || !sid_ok(b) ||
	    x->SubAuthorityCount != y->SubAuthorityCount)
		return FALSE;
	DWORD n = x->SubAuthorityCount ? x->SubAuthorityCount - 1 : 0;
	return !memcmp(x, y, 8) && !memcmp(x->SubAuthority, y->SubAuthority,
					   4 * n);
}

WINFN(BOOL) adv_CopySid(DWORD len, PSID dst, PSID src)
{
	if (!sid_ok(src) || !dst) {
		win_set_error(ERROR_INVALID_SID);
		return FALSE;
	}
	if (len < sid_size(src)) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memmove(dst, src, sid_size(src));
	return TRUE;
}

WINFN(PSID_IDENTIFIER_AUTHORITY) adv_GetSidIdentifierAuthority(PSID sid)
{
	win_set_error(ERROR_SUCCESS);
	return &((SID *)sid)->IdentifierAuthority;
}

WINFN(PDWORD) adv_GetSidSubAuthority(PSID sid, DWORD i)
{
	win_set_error(ERROR_SUCCESS);
	return &((SID *)sid)->SubAuthority[i];
}

WINFN(PUCHAR) adv_GetSidSubAuthorityCount(PSID sid)
{
	win_set_error(ERROR_SUCCESS);
	return &((SID *)sid)->SubAuthorityCount;
}

/* Well-known SIDs. */

static const struct {
	WELL_KNOWN_SID_TYPE type;
	BYTE auth, n;
	DWORD sub[2];
	long rid;	/* >= 0: in the domain given (or the machine's) */
} well_known[] = {
	{ WinNullSid, 0, 1, { 0 }, -1 },
	{ WinWorldSid, 1, 1, { 0 }, -1 },
	{ WinLocalSid, 2, 1, { 0 }, -1 },
	{ WinCreatorOwnerSid, 3, 1, { 0 }, -1 },
	{ WinCreatorGroupSid, 3, 1, { 1 }, -1 },
	{ WinCreatorOwnerServerSid, 3, 1, { 2 }, -1 },
	{ WinCreatorGroupServerSid, 3, 1, { 3 }, -1 },
	{ WinNtAuthoritySid, 5, 0, { 0 }, -1 },
	{ WinDialupSid, 5, 1, { 1 }, -1 },
	{ WinNetworkSid, 5, 1, { 2 }, -1 },
	{ WinBatchSid, 5, 1, { 3 }, -1 },
	{ WinInteractiveSid, 5, 1, { 4 }, -1 },
	{ WinServiceSid, 5, 1, { 6 }, -1 },
	{ WinAnonymousSid, 5, 1, { 7 }, -1 },
	{ WinProxySid, 5, 1, { 8 }, -1 },
	{ WinEnterpriseControllersSid, 5, 1, { 9 }, -1 },
	{ WinSelfSid, 5, 1, { 10 }, -1 },
	{ WinAuthenticatedUserSid, 5, 1, { 11 }, -1 },
	{ WinRestrictedCodeSid, 5, 1, { 12 }, -1 },
	{ WinTerminalServerSid, 5, 1, { 13 }, -1 },
	{ WinRemoteLogonIdSid, 5, 1, { 14 }, -1 },
	{ WinLocalSystemSid, 5, 1, { 18 }, -1 },
	{ WinLocalServiceSid, 5, 1, { 19 }, -1 },
	{ WinNetworkServiceSid, 5, 1, { 20 }, -1 },
	{ WinBuiltinDomainSid, 5, 1, { 32 }, -1 },
	{ WinBuiltinAdministratorsSid, 5, 2, { 32, 544 }, -1 },
	{ WinBuiltinUsersSid, 5, 2, { 32, 545 }, -1 },
	{ WinBuiltinGuestsSid, 5, 2, { 32, 546 }, -1 },
	{ WinBuiltinPowerUsersSid, 5, 2, { 32, 547 }, -1 },
	{ WinMediumLabelSid, 16, 1, { 0x2000 }, -1 },
	{ WinHighLabelSid, 16, 1, { 0x3000 }, -1 },
	{ WinAccountAdministratorSid, 0, 0, { 0 }, DOMAIN_USER_RID_ADMIN },
	{ WinAccountGuestSid, 0, 0, { 0 }, DOMAIN_USER_RID_GUEST },
	{ WinAccountDomainUsersSid, 0, 0, { 0 }, DOMAIN_GROUP_RID_USERS },
};

/* Well-known SID type in domain (a domain SID, or NULL); 0 or -1. */
static int well_known_sid(WELL_KNOWN_SID_TYPE type, const void *domain,
			  struct sid *out)
{
	for (size_t i = 0; i < sizeof(well_known) / sizeof(well_known[0]); i++) {
		if (well_known[i].type != type)
			continue;
		if (well_known[i].rid < 0) {
			*out = mksid(well_known[i].auth, 0);
			out->n = well_known[i].n;
			memcpy(out->sub, well_known[i].sub,
			       sizeof(well_known[i].sub));
			return 0;
		}
		if (domain) {
			if (!sid_ok(domain) || ((const SID *)domain)->
			    SubAuthorityCount >= SID_MAX_SUB_AUTHORITIES)
				return -1;
			memcpy(out, domain, sid_size(domain));
		} else {
			*out = machine_sid(-1);
		}
		out->sub[out->n++] = (DWORD)well_known[i].rid;
		return 0;
	}
	return -1;
}

WINFN(BOOL) adv_CreateWellKnownSid(WELL_KNOWN_SID_TYPE type, PSID domain,
				   PSID sid, DWORD *size)
{
	struct sid s;

	if (!size || well_known_sid(type, domain, &s)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	DWORD need = sid_size(&s);
	if (!sid || *size < need) {
		*size = need;
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memcpy(sid, &s, need);
	*size = need;
	return TRUE;
}

WINFN(BOOL) adv_IsWellKnownSid(PSID sid, WELL_KNOWN_SID_TYPE type)
{
	struct sid s;

	if (!sid_ok(sid) || well_known_sid(type, NULL, &s))
		return FALSE;
	if (sid_eq(sid, &s))
		return TRUE;
	/* The account SIDs are well known in any domain. */
	for (size_t i = 0; i < sizeof(well_known) / sizeof(well_known[0]); i++)
		if (well_known[i].type == type && well_known[i].rid >= 0) {
			const SID *x = sid;
			return x->SubAuthorityCount == 5 &&
			       x->IdentifierAuthority.Value[5] == 5 &&
			       x->SubAuthority[0] == SECURITY_NT_NON_UNIQUE &&
			       x->SubAuthority[4] == (DWORD)well_known[i].rid;
		}
	return FALSE;
}

/* String SIDs. */

static char *sid_to_string(const SID *s)
{
	char *out = malloc(16 + 11 * SID_MAX_SUB_AUTHORITIES + 20), *p = out;
	const BYTE *a = s->IdentifierAuthority.Value;

	if (!out)
		return NULL;
	p += sprintf(p, "S-%u-", s->Revision);
	if (a[0] || a[1])
		p += sprintf(p, "0x%02X%02X%02X%02X%02X%02X", a[0], a[1], a[2],
			     a[3], a[4], a[5]);
	else
		p += sprintf(p, "%lu", (unsigned long)((DWORD)a[2] << 24 |
						       (DWORD)a[3] << 16 |
						       (DWORD)a[4] << 8 | a[5]));
	for (int i = 0; i < s->SubAuthorityCount; i++)
		p += sprintf(p, "-%lu", (unsigned long)s->SubAuthority[i]);
	return out;
}

static BOOL sid_to_str(PSID sid, int wide, void **out)
{
	if (!out) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!sid_ok(sid)) {
		win_set_error(ERROR_INVALID_SID);
		return FALSE;
	}
	char *s = sid_to_string(sid);
	size_t n = s ? strlen(s) + 1 : 0;
	void *r = s ? global_alloc(0, n * (wide ? sizeof(WCHAR) : 1)) : NULL;
	if (r) {
		for (size_t i = 0; i < n; i++)
			if (wide)
				((WCHAR *)r)[i] = (BYTE)s[i];
			else
				((char *)r)[i] = s[i];
	}
	free(s);
	if (!r) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	*out = r;
	return TRUE;
}

WINFN(BOOL) adv_ConvertSidToStringSidA(PSID sid, LPSTR *out)
{
	return sid_to_str(sid, 0, (void **)out);
}

WINFN(BOOL) adv_ConvertSidToStringSidW(PSID sid, LPWSTR *out)
{
	return sid_to_str(sid, 1, (void **)out);
}

/* The two-letter SDDL names ConvertStringSidToSid takes too. */
static const struct {
	char alias[3];
	WELL_KNOWN_SID_TYPE type;
} sddl_aliases[] = {
	{ "WD", WinWorldSid }, { "CO", WinCreatorOwnerSid },
	{ "CG", WinCreatorGroupSid }, { "NU", WinNetworkSid },
	{ "IU", WinInteractiveSid }, { "SU", WinServiceSid },
	{ "AN", WinAnonymousSid }, { "PS", WinSelfSid },
	{ "AU", WinAuthenticatedUserSid }, { "RC", WinRestrictedCodeSid },
	{ "SY", WinLocalSystemSid }, { "LS", WinLocalServiceSid },
	{ "NS", WinNetworkServiceSid }, { "BA", WinBuiltinAdministratorsSid },
	{ "BU", WinBuiltinUsersSid }, { "BG", WinBuiltinGuestsSid },
	{ "PU", WinBuiltinPowerUsersSid }, { "LA", WinAccountAdministratorSid },
	{ "LG", WinAccountGuestSid }, { "ME", WinMediumLabelSid },
	{ "HI", WinHighLabelSid },
};

static unsigned long long parse_num(const char **p, int *ok)
{
	const char *s = *p;
	char *end;
	unsigned long long v;

	if (!(*s >= '0' && *s <= '9')) {
		*ok = 0;
		return 0;
	}
	errno = 0;
	v = strtoull(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ?
			16 : 10);
	if (errno)
		*ok = 0;
	*p = end;
	return v;
}

/* "S-1-5-32-544" or an SDDL alias; 0 or -1. */
static int string_to_sid(const char *str, struct sid *out)
{
	if (strlen(str) == 2) {
		for (size_t i = 0; i < sizeof(sddl_aliases) /
					sizeof(sddl_aliases[0]); i++)
			if (!strcasecmp(str, sddl_aliases[i].alias))
				return well_known_sid(sddl_aliases[i].type, NULL,
						      out);
		return -1;
	}
	if ((str[0] != 'S' && str[0] != 's') || str[1] != '-')
		return -1;
	const char *p = str + 2;
	int ok = 1;
	unsigned long long rev = parse_num(&p, &ok);
	if (!ok || rev != SID_REVISION || *p++ != '-')
		return -1;
	unsigned long long auth = parse_num(&p, &ok);
	if (!ok || auth >> 48)
		return -1;
	*out = mksid(0, 0);
	for (int i = 0; i < 6; i++)
		out->auth[5 - i] = (BYTE)(auth >> (8 * i));
	while (*p == '-') {
		p++;
		unsigned long long v = parse_num(&p, &ok);
		if (!ok || v > 0xffffffffULL || out->n >= SID_MAX_SUB_AUTHORITIES)
			return -1;
		out->sub[out->n++] = (DWORD)v;
	}
	return *p ? -1 : 0;
}

static BOOL str_to_sid(const char *str, PSID *out)
{
	struct sid s;

	if (!str || !out) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (string_to_sid(str, &s)) {
		win_set_error(ERROR_INVALID_SID);
		return FALSE;
	}
	void *r = global_alloc(0, sid_size(&s));
	if (!r) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	memcpy(r, &s, sid_size(&s));
	*out = r;
	return TRUE;
}

WINFN(BOOL) adv_ConvertStringSidToSidA(LPCSTR str, PSID *out)
{
	char *s = str ? acp_to_utf8(str) : NULL;
	BOOL r = str_to_sid(s, out);
	free(s);
	return r;
}

WINFN(BOOL) adv_ConvertStringSidToSidW(LPCWSTR str, PSID *out)
{
	char *s = str ? utf16_to_utf8_dup(str) : NULL;
	BOOL r = str_to_sid(s, out);
	free(s);
	return r;
}

/* Account names. */

static BOOL lookup_sid(PSID sid, int wide, void *name, LPDWORD cch,
		       void *domain, LPDWORD dcch, PSID_NAME_USE use)
{
	if (!cch || !dcch) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!sid_ok(sid)) {
		win_set_error(ERROR_INVALID_SID);
		return FALSE;
	}
	for (size_t i = 0; i < sizeof(accounts) / sizeof(accounts[0]); i++) {
		struct sid s = account_sid(&accounts[i]);
		if (!sid_eq(sid, &s))
			continue;
		DWORD n = *cch, d = *dcch;
		BOOL a = put_name(account_name(&accounts[i]), wide, name, &n);
		BOOL b = put_name(account_domain(&accounts[i]), wide, domain, &d);
		if (!a || !b) {
			/* Both sizes needed, NULs included. */
			*cch = a ? n + 1 : n;
			*dcch = b ? d + 1 : d;
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		*cch = n;
		*dcch = d;
		if (use)
			*use = accounts[i].use;
		return TRUE;
	}
	win_set_error(ERROR_NONE_MAPPED);
	return FALSE;
}

WINFN(BOOL) adv_LookupAccountSidA(LPCSTR system, PSID sid, LPSTR name,
				  LPDWORD cch, LPSTR domain, LPDWORD dcch,
				  PSID_NAME_USE use)
{
	return lookup_sid(sid, 0, name, cch, domain, dcch, use);
}

WINFN(BOOL) adv_LookupAccountSidW(LPCWSTR system, PSID sid, LPWSTR name,
				  LPDWORD cch, LPWSTR domain, LPDWORD dcch,
				  PSID_NAME_USE use)
{
	return lookup_sid(sid, 1, name, cch, domain, dcch, use);
}

/* "name" or "DOMAIN\name", matched ignoring case. */
static BOOL lookup_name(const char *account, int wide, PSID sid,
			LPDWORD cb, void *domain, LPDWORD dcch,
			PSID_NAME_USE use)
{
	if (!account || !cb || !dcch) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const char *slash = strchr(account, '\\');
	size_t dlen = slash ? (size_t)(slash - account) : 0;
	const char *name = slash ? slash + 1 : account;

	for (size_t i = 0; i < sizeof(accounts) / sizeof(accounts[0]); i++) {
		const struct account *a = &accounts[i];
		const char *d = account_domain(a);
		if (strcasecmp(name, account_name(a)) ||
		    (slash && (strlen(d) != dlen || strncasecmp(d, account, dlen))))
			continue;
		struct sid s = account_sid(a);
		DWORD need = sid_size(&s), dn = *dcch;
		BOOL fits = sid && *cb >= need;
		BOOL dfits = put_name(d, wide, domain, &dn);
		if (!fits || !dfits) {
			*cb = need;
			*dcch = dfits ? dn + 1 : dn;
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		memcpy(sid, &s, need);
		*cb = need;
		*dcch = dn;
		if (use)
			*use = a->use;
		return TRUE;
	}
	win_set_error(ERROR_NONE_MAPPED);
	return FALSE;
}

WINFN(BOOL) adv_LookupAccountNameA(LPCSTR system, LPCSTR account, PSID sid,
				   LPDWORD cb, LPSTR domain, LPDWORD dcch,
				   PSID_NAME_USE use)
{
	char *a = account ? acp_to_utf8(account) : NULL;
	BOOL r = lookup_name(a, 0, sid, cb, domain, dcch, use);
	free(a);
	return r;
}

WINFN(BOOL) adv_LookupAccountNameW(LPCWSTR system, LPCWSTR account, PSID sid,
				   LPDWORD cb, LPWSTR domain, LPDWORD dcch,
				   PSID_NAME_USE use)
{
	char *a = account ? utf16_to_utf8_dup(account) : NULL;
	BOOL r = lookup_name(a, 1, sid, cb, domain, dcch, use);
	free(a);
	return r;
}

/* ACLs. */

static int acl_ok(const ACL *acl)
{
	if (!acl || acl->AclRevision < ACL_REVISION ||
	    acl->AclRevision > ACL_REVISION_DS ||
	    acl->AclSize < sizeof(ACL))
		return 0;
	DWORD at = sizeof(ACL);
	for (WORD i = 0; i < acl->AceCount; i++) {
		const ACE_HEADER *h = (const ACE_HEADER *)((const BYTE *)acl + at);
		if (at + sizeof(*h) > acl->AclSize || h->AceSize < sizeof(*h) ||
		    at + h->AceSize > acl->AclSize)
			return 0;
		at += h->AceSize;
	}
	return 1;
}

/* Where the next ACE goes (past the last), or NULL if acl is bad. */
static BYTE *acl_end(ACL *acl)
{
	BYTE *p = (BYTE *)(acl + 1);

	for (WORD i = 0; i < acl->AceCount; i++)
		p += ((ACE_HEADER *)p)->AceSize;
	return p;
}

WINFN(BOOL) adv_InitializeAcl(PACL acl, DWORD len, DWORD rev)
{
	if (!acl || rev < ACL_REVISION || rev > ACL_REVISION_DS ||
	    len > 0xffff) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (len < sizeof(ACL)) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memset(acl, 0, sizeof(*acl));
	acl->AclRevision = (BYTE)rev;
	acl->AclSize = (WORD)(len & ~3u);
	return TRUE;
}

WINFN(BOOL) adv_IsValidAcl(PACL acl)
{
	return acl_ok(acl);
}

static BOOL add_ace(PACL acl, DWORD rev, BYTE type, DWORD flags,
		    DWORD mask, PSID sid)
{
	if (!acl_ok(acl)) {
		win_set_error(ERROR_INVALID_ACL);
		return FALSE;
	}
	if (!sid_ok(sid)) {
		win_set_error(ERROR_INVALID_SID);
		return FALSE;
	}
	if (flags & ~VALID_INHERIT_FLAGS) {
		win_set_error(ERROR_INVALID_FLAGS);
		return FALSE;
	}
	BYTE *p = acl_end(acl);
	DWORD n = offsetof(ACCESS_ALLOWED_ACE, SidStart) + sid_size(sid);
	if (p + n > (BYTE *)acl + acl->AclSize) {
		win_set_error(ERROR_ALLOTTED_SPACE_EXCEEDED);
		return FALSE;
	}
	ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)p;
	ace->Header.AceType = type;
	ace->Header.AceFlags = (BYTE)flags;
	ace->Header.AceSize = (WORD)n;
	ace->Mask = mask;
	memcpy(&ace->SidStart, sid, sid_size(sid));
	acl->AceCount++;
	if (rev > acl->AclRevision)
		acl->AclRevision = (BYTE)rev;
	return TRUE;
}

WINFN(BOOL) adv_AddAccessAllowedAce(PACL acl, DWORD rev, DWORD mask, PSID sid)
{
	return add_ace(acl, rev, ACCESS_ALLOWED_ACE_TYPE, 0, mask, sid);
}

WINFN(BOOL) adv_AddAccessAllowedAceEx(PACL acl, DWORD rev, DWORD flags,
				      DWORD mask, PSID sid)
{
	return add_ace(acl, rev, ACCESS_ALLOWED_ACE_TYPE, flags, mask, sid);
}

WINFN(BOOL) adv_AddAccessDeniedAce(PACL acl, DWORD rev, DWORD mask, PSID sid)
{
	return add_ace(acl, rev, ACCESS_DENIED_ACE_TYPE, 0, mask, sid);
}

WINFN(BOOL) adv_AddAccessDeniedAceEx(PACL acl, DWORD rev, DWORD flags,
				     DWORD mask, PSID sid)
{
	return add_ace(acl, rev, ACCESS_DENIED_ACE_TYPE, flags, mask, sid);
}

static BYTE *ace_at(PACL acl, DWORD i)
{
	BYTE *p = (BYTE *)(acl + 1);

	while (i--)
		p += ((ACE_HEADER *)p)->AceSize;
	return p;
}

WINFN(BOOL) adv_GetAce(PACL acl, DWORD i, LPVOID *ace)
{
	if (!acl_ok(acl) || !ace || i >= acl->AceCount) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*ace = ace_at(acl, i);
	return TRUE;
}

/* Inserts a list of ACEs at index start (MAXDWORD: at the end). */
WINFN(BOOL) adv_AddAce(PACL acl, DWORD rev, DWORD start, LPVOID list,
		       DWORD len)
{
	if (!acl_ok(acl) || !list) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	WORD count = 0;
	for (DWORD at = 0; at < len; count++) {
		const ACE_HEADER *h = (const ACE_HEADER *)((BYTE *)list + at);
		if (at + sizeof(*h) > len || h->AceSize < sizeof(*h) ||
		    at + h->AceSize > len) {
			win_set_error(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		at += h->AceSize;
	}
	BYTE *end = acl_end(acl);
	if (end + len > (BYTE *)acl + acl->AclSize) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (start > acl->AceCount)
		start = acl->AceCount;
	BYTE *at = ace_at(acl, start);
	memmove(at + len, at, end - at);
	memcpy(at, list, len);
	acl->AceCount += count;
	if (rev > acl->AclRevision)
		acl->AclRevision = (BYTE)rev;
	return TRUE;
}

WINFN(BOOL) adv_DeleteAce(PACL acl, DWORD i)
{
	if (!acl_ok(acl) || i >= acl->AceCount) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	BYTE *at = ace_at(acl, i), *end = acl_end(acl);
	WORD n = ((ACE_HEADER *)at)->AceSize;
	memmove(at, at + n, end - at - n);
	acl->AceCount--;
	return TRUE;
}

WINFN(BOOL) adv_GetAclInformation(PACL acl, LPVOID info, DWORD len,
				  ACL_INFORMATION_CLASS c)
{
	if (!acl_ok(acl) || !info) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (c == AclRevisionInformation) {
		if (len < sizeof(ACL_REVISION_INFORMATION)) {
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		((ACL_REVISION_INFORMATION *)info)->AclRevision = acl->AclRevision;
		return TRUE;
	}
	if (c == AclSizeInformation) {
		if (len < sizeof(ACL_SIZE_INFORMATION)) {
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		ACL_SIZE_INFORMATION *s = info;
		DWORD used = (DWORD)(acl_end(acl) - (BYTE *)acl);
		s->AceCount = acl->AceCount;
		s->AclBytesInUse = used;
		s->AclBytesFree = acl->AclSize - used;
		return TRUE;
	}
	win_set_error(ERROR_INVALID_PARAMETER);
	return FALSE;
}

/* Security descriptors, absolute (pointers) or self-relative (offsets). */

int sec_valid_descriptor(const void *sd)
{
	return sd && ((const SECURITY_DESCRIPTOR *)sd)->Revision ==
		     SECURITY_DESCRIPTOR_REVISION;
}

static int self_relative(const void *sd)
{
	return ((const SECURITY_DESCRIPTOR *)sd)->Control & SE_SELF_RELATIVE;
}

/* The parts of sd, either form. */
static void sd_parts(const void *sd, PSID *owner, PSID *group, PACL *sacl,
		     PACL *dacl)
{
	if (self_relative(sd)) {
		const SECURITY_DESCRIPTOR_RELATIVE *r = sd;
		BYTE *b = (BYTE *)sd;
		*owner = r->Owner ? b + r->Owner : NULL;
		*group = r->Group ? b + r->Group : NULL;
		*sacl = r->Sacl && (r->Control & SE_SACL_PRESENT) ?
			(PACL)(b + r->Sacl) : NULL;
		*dacl = r->Dacl && (r->Control & SE_DACL_PRESENT) ?
			(PACL)(b + r->Dacl) : NULL;
	} else {
		const SECURITY_DESCRIPTOR *a = sd;
		*owner = a->Owner;
		*group = a->Group;
		*sacl = a->Control & SE_SACL_PRESENT ? a->Sacl : NULL;
		*dacl = a->Control & SE_DACL_PRESENT ? a->Dacl : NULL;
	}
}

static DWORD sd_length(const void *sd)
{
	PSID o, g;
	PACL s, d;

	sd_parts(sd, &o, &g, &s, &d);
	return sizeof(SECURITY_DESCRIPTOR_RELATIVE) +
	       (o ? sid_size(o) : 0) + (g ? sid_size(g) : 0) +
	       (s ? s->AclSize : 0) + (d ? d->AclSize : 0);
}

WINFN(BOOL) adv_InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR sd, DWORD rev)
{
	if (!sd) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (rev != SECURITY_DESCRIPTOR_REVISION) {
		win_set_error(ERROR_UNKNOWN_REVISION);
		return FALSE;
	}
	memset(sd, 0, sizeof(SECURITY_DESCRIPTOR));
	((SECURITY_DESCRIPTOR *)sd)->Revision = SECURITY_DESCRIPTOR_REVISION;
	return TRUE;
}

WINFN(BOOL) adv_IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR sd)
{
	if (!sec_valid_descriptor(sd)) {
		win_set_error(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	return TRUE;
}

WINFN(DWORD) adv_GetSecurityDescriptorLength(PSECURITY_DESCRIPTOR sd)
{
	return sec_valid_descriptor(sd) ? sd_length(sd) : 0;
}

WINFN(BOOL) adv_GetSecurityDescriptorControl(PSECURITY_DESCRIPTOR sd,
					     PSECURITY_DESCRIPTOR_CONTROL control,
					     LPDWORD rev)
{
	if (!sec_valid_descriptor(sd) || !control || !rev) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*control = ((SECURITY_DESCRIPTOR *)sd)->Control;
	*rev = ((SECURITY_DESCRIPTOR *)sd)->Revision;
	return TRUE;
}

/* Only the inheritance bits can be set this way. */
WINFN(BOOL) adv_SetSecurityDescriptorControl(PSECURITY_DESCRIPTOR sd,
					     SECURITY_DESCRIPTOR_CONTROL mask,
					     SECURITY_DESCRIPTOR_CONTROL bits)
{
	const WORD settable = SE_DACL_AUTO_INHERIT_REQ | SE_SACL_AUTO_INHERIT_REQ |
			      SE_DACL_AUTO_INHERITED | SE_SACL_AUTO_INHERITED |
			      SE_DACL_PROTECTED | SE_SACL_PROTECTED;

	if (!sec_valid_descriptor(sd) || (mask & ~settable)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	SECURITY_DESCRIPTOR *s = sd;
	s->Control = (s->Control & ~mask) | (bits & mask);
	return TRUE;
}

/* The setters work on the absolute form only, as on Windows. */
static SECURITY_DESCRIPTOR *absolute(PSECURITY_DESCRIPTOR sd)
{
	if (!sec_valid_descriptor(sd) || self_relative(sd)) {
		win_set_error(ERROR_INVALID_SECURITY_DESCR);
		return NULL;
	}
	return sd;
}

static void set_bit(SECURITY_DESCRIPTOR *s, WORD bit, BOOL on)
{
	s->Control = on ? s->Control | bit : s->Control & ~bit;
}

WINFN(BOOL) adv_SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID sid,
					   BOOL defaulted)
{
	SECURITY_DESCRIPTOR *s = absolute(sd);

	if (!s)
		return FALSE;
	s->Owner = sid;
	set_bit(s, SE_OWNER_DEFAULTED, defaulted);
	return TRUE;
}

WINFN(BOOL) adv_SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID sid,
					   BOOL defaulted)
{
	SECURITY_DESCRIPTOR *s = absolute(sd);

	if (!s)
		return FALSE;
	s->Group = sid;
	set_bit(s, SE_GROUP_DEFAULTED, defaulted);
	return TRUE;
}

WINFN(BOOL) adv_SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd,
					  BOOL present, PACL acl,
					  BOOL defaulted)
{
	SECURITY_DESCRIPTOR *s = absolute(sd);

	if (!s)
		return FALSE;
	s->Dacl = present ? acl : NULL;
	set_bit(s, SE_DACL_PRESENT, present);
	set_bit(s, SE_DACL_DEFAULTED, present && defaulted);
	return TRUE;
}

WINFN(BOOL) adv_SetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR sd,
					  BOOL present, PACL acl,
					  BOOL defaulted)
{
	SECURITY_DESCRIPTOR *s = absolute(sd);

	if (!s)
		return FALSE;
	s->Sacl = present ? acl : NULL;
	set_bit(s, SE_SACL_PRESENT, present);
	set_bit(s, SE_SACL_DEFAULTED, present && defaulted);
	return TRUE;
}

WINFN(BOOL) adv_GetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID *sid,
					   LPBOOL defaulted)
{
	PSID o, g;
	PACL s, d;

	if (!sec_valid_descriptor(sd) || !sid || !defaulted) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	sd_parts(sd, &o, &g, &s, &d);
	*sid = o;
	*defaulted = (((SECURITY_DESCRIPTOR *)sd)->Control &
		      SE_OWNER_DEFAULTED) != 0;
	return TRUE;
}

WINFN(BOOL) adv_GetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID *sid,
					   LPBOOL defaulted)
{
	PSID o, g;
	PACL s, d;

	if (!sec_valid_descriptor(sd) || !sid || !defaulted) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	sd_parts(sd, &o, &g, &s, &d);
	*sid = g;
	*defaulted = (((SECURITY_DESCRIPTOR *)sd)->Control &
		      SE_GROUP_DEFAULTED) != 0;
	return TRUE;
}

static BOOL get_acl(PSECURITY_DESCRIPTOR sd, int dacl, LPBOOL present,
		    PACL *acl, LPBOOL defaulted)
{
	PSID o, g;
	PACL s, d;

	if (!sec_valid_descriptor(sd) || !present || !acl || !defaulted) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	WORD c = ((SECURITY_DESCRIPTOR *)sd)->Control;
	sd_parts(sd, &o, &g, &s, &d);
	*present = (c & (dacl ? SE_DACL_PRESENT : SE_SACL_PRESENT)) != 0;
	*acl = dacl ? d : s;
	*defaulted = (c & (dacl ? SE_DACL_DEFAULTED : SE_SACL_DEFAULTED)) != 0;
	return TRUE;
}

WINFN(BOOL) adv_GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd,
					  LPBOOL present, PACL *acl,
					  LPBOOL defaulted)
{
	return get_acl(sd, 1, present, acl, defaulted);
}

WINFN(BOOL) adv_GetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR sd,
					  LPBOOL present, PACL *acl,
					  LPBOOL defaulted)
{
	return get_acl(sd, 0, present, acl, defaulted);
}

/* The self-relative form of sd (either form) into out. */
static void make_relative(const void *sd, BYTE *out)
{
	PSID o, g;
	PACL s, d;
	SECURITY_DESCRIPTOR_RELATIVE *r = (SECURITY_DESCRIPTOR_RELATIVE *)out;
	DWORD at = sizeof(*r);

	sd_parts(sd, &o, &g, &s, &d);
	memset(r, 0, sizeof(*r));
	r->Revision = SECURITY_DESCRIPTOR_REVISION;
	r->Control = ((const SECURITY_DESCRIPTOR *)sd)->Control | SE_SELF_RELATIVE;
	if (o) {
		memcpy(out + at, o, sid_size(o));
		r->Owner = at;
		at += sid_size(o);
	}
	if (g) {
		memcpy(out + at, g, sid_size(g));
		r->Group = at;
		at += sid_size(g);
	}
	if (s) {
		memcpy(out + at, s, s->AclSize);
		r->Sacl = at;
		at += s->AclSize;
	}
	if (d) {
		memcpy(out + at, d, d->AclSize);
		r->Dacl = at;
	}
}

WINFN(BOOL) adv_MakeSelfRelativeSD(PSECURITY_DESCRIPTOR sd,
				   PSECURITY_DESCRIPTOR out, LPDWORD len)
{
	if (!sec_valid_descriptor(sd) || !len) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	DWORD need = sd_length(sd);
	if (!out || *len < need) {
		*len = need;
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	make_relative(sd, out);
	*len = need;
	return TRUE;
}

void *sec_default_descriptor(SECURITY_INFORMATION which, DWORD *size)
{
	struct sid admins = mksid(5, 2, 32, 544), none =
		machine_sid(DOMAIN_GROUP_RID_USERS);
	struct sid system = mksid(5, 1, SECURITY_LOCAL_SYSTEM_RID),
		   user = user_sid(), users = mksid(5, 2, 32, 545);
	BYTE acl_buf[256];
	ACL *acl = (ACL *)acl_buf;
	SECURITY_DESCRIPTOR sd;

	adv_InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	if (which & OWNER_SECURITY_INFORMATION)
		sd.Owner = &admins;
	if (which & GROUP_SECURITY_INFORMATION)
		sd.Group = &none;
	if (which & DACL_SECURITY_INFORMATION) {
		const DWORD all = STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x1ff;
		adv_InitializeAcl(acl, sizeof(acl_buf), ACL_REVISION);
		add_ace(acl, ACL_REVISION, ACCESS_ALLOWED_ACE_TYPE, 0, all, &system);
		add_ace(acl, ACL_REVISION, ACCESS_ALLOWED_ACE_TYPE, 0, all, &admins);
		add_ace(acl, ACL_REVISION, ACCESS_ALLOWED_ACE_TYPE, 0, all, &user);
		add_ace(acl, ACL_REVISION, ACCESS_ALLOWED_ACE_TYPE, 0,
			READ_CONTROL | SYNCHRONIZE | 0xa9, &users);
		acl->AclSize = (WORD)(acl_end(acl) - acl_buf);
		sd.Dacl = acl;
		sd.Control |= SE_DACL_PRESENT;
	}
	*size = sd_length(&sd);
	BYTE *out = malloc(*size);
	if (out)
		make_relative(&sd, out);
	return out;
}

/* Objects' security. */

/* Whether the named object is there: 0, or a Win32 error. */
static DWORD object_exists(const char *name, SE_OBJECT_TYPE type)
{
	if (type == SE_FILE_OBJECT) {
		char *u = path_to_unix(name);
		DWORD r = !u ? ERROR_PATH_NOT_FOUND :
			  access(u, F_OK) ? ERROR_FILE_NOT_FOUND : 0;
		free(u);
		return r;
	}
	if (type == SE_REGISTRY_KEY || type == SE_REGISTRY_WOW64_32KEY) {
		/* CLASSES_ROOT\x, CURRENT_USER\x, MACHINE\x or USERS\x. */
		static const struct {
			const char *name, *hive;
		} roots[] = {
			{ "CLASSES_ROOT", "HKEY_LOCAL_MACHINE\\Software\\Classes" },
			{ "CURRENT_USER", "HKEY_CURRENT_USER" },
			{ "MACHINE", "HKEY_LOCAL_MACHINE" },
			{ "USERS", "HKEY_USERS" },
			{ "CURRENT_CONFIG", "HKEY_CURRENT_CONFIG" },
		};
		size_t n = strcspn(name, "\\");
		for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
			if (strlen(roots[i].name) != n ||
			    strncasecmp(name, roots[i].name, n))
				continue;
			char *path = malloc(strlen(roots[i].hive) +
					    strlen(name + n) + 1);
			if (!path)
				return ERROR_NOT_ENOUGH_MEMORY;
			sprintf(path, "%s%s", roots[i].hive, name + n);
			reg_lock();
			DWORD r = reg_open(path) ? 0 : ERROR_FILE_NOT_FOUND;
			reg_unlock();
			free(path);
			return r;
		}
		return ERROR_INVALID_PARAMETER;
	}
	return 0;
}

/* The descriptor and its parts for GetNamedSecurityInfo and the like. */
static DWORD give_descriptor(SECURITY_INFORMATION which, PSID *owner,
			     PSID *group, PACL *dacl, PACL *sacl,
			     PSECURITY_DESCRIPTOR *out)
{
	if (!out && (owner || group || dacl || sacl))
		return ERROR_INVALID_PARAMETER;
	DWORD size;
	void *sd = sec_default_descriptor(which, &size);
	void *l = sd ? global_alloc(0, size) : NULL;
	if (!l) {
		free(sd);
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	memcpy(l, sd, size);
	free(sd);
	PSID o, g;
	PACL s, d;
	sd_parts(l, &o, &g, &s, &d);
	if (owner)
		*owner = o;
	if (group)
		*group = g;
	if (dacl)
		*dacl = d;
	if (sacl)
		*sacl = s;
	if (out)
		*out = l;
	else
		global_free(l);
	return ERROR_SUCCESS;
}

static DWORD get_named(const char *name, SE_OBJECT_TYPE type,
		       SECURITY_INFORMATION which, PSID *owner, PSID *group,
		       PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *out)
{
	if (!name)
		return ERROR_INVALID_PARAMETER;
	DWORD err = object_exists(name, type);
	return err ? err : give_descriptor(which, owner, group, dacl, sacl, out);
}

WINFN(DWORD) adv_GetNamedSecurityInfoA(LPSTR name, SE_OBJECT_TYPE type,
				       SECURITY_INFORMATION which, PSID *owner,
				       PSID *group, PACL *dacl, PACL *sacl,
				       PSECURITY_DESCRIPTOR *sd)
{
	char *n = name ? acp_to_utf8(name) : NULL;
	DWORD r = get_named(n, type, which, owner, group, dacl, sacl, sd);
	free(n);
	return r;
}

WINFN(DWORD) adv_GetNamedSecurityInfoW(LPWSTR name, SE_OBJECT_TYPE type,
				       SECURITY_INFORMATION which, PSID *owner,
				       PSID *group, PACL *dacl, PACL *sacl,
				       PSECURITY_DESCRIPTOR *sd)
{
	char *n = name ? utf16_to_utf8_dup(name) : NULL;
	DWORD r = get_named(n, type, which, owner, group, dacl, sacl, sd);
	free(n);
	return r;
}

WINFN(DWORD) adv_SetNamedSecurityInfoA(LPSTR name, SE_OBJECT_TYPE type,
				       SECURITY_INFORMATION which, PSID owner,
				       PSID group, PACL dacl, PACL sacl)
{
	char *n = name ? acp_to_utf8(name) : NULL;
	DWORD r = n ? object_exists(n, type) : ERROR_INVALID_PARAMETER;
	free(n);
	return r;
}

WINFN(DWORD) adv_SetNamedSecurityInfoW(LPWSTR name, SE_OBJECT_TYPE type,
				       SECURITY_INFORMATION which, PSID owner,
				       PSID group, PACL dacl, PACL sacl)
{
	char *n = name ? utf16_to_utf8_dup(name) : NULL;
	DWORD r = n ? object_exists(n, type) : ERROR_INVALID_PARAMETER;
	free(n);
	return r;
}

WINFN(DWORD) adv_GetSecurityInfo(HANDLE h, SE_OBJECT_TYPE type,
				 SECURITY_INFORMATION which, PSID *owner,
				 PSID *group, PACL *dacl, PACL *sacl,
				 PSECURITY_DESCRIPTOR *sd)
{
	/* -1 is GetCurrentProcess(), not INVALID_HANDLE_VALUE, here. */
	if (!h)
		return ERROR_INVALID_HANDLE;
	return give_descriptor(which, owner, group, dacl, sacl, sd);
}

WINFN(DWORD) adv_SetSecurityInfo(HANDLE h, SE_OBJECT_TYPE type,
				 SECURITY_INFORMATION which, PSID owner,
				 PSID group, PACL dacl, PACL sacl)
{
	return h ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

static BOOL get_file_security(const char *name, SECURITY_INFORMATION which,
			      PSECURITY_DESCRIPTOR sd, DWORD len, LPDWORD need)
{
	DWORD err = name ? object_exists(name, SE_FILE_OBJECT) :
			   ERROR_INVALID_PARAMETER;

	if (!err && !need)
		err = ERROR_NOACCESS;
	if (err) {
		win_set_error(err);
		return FALSE;
	}
	DWORD size;
	void *d = sec_default_descriptor(which, &size);
	if (!d) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	*need = size;
	BOOL ok = sd && len >= size;
	if (ok)
		memcpy(sd, d, size);
	else
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
	free(d);
	return ok;
}

WINFN(BOOL) adv_GetFileSecurityA(LPCSTR name, SECURITY_INFORMATION which,
				 PSECURITY_DESCRIPTOR sd, DWORD len,
				 LPDWORD need)
{
	char *n = name ? acp_to_utf8(name) : NULL;
	BOOL r = get_file_security(n, which, sd, len, need);
	free(n);
	return r;
}

WINFN(BOOL) adv_GetFileSecurityW(LPCWSTR name, SECURITY_INFORMATION which,
				 PSECURITY_DESCRIPTOR sd, DWORD len,
				 LPDWORD need)
{
	char *n = name ? utf16_to_utf8_dup(name) : NULL;
	BOOL r = get_file_security(n, which, sd, len, need);
	free(n);
	return r;
}

static BOOL set_file_security(const char *name, PSECURITY_DESCRIPTOR sd)
{
	DWORD err = !name || !sec_valid_descriptor(sd) ?
		    ERROR_INVALID_PARAMETER : object_exists(name, SE_FILE_OBJECT);

	if (err)
		win_set_error(err);
	return !err;
}

WINFN(BOOL) adv_SetFileSecurityA(LPCSTR name, SECURITY_INFORMATION which,
				 PSECURITY_DESCRIPTOR sd)
{
	char *n = name ? acp_to_utf8(name) : NULL;
	BOOL r = set_file_security(n, sd);
	free(n);
	return r;
}

WINFN(BOOL) adv_SetFileSecurityW(LPCWSTR name, SECURITY_INFORMATION which,
				 PSECURITY_DESCRIPTOR sd)
{
	char *n = name ? utf16_to_utf8_dup(name) : NULL;
	BOOL r = set_file_security(n, sd);
	free(n);
	return r;
}

/* CryptoAPI: providers only for their random numbers. */

/*
 * Providers live in a list, so a handle that was never one, or was
 * released, is refused rather than followed.
 */
struct provider {
	struct provider *next;
	LONG refs;
};

static pthread_mutex_t prov_lock = PTHREAD_MUTEX_INITIALIZER;
static struct provider *providers;

static int known_type(DWORD type)
{
	switch (type) {
	case PROV_RSA_FULL:
	case PROV_RSA_SIG:
	case PROV_DSS:
	case PROV_RSA_SCHANNEL:
	case PROV_DSS_DH:
	case PROV_DH_SCHANNEL:
	case PROV_RSA_AES:
		return 1;
	}
	return 0;
}

static BOOL acquire(HCRYPTPROV *out, DWORD type, DWORD flags)
{
	const DWORD known = CRYPT_VERIFYCONTEXT | CRYPT_NEWKEYSET |
			    CRYPT_DELETEKEYSET | CRYPT_MACHINE_KEYSET |
			    CRYPT_SILENT;

	if (!out) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!known_type(type)) {
		win_set_error((DWORD)NTE_BAD_PROV_TYPE);
		return FALSE;
	}
	if (flags & ~known) {
		win_set_error((DWORD)NTE_BAD_FLAGS);
		return FALSE;
	}
	if (flags & CRYPT_DELETEKEYSET) {
		*out = 0;
		return TRUE;
	}
	struct provider *p = malloc(sizeof(*p));
	if (!p) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	p->refs = 1;
	pthread_mutex_lock(&prov_lock);
	p->next = providers;
	providers = p;
	pthread_mutex_unlock(&prov_lock);
	*out = (HCRYPTPROV)p;
	return TRUE;
}

WINFN(BOOL) adv_CryptAcquireContextA(HCRYPTPROV *out, LPCSTR container,
				     LPCSTR provider, DWORD type, DWORD flags)
{
	return acquire(out, type, flags);
}

WINFN(BOOL) adv_CryptAcquireContextW(HCRYPTPROV *out, LPCWSTR container,
				     LPCWSTR provider, DWORD type, DWORD flags)
{
	return acquire(out, type, flags);
}

/* The live provider h, with prov_lock held; NULL (unlocked) if none. */
static struct provider *lock_provider(HCRYPTPROV h)
{
	pthread_mutex_lock(&prov_lock);
	for (struct provider *p = providers; p; p = p->next)
		if ((HCRYPTPROV)p == h)
			return p;
	pthread_mutex_unlock(&prov_lock);
	win_set_error(ERROR_INVALID_PARAMETER);
	return NULL;
}

WINFN(BOOL) adv_CryptContextAddRef(HCRYPTPROV h, DWORD *reserved, DWORD flags)
{
	struct provider *p = lock_provider(h);

	if (!p)
		return FALSE;
	p->refs++;
	pthread_mutex_unlock(&prov_lock);
	return TRUE;
}

WINFN(BOOL) adv_CryptReleaseContext(HCRYPTPROV h, DWORD flags)
{
	struct provider *p = lock_provider(h);

	if (!p)
		return FALSE;
	if (flags) {
		pthread_mutex_unlock(&prov_lock);
		win_set_error((DWORD)NTE_BAD_FLAGS);
		return FALSE;
	}
	if (!--p->refs) {
		for (struct provider **pp = &providers; *pp; pp = &(*pp)->next)
			if (*pp == p) {
				*pp = p->next;
				break;
			}
		free(p);
	}
	pthread_mutex_unlock(&prov_lock);
	return TRUE;
}

static int fill_random(BYTE *buf, DWORD len)
{
	while (len) {
		ssize_t n = getrandom(buf, len, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= (DWORD)n;
	}
	return 0;
}

WINFN(BOOL) adv_CryptGenRandom(HCRYPTPROV h, DWORD len, BYTE *buf)
{
	if (!lock_provider(h))
		return FALSE;
	pthread_mutex_unlock(&prov_lock);
	if (len && !buf) {
		win_set_error(ERROR_NOACCESS);
		return FALSE;
	}
	if (fill_random(buf, len)) {
		win_set_error(ERROR_GEN_FAILURE);
		return FALSE;
	}
	return TRUE;
}

/* RtlGenRandom, which advapi32 exports as SystemFunction036. */
WINFN(BOOLEAN) adv_SystemFunction036(PVOID buf, ULONG len)
{
	return (!len || buf) && !fill_random(buf, len);
}

#define E(name) { #name, 0, (void *)adv_##name }

const struct builtin_export adv_security_exports[] = {
	E(AddAccessAllowedAce),
	E(AddAccessAllowedAceEx),
	E(AddAccessDeniedAce),
	E(AddAccessDeniedAceEx),
	E(AddAce),
	E(AdjustTokenPrivileges),
	E(AllocateAndInitializeSid),
	E(CheckTokenMembership),
	E(ConvertSidToStringSidA),
	E(ConvertSidToStringSidW),
	E(ConvertStringSidToSidA),
	E(ConvertStringSidToSidW),
	E(CopySid),
	E(CreateWellKnownSid),
	E(CryptAcquireContextA),
	E(CryptAcquireContextW),
	E(CryptContextAddRef),
	E(CryptGenRandom),
	E(CryptReleaseContext),
	E(DeleteAce),
	E(DuplicateToken),
	E(DuplicateTokenEx),
	E(EqualPrefixSid),
	E(EqualSid),
	E(FreeSid),
	E(GetAce),
	E(GetAclInformation),
	E(GetFileSecurityA),
	E(GetFileSecurityW),
	E(GetLengthSid),
	E(GetNamedSecurityInfoA),
	E(GetNamedSecurityInfoW),
	E(GetSecurityDescriptorControl),
	E(GetSecurityDescriptorDacl),
	E(GetSecurityDescriptorGroup),
	E(GetSecurityDescriptorLength),
	E(GetSecurityDescriptorOwner),
	E(GetSecurityDescriptorSacl),
	E(GetSecurityInfo),
	E(GetSidIdentifierAuthority),
	E(GetSidLengthRequired),
	E(GetSidSubAuthority),
	E(GetSidSubAuthorityCount),
	E(GetTokenInformation),
	E(ImpersonateSelf),
	E(InitializeAcl),
	E(InitializeSecurityDescriptor),
	E(InitializeSid),
	E(IsValidAcl),
	E(IsValidSecurityDescriptor),
	E(IsValidSid),
	E(IsWellKnownSid),
	E(LookupAccountNameA),
	E(LookupAccountNameW),
	E(LookupAccountSidA),
	E(LookupAccountSidW),
	E(LookupPrivilegeNameA),
	E(LookupPrivilegeNameW),
	E(LookupPrivilegeValueA),
	E(LookupPrivilegeValueW),
	E(MakeSelfRelativeSD),
	E(OpenProcessToken),
	E(OpenThreadToken),
	E(PrivilegeCheck),
	E(RevertToSelf),
	E(SetFileSecurityA),
	E(SetFileSecurityW),
	E(SetNamedSecurityInfoA),
	E(SetNamedSecurityInfoW),
	E(SetSecurityDescriptorControl),
	E(SetSecurityDescriptorDacl),
	E(SetSecurityDescriptorGroup),
	E(SetSecurityDescriptorOwner),
	E(SetSecurityDescriptorSacl),
	E(SetSecurityInfo),
	E(SetThreadToken),
	E(SetTokenInformation),
	E(SystemFunction036),
	BUILTIN_END
};
