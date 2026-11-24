/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * advapi32's security functions, called through its export table: tokens
 * and privileges, SIDs and account names, ACLs and security descriptors,
 * objects' security, and CryptGenRandom.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "win32/internal.h"
#include "test.h"

static struct module *adv, *k32;
static char dir[256];

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "no %s\n", name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(adv, #name)

static HLOCAL local_free(HLOCAL h)
{
	HLOCAL (WINAPI *f)(HLOCAL) = get_from(k32, "LocalFree");
	return f(h);
}

static BOOL close_handle(HANDLE h)
{
	BOOL (WINAPI *f)(HANDLE) = get_from(k32, "CloseHandle");
	return f(h);
}

/* A SID's string form (malloc()ed), for comparing. */
static char *sid_str(PSID sid)
{
	FN(BOOL, ConvertSidToStringSidA, PSID, LPSTR *);
	LPSTR s = NULL;

	if (!ConvertSidToStringSidA(sid, &s))
		return strdup("?");
	char *r = strdup(s);
	local_free(s);
	return r;
}

#define CHECK_SID(sid, want) do { \
	char *s_ = sid_str(sid); \
	CHECK_STR(s_, want); \
	free(s_); \
} while (0)

#define MACHINE "S-1-5-21-1004336348-1177238915-682003330"

static const char *user(void)
{
	const char *u = getenv("USER");
	return u && *u ? u : "user";
}

static void test_tokens(void)
{
	FN(BOOL, OpenProcessToken, HANDLE, DWORD, PHANDLE);
	FN(BOOL, OpenThreadToken, HANDLE, DWORD, BOOL, PHANDLE);
	FN(BOOL, GetTokenInformation, HANDLE, TOKEN_INFORMATION_CLASS, LPVOID,
	   DWORD, PDWORD);
	FN(BOOL, AdjustTokenPrivileges, HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD,
	   PTOKEN_PRIVILEGES, PDWORD);
	FN(BOOL, LookupPrivilegeValueA, LPCSTR, LPCSTR, PLUID);
	FN(BOOL, LookupPrivilegeNameA, LPCSTR, PLUID, LPSTR, LPDWORD);
	FN(BOOL, PrivilegeCheck, HANDLE, PPRIVILEGE_SET, LPBOOL);
	FN(BOOL, ImpersonateSelf, SECURITY_IMPERSONATION_LEVEL);
	FN(BOOL, RevertToSelf, void);
	FN(BOOL, DuplicateTokenEx, HANDLE, DWORD, LPSECURITY_ATTRIBUTES,
	   SECURITY_IMPERSONATION_LEVEL, TOKEN_TYPE, PHANDLE);
	FN(BOOL, CheckTokenMembership, HANDLE, PSID, PBOOL);
	FN(BOOL, CreateWellKnownSid, WELL_KNOWN_SID_TYPE, PSID, PSID, DWORD *);
	HANDLE tok, tok2, imp, th;
	BYTE buf[1024];
	DWORD len = 0;

	CHECK(OpenProcessToken((HANDLE)(LONG_PTR)-1, TOKEN_QUERY, &tok));

	/* The user, by size query first. */
	CHECK(!GetTokenInformation(tok, TokenUser, NULL, 0, &len));
	CHECK_EQ(win_get_error(), ERROR_INSUFFICIENT_BUFFER);
	CHECK(len > sizeof(TOKEN_USER));
	DWORD need = len;
	CHECK(!GetTokenInformation(tok, TokenUser, buf, need - 1, &len));
	CHECK(GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len));
	CHECK_EQ(len, need);
	CHECK_SID(((TOKEN_USER *)buf)->User.Sid, MACHINE "-1000");

	/* Groups: Administrators owns, the logon SID is marked. */
	CHECK(GetTokenInformation(tok, TokenGroups, buf, sizeof(buf), &len));
	TOKEN_GROUPS *g = (TOKEN_GROUPS *)buf;
	int admins = 0, logon = 0;
	CHECK_EQ(g->GroupCount, 8);
	for (DWORD i = 0; i < g->GroupCount; i++) {
		char *s = sid_str(g->Groups[i].Sid);
		if (!strcmp(s, "S-1-5-32-544"))
			admins = g->Groups[i].Attributes ==
				 (SE_GROUP_MANDATORY | SE_GROUP_ENABLED |
				  SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_OWNER);
		logon |= !strncmp(s, "S-1-5-5-0-", 10) &&
			 (g->Groups[i].Attributes & SE_GROUP_LOGON_ID) ==
			 SE_GROUP_LOGON_ID;
		free(s);
		/* Each SID lies inside the buffer given. */
		CHECK((BYTE *)g->Groups[i].Sid > buf &&
		      (BYTE *)g->Groups[i].Sid < buf + len);
	}
	CHECK(admins && logon);

	CHECK(GetTokenInformation(tok, TokenOwner, buf, sizeof(buf), &len));
	CHECK_SID(((TOKEN_OWNER *)buf)->Owner, "S-1-5-32-544");
	CHECK(GetTokenInformation(tok, TokenPrimaryGroup, buf, sizeof(buf), &len));
	CHECK_SID(((TOKEN_PRIMARY_GROUP *)buf)->PrimaryGroup, MACHINE "-513");
	CHECK(GetTokenInformation(tok, TokenElevation, buf, sizeof(buf), &len));
	CHECK(len == sizeof(TOKEN_ELEVATION) &&
	      ((TOKEN_ELEVATION *)buf)->TokenIsElevated);
	CHECK(GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf),
				  &len));
	CHECK_SID(((TOKEN_MANDATORY_LABEL *)buf)->Label.Sid, "S-1-16-12288");
	CHECK(GetTokenInformation(tok, TokenType, buf, sizeof(buf), &len));
	CHECK_EQ(*(TOKEN_TYPE *)buf, TokenPrimary);
	CHECK(!GetTokenInformation(tok, TokenImpersonationLevel, buf,
				   sizeof(buf), &len));
	CHECK_EQ(win_get_error(), ERROR_INVALID_PARAMETER);
	CHECK(GetTokenInformation(tok, TokenStatistics, buf, sizeof(buf), &len));
	CHECK(((TOKEN_STATISTICS *)buf)->GroupCount == 8 &&
	      ((TOKEN_STATISTICS *)buf)->PrivilegeCount == 20);

	/* Privileges by name and LUID. */
	LUID debug, tcb;
	CHECK(LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &debug));
	CHECK(debug.LowPart == 20 && debug.HighPart == 0);
	CHECK(LookupPrivilegeValueA(NULL, "setcbprivilege", &tcb));
	CHECK(!LookupPrivilegeValueA(NULL, "SeNoSuchPrivilege", &tcb));
	CHECK_EQ(win_get_error(), ERROR_NO_SUCH_PRIVILEGE);
	char name[64];
	DWORD cch = 4;
	CHECK(!LookupPrivilegeNameA(NULL, &debug, name, &cch));
	CHECK(win_get_error() == ERROR_INSUFFICIENT_BUFFER && cch == 17);
	CHECK(LookupPrivilegeNameA(NULL, &debug, name, &cch));
	CHECK(!strcmp(name, "SeDebugPrivilege") && cch == 16);

	/* SeChangeNotify starts enabled, SeDebug held but not. */
	CHECK(GetTokenInformation(tok, TokenPrivileges, buf, sizeof(buf), &len));
	TOKEN_PRIVILEGES *tp = (TOKEN_PRIVILEGES *)buf;
	DWORD notify = 0, dbg = 9;
	CHECK_EQ(tp->PrivilegeCount, 20);
	for (DWORD i = 0; i < tp->PrivilegeCount; i++) {
		if (tp->Privileges[i].Luid.LowPart == 23)
			notify = tp->Privileges[i].Attributes;
		if (tp->Privileges[i].Luid.LowPart == 20)
			dbg = tp->Privileges[i].Attributes;
	}
	CHECK_EQ(notify, SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT);
	CHECK_EQ(dbg, 0);

	/* Enabling one, seen through another handle to the same token. */
	struct {
		DWORD n;
		LUID_AND_ATTRIBUTES p[2];
	} want = { 1, { { debug, SE_PRIVILEGE_ENABLED } } }, prev;
	DWORD plen = 0;
	CHECK(!AdjustTokenPrivileges(tok, FALSE, (PTOKEN_PRIVILEGES)&want, 4,
				     (PTOKEN_PRIVILEGES)&prev, &plen));
	CHECK(win_get_error() == ERROR_INSUFFICIENT_BUFFER && plen == 16);
	CHECK(AdjustTokenPrivileges(tok, FALSE, (PTOKEN_PRIVILEGES)&want,
				    sizeof(prev), (PTOKEN_PRIVILEGES)&prev,
				    &plen));
	CHECK_EQ(win_get_error(), ERROR_SUCCESS);
	CHECK(prev.n == 1 && prev.p[0].Luid.LowPart == 20 &&
	      prev.p[0].Attributes == 0);
	CHECK(OpenProcessToken((HANDLE)(LONG_PTR)-1, TOKEN_QUERY, &tok2));
	struct {
		DWORD n, control;
		LUID_AND_ATTRIBUTES p[1];
	} check = { 1, PRIVILEGE_SET_ALL_NECESSARY, { { debug, 0 } } };
	BOOL has = FALSE;
	CHECK(PrivilegeCheck(tok2, (PPRIVILEGE_SET)&check, &has));
	CHECK(has && (check.p[0].Attributes & SE_PRIVILEGE_USED_FOR_ACCESS));
	/* One it doesn't hold: done, with ERROR_NOT_ALL_ASSIGNED. */
	want.p[0].Luid = tcb;
	CHECK(AdjustTokenPrivileges(tok, FALSE, (PTOKEN_PRIVILEGES)&want, 0,
				    NULL, NULL));
	CHECK_EQ(win_get_error(), ERROR_NOT_ALL_ASSIGNED);
	CHECK(AdjustTokenPrivileges(tok2, TRUE, NULL, 0, NULL, NULL));
	check.p[0].Attributes = 0;
	CHECK(PrivilegeCheck(tok, (PPRIVILEGE_SET)&check, &has));
	CHECK(!has);

	/* Impersonation, and membership through it. */
	BYTE admins_sid[SECURITY_MAX_SID_SIZE], guests[SECURITY_MAX_SID_SIZE];
	DWORD n1 = sizeof(admins_sid), n2 = sizeof(guests);
	CHECK(CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admins_sid,
				 &n1));
	CHECK(CreateWellKnownSid(WinBuiltinGuestsSid, NULL, guests, &n2));
	BOOL member = TRUE;
	CHECK(!OpenThreadToken((HANDLE)(LONG_PTR)-2, TOKEN_QUERY, FALSE, &th));
	CHECK_EQ(win_get_error(), ERROR_NO_TOKEN);
	CHECK(!CheckTokenMembership(tok, admins_sid, &member));
	CHECK_EQ(win_get_error(), ERROR_NO_IMPERSONATION_TOKEN);
	CHECK(CheckTokenMembership(NULL, admins_sid, &member) && member);
	CHECK(CheckTokenMembership(NULL, guests, &member) && !member);
	CHECK(ImpersonateSelf(SecurityImpersonation));
	CHECK(OpenThreadToken((HANDLE)(LONG_PTR)-2, TOKEN_QUERY, FALSE, &th));
	CHECK(GetTokenInformation(th, TokenImpersonationLevel, buf, sizeof(buf),
				  &len));
	CHECK_EQ(*(SECURITY_IMPERSONATION_LEVEL *)buf, SecurityImpersonation);
	CHECK(CheckTokenMembership(th, admins_sid, &member) && member);
	CHECK(RevertToSelf());
	CHECK(!OpenThreadToken((HANDLE)(LONG_PTR)-2, TOKEN_QUERY, FALSE, &imp));
	CHECK(DuplicateTokenEx(tok, 0, NULL, SecurityIdentification,
			       TokenImpersonation, &imp));
	CHECK(CheckTokenMembership(imp, admins_sid, &member) && member);
	CHECK(GetTokenInformation(imp, TokenType, buf, sizeof(buf), &len));
	CHECK_EQ(*(TOKEN_TYPE *)buf, TokenImpersonation);

	CHECK(close_handle(imp) && close_handle(th) && close_handle(tok2) &&
	      close_handle(tok));
	CHECK(!GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len));
	CHECK_EQ(win_get_error(), ERROR_INVALID_HANDLE);
}

static void test_sids(void)
{
	FN(BOOL, AllocateAndInitializeSid, PSID_IDENTIFIER_AUTHORITY, BYTE, DWORD,
	   DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, PSID *);
	FN(PVOID, FreeSid, PSID);
	FN(BOOL, EqualSid, PSID, PSID);
	FN(BOOL, EqualPrefixSid, PSID, PSID);
	FN(BOOL, IsValidSid, PSID);
	FN(DWORD, GetLengthSid, PSID);
	FN(DWORD, GetSidLengthRequired, UCHAR);
	FN(BOOL, CopySid, DWORD, PSID, PSID);
	FN(PDWORD, GetSidSubAuthority, PSID, DWORD);
	FN(PUCHAR, GetSidSubAuthorityCount, PSID);
	FN(BOOL, CreateWellKnownSid, WELL_KNOWN_SID_TYPE, PSID, PSID, DWORD *);
	FN(BOOL, IsWellKnownSid, PSID, WELL_KNOWN_SID_TYPE);
	FN(BOOL, ConvertStringSidToSidA, LPCSTR, PSID *);
	FN(BOOL, ConvertSidToStringSidW, PSID, LPWSTR *);
	SID_IDENTIFIER_AUTHORITY nt = { { 0, 0, 0, 0, 0, 5 } };
	PSID ba = NULL, users = NULL, s = NULL;
	BYTE wk[SECURITY_MAX_SID_SIZE], copy[16];
	DWORD n = sizeof(wk);

	CHECK(AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
				       DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
				       &ba));
	CHECK(AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
				       DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0,
				       &users));
	CHECK(IsValidSid(ba) && GetLengthSid(ba) == 16);
	CHECK_EQ(GetSidLengthRequired(2), 16);
	CHECK(CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, wk, &n));
	CHECK(n == 16 && EqualSid(ba, wk) && !EqualSid(ba, users));
	CHECK(EqualPrefixSid(ba, users));
	CHECK(IsWellKnownSid(ba, WinBuiltinAdministratorsSid));
	CHECK(!IsWellKnownSid(ba, WinBuiltinUsersSid));
	CHECK(*GetSidSubAuthorityCount(ba) == 2 && *GetSidSubAuthority(ba, 1) == 544);
	CHECK(!CopySid(8, copy, ba));
	CHECK_EQ(win_get_error(), ERROR_INSUFFICIENT_BUFFER);
	CHECK(CopySid(sizeof(copy), copy, ba) && EqualSid(copy, ba));
	CHECK_SID(ba, "S-1-5-32-544");

	/* Account SIDs: in the machine's domain, or the one given. */
	n = sizeof(wk);
	CHECK(!CreateWellKnownSid(WinAccountAdministratorSid, NULL, wk, NULL));
	CHECK(CreateWellKnownSid(WinAccountAdministratorSid, NULL, wk, &n));
	CHECK_SID(wk, MACHINE "-500");
	CHECK(IsWellKnownSid(wk, WinAccountAdministratorSid));
	n = 4;
	CHECK(!CreateWellKnownSid(WinWorldSid, NULL, wk, &n));
	CHECK(win_get_error() == ERROR_INSUFFICIENT_BUFFER && n == 12);

	/* String forms: numbers, hex authorities, SDDL's names. */
	CHECK(ConvertStringSidToSidA("S-1-5-32-544", &s) && EqualSid(s, ba));
	local_free(s);
	CHECK(ConvertStringSidToSidA("BA", &s) && EqualSid(s, ba));
	local_free(s);
	CHECK(ConvertStringSidToSidA("wd", &s));
	CHECK_SID(s, "S-1-1-0");
	local_free(s);
	CHECK(ConvertStringSidToSidA("S-1-0x1000000000A-7", &s));
	CHECK_SID(s, "S-1-0x01000000000A-7");
	local_free(s);
	CHECK(!ConvertStringSidToSidA("S-1-5-x", &s));
	CHECK_EQ(win_get_error(), ERROR_INVALID_SID);
	CHECK(!ConvertStringSidToSidA("S-2-5", &s));
	CHECK(!ConvertStringSidToSidA("S-1-5-1-2-3-4-5-6-7-8-9-10-11-12-13-14-15-16",
				      &s));
	LPWSTR w = NULL;
	CHECK(ConvertSidToStringSidW(users, &w));
	CHECK(w && w[0] == 'S' && w[11] == '5' && w[12] == 0);
	local_free(w);

	CHECK(FreeSid(ba) == NULL);
	FreeSid(users);
}

static void test_accounts(void)
{
	FN(BOOL, LookupAccountSidA, LPCSTR, PSID, LPSTR, LPDWORD, LPSTR, LPDWORD,
	   PSID_NAME_USE);
	FN(BOOL, LookupAccountNameA, LPCSTR, LPCSTR, PSID, LPDWORD, LPSTR,
	   LPDWORD, PSID_NAME_USE);
	FN(BOOL, LookupAccountNameW, LPCWSTR, LPCWSTR, PSID, LPDWORD, LPWSTR,
	   LPDWORD, PSID_NAME_USE);
	FN(BOOL, ConvertStringSidToSidA, LPCSTR, PSID *);
	char name[64], domain[64];
	DWORD cn = sizeof(name), cd = sizeof(domain), cb;
	SID_NAME_USE use = 0;
	BYTE sid[SECURITY_MAX_SID_SIZE];
	PSID s;

	/* The user, their groups and the well-known ones. */
	CHECK(ConvertStringSidToSidA(MACHINE "-1000", &s));
	CHECK(LookupAccountSidA(NULL, s, name, &cn, domain, &cd, &use));
	CHECK_STR(name, user());
	CHECK_STR(domain, "SEANCE");
	CHECK(use == SidTypeUser && cn == strlen(user()) && cd == 6);
	local_free(s);
	CHECK(ConvertStringSidToSidA("S-1-1-0", &s));
	cn = sizeof(name);
	cd = sizeof(domain);
	CHECK(LookupAccountSidA(NULL, s, name, &cn, domain, &cd, &use));
	CHECK(!strcmp(name, "Everyone") && !strcmp(domain, "") &&
	      use == SidTypeWellKnownGroup && cd == 0);
	/* Too small: both sizes needed, NULs included. */
	cn = 2;
	cd = sizeof(domain);
	CHECK(!LookupAccountSidA(NULL, s, name, &cn, domain, &cd, &use));
	CHECK(win_get_error() == ERROR_INSUFFICIENT_BUFFER && cn == 9 && cd == 1);
	local_free(s);
	CHECK(ConvertStringSidToSidA("S-1-5-5-0-1", &s));
	cn = sizeof(name);
	cd = sizeof(domain);
	CHECK(!LookupAccountSidA(NULL, s, name, &cn, domain, &cd, &use));
	CHECK_EQ(win_get_error(), ERROR_NONE_MAPPED);
	local_free(s);

	/* By name, with or without the domain, ignoring case. */
	cb = sizeof(sid);
	cd = sizeof(domain);
	CHECK(LookupAccountNameA(NULL, "administrators", sid, &cb, domain, &cd,
				 &use));
	CHECK_SID(sid, "S-1-5-32-544");
	CHECK(!strcmp(domain, "BUILTIN") && use == SidTypeAlias && cb == 16);
	cb = sizeof(sid);
	cd = sizeof(domain);
	CHECK(LookupAccountNameA(NULL, "NT AUTHORITY\\SYSTEM", sid, &cb, domain,
				 &cd, &use));
	CHECK_SID(sid, "S-1-5-18");
	char qualified[128];
	snprintf(qualified, sizeof(qualified), "seance\\%s", user());
	cb = sizeof(sid);
	cd = sizeof(domain);
	CHECK(LookupAccountNameA(NULL, qualified, sid, &cb, domain, &cd, &use));
	CHECK_SID(sid, MACHINE "-1000");
	cb = 0;
	cd = 0;
	CHECK(!LookupAccountNameA(NULL, "Users", NULL, &cb, NULL, &cd, &use));
	CHECK(win_get_error() == ERROR_INSUFFICIENT_BUFFER && cb == 16 && cd == 8);
	cb = sizeof(sid);
	cd = sizeof(domain);
	CHECK(!LookupAccountNameA(NULL, "BUILTIN\\SYSTEM", sid, &cb, domain, &cd,
				  &use));
	CHECK_EQ(win_get_error(), ERROR_NONE_MAPPED);
	const WCHAR none[] = { 'N', 'o', 'n', 'e', 0 };
	WCHAR wd[32];
	cb = sizeof(sid);
	cd = 32;
	CHECK(LookupAccountNameW(NULL, none, sid, &cb, wd, &cd, &use));
	CHECK(use == SidTypeGroup && cd == 6 && wd[0] == 'S' && wd[6] == 0);
	CHECK_SID(sid, MACHINE "-513");
}

static void test_acls(void)
{
	FN(BOOL, InitializeAcl, PACL, DWORD, DWORD);
	FN(BOOL, IsValidAcl, PACL);
	FN(BOOL, AddAccessAllowedAce, PACL, DWORD, DWORD, PSID);
	FN(BOOL, AddAccessDeniedAceEx, PACL, DWORD, DWORD, DWORD, PSID);
	FN(BOOL, AddAce, PACL, DWORD, DWORD, LPVOID, DWORD);
	FN(BOOL, DeleteAce, PACL, DWORD);
	FN(BOOL, GetAce, PACL, DWORD, LPVOID *);
	FN(BOOL, GetAclInformation, PACL, LPVOID, DWORD, ACL_INFORMATION_CLASS);
	FN(BOOL, CreateWellKnownSid, WELL_KNOWN_SID_TYPE, PSID, PSID, DWORD *);
	FN(BOOL, EqualSid, PSID, PSID);
	BYTE world[SECURITY_MAX_SID_SIZE], users[SECURITY_MAX_SID_SIZE];
	DWORD n1 = sizeof(world), n2 = sizeof(users);
	DWORD buf[64], small[5];
	ACL *acl = (ACL *)buf;
	ACL_SIZE_INFORMATION si;
	ACCESS_ALLOWED_ACE *ace;

	CreateWellKnownSid(WinWorldSid, NULL, world, &n1);
	CreateWellKnownSid(WinBuiltinUsersSid, NULL, users, &n2);
	CHECK(!InitializeAcl(acl, 4, ACL_REVISION));
	CHECK_EQ(win_get_error(), ERROR_INSUFFICIENT_BUFFER);
	CHECK(!InitializeAcl(acl, sizeof(buf), 9));
	CHECK(InitializeAcl(acl, sizeof(buf), ACL_REVISION) && IsValidAcl(acl));
	CHECK(AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_READ, world));
	CHECK(AddAccessDeniedAceEx(acl, ACL_REVISION, CONTAINER_INHERIT_ACE,
				   DELETE, users));
	CHECK(!AddAccessDeniedAceEx(acl, ACL_REVISION, 0x80, DELETE, users));
	CHECK(GetAclInformation(acl, &si, sizeof(si), AclSizeInformation));
	CHECK(si.AceCount == 2 && si.AclBytesInUse == 8 + 20 + 24 &&
	      si.AclBytesFree == sizeof(buf) - 52);
	CHECK(GetAce(acl, 1, (void **)&ace));
	CHECK(ace->Header.AceType == ACCESS_DENIED_ACE_TYPE &&
	      ace->Header.AceFlags == CONTAINER_INHERIT_ACE &&
	      ace->Header.AceSize == 24 && ace->Mask == DELETE &&
	      EqualSid(&ace->SidStart, users));
	CHECK(!GetAce(acl, 2, (void **)&ace));

	/* Inserting a copy of the first at the front, then deleting it. */
	BYTE first[20];
	GetAce(acl, 0, (void **)&ace);
	memcpy(first, ace, sizeof(first));
	CHECK(AddAce(acl, ACL_REVISION, 0, first, sizeof(first)));
	CHECK(GetAclInformation(acl, &si, sizeof(si), AclSizeInformation));
	CHECK(si.AceCount == 3 && si.AclBytesInUse == 72);
	CHECK(DeleteAce(acl, 0) && DeleteAce(acl, 0));
	CHECK(GetAce(acl, 0, (void **)&ace));
	CHECK_EQ(ace->Header.AceType, ACCESS_DENIED_ACE_TYPE);
	CHECK(IsValidAcl(acl));

	/* No room. */
	CHECK(InitializeAcl((ACL *)small, sizeof(small), ACL_REVISION));
	CHECK(!AddAccessAllowedAce((ACL *)small, ACL_REVISION, GENERIC_READ,
				   world));
	CHECK_EQ(win_get_error(), ERROR_ALLOTTED_SPACE_EXCEEDED);
	acl->AclSize = 6;
	CHECK(!IsValidAcl(acl));
}

static void test_descriptors(void)
{
	FN(BOOL, InitializeSecurityDescriptor, PSECURITY_DESCRIPTOR, DWORD);
	FN(BOOL, IsValidSecurityDescriptor, PSECURITY_DESCRIPTOR);
	FN(BOOL, SetSecurityDescriptorOwner, PSECURITY_DESCRIPTOR, PSID, BOOL);
	FN(BOOL, SetSecurityDescriptorGroup, PSECURITY_DESCRIPTOR, PSID, BOOL);
	FN(BOOL, SetSecurityDescriptorDacl, PSECURITY_DESCRIPTOR, BOOL, PACL, BOOL);
	FN(BOOL, GetSecurityDescriptorOwner, PSECURITY_DESCRIPTOR, PSID *, LPBOOL);
	FN(BOOL, GetSecurityDescriptorDacl, PSECURITY_DESCRIPTOR, LPBOOL, PACL *,
	   LPBOOL);
	FN(BOOL, GetSecurityDescriptorControl, PSECURITY_DESCRIPTOR,
	   PSECURITY_DESCRIPTOR_CONTROL, LPDWORD);
	FN(DWORD, GetSecurityDescriptorLength, PSECURITY_DESCRIPTOR);
	FN(BOOL, MakeSelfRelativeSD, PSECURITY_DESCRIPTOR, PSECURITY_DESCRIPTOR,
	   LPDWORD);
	FN(BOOL, InitializeAcl, PACL, DWORD, DWORD);
	FN(BOOL, AddAccessAllowedAce, PACL, DWORD, DWORD, PSID);
	FN(BOOL, CreateWellKnownSid, WELL_KNOWN_SID_TYPE, PSID, PSID, DWORD *);
	FN(BOOL, EqualSid, PSID, PSID);
	SECURITY_DESCRIPTOR sd;
	BYTE owner[SECURITY_MAX_SID_SIZE], group[SECURITY_MAX_SID_SIZE];
	DWORD n1 = sizeof(owner), n2 = sizeof(group), acl_buf[16];
	ACL *acl = (ACL *)acl_buf;

	CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, owner, &n1);
	CreateWellKnownSid(WinLocalSystemSid, NULL, group, &n2);
	InitializeAcl(acl, sizeof(acl_buf), ACL_REVISION);
	AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, owner);

	CHECK(!InitializeSecurityDescriptor(&sd, 2));
	CHECK_EQ(win_get_error(), ERROR_UNKNOWN_REVISION);
	CHECK(InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION));
	CHECK(IsValidSecurityDescriptor(&sd));
	CHECK_EQ(GetSecurityDescriptorLength(&sd), 20);
	CHECK(SetSecurityDescriptorOwner(&sd, owner, FALSE));
	CHECK(SetSecurityDescriptorGroup(&sd, group, TRUE));
	CHECK(SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE));
	CHECK_EQ(GetSecurityDescriptorLength(&sd), 20 + 16 + 12 + 64);

	/* The self-relative form: offsets, and read back the same way. */
	BYTE rel[256];
	DWORD len = 8;
	CHECK(!MakeSelfRelativeSD(&sd, rel, &len));
	CHECK(win_get_error() == ERROR_INSUFFICIENT_BUFFER && len == 112);
	CHECK(MakeSelfRelativeSD(&sd, rel, &len));
	SECURITY_DESCRIPTOR_CONTROL c;
	DWORD rev;
	CHECK(GetSecurityDescriptorControl(rel, &c, &rev));
	CHECK(rev == 1 && (c & SE_SELF_RELATIVE) && (c & SE_DACL_PRESENT) &&
	      (c & SE_GROUP_DEFAULTED) && !(c & SE_OWNER_DEFAULTED));
	PSID o = NULL;
	BOOL def = TRUE, present = FALSE;
	PACL d = NULL;
	CHECK(GetSecurityDescriptorOwner(rel, &o, &def));
	CHECK(o == rel + 20 && EqualSid(o, owner) && !def);
	CHECK(GetSecurityDescriptorDacl(rel, &present, &d, &def));
	CHECK(present && d && d->AceCount == 1 && d->AclSize == 64);
	CHECK_EQ(GetSecurityDescriptorLength(rel), 112);
	CHECK(!SetSecurityDescriptorOwner(rel, owner, FALSE));
	CHECK_EQ(win_get_error(), ERROR_INVALID_SECURITY_DESCR);
	/* A NULL DACL: present, and none. */
	CHECK(SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE));
	CHECK(GetSecurityDescriptorDacl(&sd, &present, &d, &def));
	CHECK(present && !d);
}

static void test_objects(void)
{
	FN(DWORD, GetNamedSecurityInfoA, LPSTR, SE_OBJECT_TYPE,
	   SECURITY_INFORMATION, PSID *, PSID *, PACL *, PACL *,
	   PSECURITY_DESCRIPTOR *);
	FN(DWORD, SetNamedSecurityInfoA, LPSTR, SE_OBJECT_TYPE,
	   SECURITY_INFORMATION, PSID, PSID, PACL, PACL);
	FN(BOOL, GetFileSecurityA, LPCSTR, SECURITY_INFORMATION,
	   PSECURITY_DESCRIPTOR, DWORD, LPDWORD);
	FN(BOOL, SetFileSecurityA, LPCSTR, SECURITY_INFORMATION,
	   PSECURITY_DESCRIPTOR);
	FN(DWORD, GetSecurityInfo, HANDLE, SE_OBJECT_TYPE, SECURITY_INFORMATION,
	   PSID *, PSID *, PACL *, PACL *, PSECURITY_DESCRIPTOR *);
	PSID owner = NULL, group = NULL;
	PACL dacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	char path[400];

	snprintf(path, sizeof(path), "%s/drive_c", dir);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/drive_c/sec.txt", dir);
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	if (f)
		fclose(f);

	CHECK_EQ(GetNamedSecurityInfoA("C:\\sec.txt", SE_FILE_OBJECT,
				       OWNER_SECURITY_INFORMATION |
				       DACL_SECURITY_INFORMATION, &owner, &group,
				       &dacl, NULL, &sd), 0);
	CHECK_SID(owner, "S-1-5-32-544");
	CHECK(!group && dacl && dacl->AceCount == 4);
	ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)(dacl + 1);
	CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
	      ace->Mask == 0x1f01ff);
	CHECK_SID(&ace->SidStart, "S-1-5-18");
	local_free(sd);
	CHECK_EQ(GetNamedSecurityInfoA("C:\\nope.txt", SE_FILE_OBJECT,
				       OWNER_SECURITY_INFORMATION, &owner, NULL,
				       NULL, NULL, &sd), ERROR_FILE_NOT_FOUND);
	CHECK_EQ(GetNamedSecurityInfoA("C:\\sec.txt", SE_FILE_OBJECT,
				       OWNER_SECURITY_INFORMATION, &owner, NULL,
				       NULL, NULL, NULL), ERROR_INVALID_PARAMETER);
	CHECK_EQ(GetNamedSecurityInfoA("MACHINE\\Software", SE_REGISTRY_KEY,
				       GROUP_SECURITY_INFORMATION, NULL, &group,
				       NULL, NULL, &sd), 0);
	CHECK_SID(group, MACHINE "-513");
	local_free(sd);
	CHECK_EQ(GetNamedSecurityInfoA("MACHINE\\No\\Such", SE_REGISTRY_KEY,
				       GROUP_SECURITY_INFORMATION, NULL, &group,
				       NULL, NULL, &sd), ERROR_FILE_NOT_FOUND);
	CHECK_EQ(SetNamedSecurityInfoA("C:\\sec.txt", SE_FILE_OBJECT,
				       DACL_SECURITY_INFORMATION, NULL, NULL,
				       NULL, NULL), 0);
	CHECK_EQ(GetSecurityInfo((HANDLE)(LONG_PTR)-1, SE_KERNEL_OBJECT,
				 OWNER_SECURITY_INFORMATION, &owner, NULL, NULL,
				 NULL, &sd), 0);
	local_free(sd);

	BYTE buf[256];
	DWORD need = 0;
	CHECK(!GetFileSecurityA("C:\\sec.txt", DACL_SECURITY_INFORMATION, buf, 8,
				&need));
	CHECK(win_get_error() == ERROR_INSUFFICIENT_BUFFER && need > 20 &&
	      need <= sizeof(buf));
	CHECK(GetFileSecurityA("C:\\sec.txt", DACL_SECURITY_INFORMATION, buf,
			       sizeof(buf), &need));
	CHECK(SetFileSecurityA("C:\\sec.txt", DACL_SECURITY_INFORMATION, buf));
	CHECK(!GetFileSecurityA("C:\\nope.txt", DACL_SECURITY_INFORMATION, buf,
				sizeof(buf), &need));
	CHECK_EQ(win_get_error(), ERROR_FILE_NOT_FOUND);
}

static void test_crypt(void)
{
	FN(BOOL, CryptAcquireContextA, HCRYPTPROV *, LPCSTR, LPCSTR, DWORD, DWORD);
	FN(BOOL, CryptReleaseContext, HCRYPTPROV, DWORD);
	FN(BOOL, CryptContextAddRef, HCRYPTPROV, DWORD *, DWORD);
	FN(BOOL, CryptGenRandom, HCRYPTPROV, DWORD, BYTE *);
	FN(BOOLEAN, SystemFunction036, PVOID, ULONG);
	HCRYPTPROV p = 0;
	BYTE a[32], b[32], zero[32] = { 0 };

	CHECK(!CryptAcquireContextA(&p, NULL, NULL, 99, 0));
	CHECK_EQ(win_get_error(), (DWORD)NTE_BAD_PROV_TYPE);
	CHECK(!CryptAcquireContextA(&p, NULL, NULL, PROV_RSA_FULL, 0x4));
	CHECK_EQ(win_get_error(), (DWORD)NTE_BAD_FLAGS);
	CHECK(CryptAcquireContextA(&p, NULL, NULL, PROV_RSA_FULL,
				   CRYPT_VERIFYCONTEXT) && p);
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	CHECK(CryptGenRandom(p, sizeof(a), a) && CryptGenRandom(p, sizeof(b), b));
	CHECK(memcmp(a, b, sizeof(a)) && memcmp(a, zero, sizeof(a)));
	CHECK(!CryptGenRandom(0, sizeof(a), a));
	CHECK(!CryptReleaseContext(p, 1));
	CHECK_EQ(win_get_error(), (DWORD)NTE_BAD_FLAGS);
	CHECK(CryptContextAddRef(p, NULL, 0));
	CHECK(CryptReleaseContext(p, 0));
	CHECK(CryptGenRandom(p, 4, a));	/* still referenced */
	CHECK(CryptReleaseContext(p, 0));
	/* Released, and never a provider: refused, not followed. */
	CHECK(!CryptGenRandom(p, 4, a));
	CHECK_EQ(win_get_error(), ERROR_INVALID_PARAMETER);
	CHECK(!CryptReleaseContext(p, 0));
	CHECK(!CryptGenRandom((HCRYPTPROV)0x1234, 4, a));
	CHECK(!CryptContextAddRef((HCRYPTPROV)0x1234, NULL, 0));
	HCRYPTPROV gone = 1;
	CHECK(CryptAcquireContextA(&gone, "c", NULL, PROV_RSA_AES,
				   CRYPT_DELETEKEYSET) && !gone);
	memset(a, 0, sizeof(a));
	CHECK(SystemFunction036(a, sizeof(a)) && memcmp(a, zero, sizeof(a)));
}

/*
 * With the handle table full, what fails to get a handle is left intact:
 * the process token, a key, an event a watch would have signalled.
 */
static void test_handles_full(void)
{
	FN(BOOL, OpenProcessToken, HANDLE, DWORD, PHANDLE);
	FN(BOOL, GetTokenInformation, HANDLE, TOKEN_INFORMATION_CLASS, LPVOID,
	   DWORD, PDWORD);
	FN(LONG, RegCreateKeyExA, HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
	   void *, HKEY *, LPDWORD);
	FN(LONG, RegOpenKeyExA, HKEY, LPCSTR, DWORD, REGSAM, HKEY *);
	FN(LONG, RegNotifyChangeKeyValue, HKEY, BOOL, DWORD, HANDLE, BOOL);
	FN(LONG, RegCloseKey, HKEY);
	HANDLE (WINAPI *create_event)(void *, BOOL, BOOL, LPCSTR) =
		get_from(k32, "CreateEventA");
	DWORD (WINAPI *wait)(HANDLE, DWORD) =
		get_from(k32, "WaitForSingleObject");
	HANDLE tok = NULL, ev = create_event(NULL, TRUE, FALSE, NULL);
	HKEY key = NULL, k2 = NULL;
	BYTE buf[256];
	DWORD len;

	CHECK_EQ(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Full", 0, NULL,
				 0, KEY_ALL_ACCESS, NULL, &key, NULL), 0);
	size_t cap = 70000, n = 0;
	HANDLE *fill = malloc(cap * sizeof(*fill));
	while (fill && n < cap && (fill[n] = create_event(NULL, TRUE, FALSE,
							  NULL)))
		n++;
	CHECK(n > 0 && n < cap);

	CHECK(!OpenProcessToken((HANDLE)(LONG_PTR)-1, TOKEN_QUERY, &tok));
	CHECK(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Full", 0, KEY_READ,
			    &k2) != ERROR_SUCCESS && !k2);
	CHECK_EQ(RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
					 ev, TRUE), ERROR_TOO_MANY_OPEN_FILES);

	while (n)
		close_handle(fill[--n]);
	free(fill);
	CHECK_EQ(wait(ev, 0), WAIT_TIMEOUT);	/* the event is still there */
	CHECK(OpenProcessToken((HANDLE)(LONG_PTR)-1, TOKEN_QUERY, &tok));
	CHECK(GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len));
	close_handle(tok);
	close_handle(ev);
	RegCloseKey(key);
}

/*
 * SetEntriesInAcl, as LEAD's DSKernel2 uses it to open its registry key
 * to Everyone: entries by SID and by name, applied to an existing ACL.
 */
static void test_entries(void)
{
	FN(BOOL, CreateWellKnownSid, WELL_KNOWN_SID_TYPE, PSID, PSID, DWORD *);
	FN(void, BuildTrusteeWithSidA, PTRUSTEE_A, PSID);
	FN(void, BuildExplicitAccessWithNameA, PEXPLICIT_ACCESS_A, LPSTR, DWORD,
	   ACCESS_MODE, DWORD);
	FN(DWORD, SetEntriesInAclA, ULONG, PEXPLICIT_ACCESS_A, PACL, PACL *);
	FN(DWORD, SetEntriesInAclW, ULONG, PEXPLICIT_ACCESS_W, PACL, PACL *);
	FN(TRUSTEE_FORM, GetTrusteeFormA, PTRUSTEE_A);
	FN(BOOL, GetAce, PACL, DWORD, LPVOID *);
	FN(BOOL, InitializeAcl, PACL, DWORD, DWORD);
	FN(BOOL, IsValidAcl, PACL);
	FN(BOOL, EqualSid, PSID, PSID);
	BYTE world[SECURITY_MAX_SID_SIZE], users[SECURITY_MAX_SID_SIZE];
	DWORD n1 = sizeof(world), n2 = sizeof(users);
	EXPLICIT_ACCESS_A ea[3];
	PACL acl = NULL, acl2 = NULL;
	ACCESS_ALLOWED_ACE *ace;

	CreateWellKnownSid(WinWorldSid, NULL, world, &n1);
	CreateWellKnownSid(WinBuiltinUsersSid, NULL, users, &n2);
	memset(ea, 0, sizeof(ea));
	ea[0].grfAccessPermissions = KEY_ALL_ACCESS;
	ea[0].grfAccessMode = SET_ACCESS;
	ea[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	BuildTrusteeWithSidA(&ea[0].Trustee, world);
	CHECK_EQ(GetTrusteeFormA(&ea[0].Trustee), TRUSTEE_IS_SID);
	CHECK_EQ(SetEntriesInAclA(1, ea, NULL, &acl), 0);
	CHECK(acl && IsValidAcl(acl));
	if (!acl)
		return;
	CHECK_EQ(acl->AceCount, 1);
	CHECK(GetAce(acl, 0, (void **)&ace));
	CHECK_EQ(ace->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
	CHECK_EQ(ace->Header.AceFlags, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
	CHECK_EQ(ace->Mask, KEY_ALL_ACCESS);
	CHECK(EqualSid(&ace->SidStart, world));

	/* On top of it: a deny by name goes first, a grant after the
	 * denies, ahead of the existing allow. */
	BuildExplicitAccessWithNameA(&ea[0], "Users", DELETE, DENY_ACCESS, 0);
	BuildExplicitAccessWithNameA(&ea[1], "CURRENT_USER", GENERIC_READ,
				     GRANT_ACCESS, NO_INHERITANCE);
	CHECK_EQ(SetEntriesInAclA(2, ea, acl, &acl2), 0);
	CHECK(acl2 && IsValidAcl(acl2));
	if (acl2) {
		CHECK_EQ(acl2->AceCount, 3);
		CHECK(GetAce(acl2, 0, (void **)&ace));
		CHECK_EQ(ace->Header.AceType, ACCESS_DENIED_ACE_TYPE);
		CHECK(EqualSid(&ace->SidStart, users));
		CHECK(GetAce(acl2, 1, (void **)&ace));
		CHECK_EQ(ace->Mask, GENERIC_READ);
		CHECK(GetAce(acl2, 2, (void **)&ace));
		CHECK(EqualSid(&ace->SidStart, world));
		local_free(acl2);
	}

	/* The result is sized to its ACEs, not to the old ACL's room: an
	 * empty ACL of the largest size takes a grant, and setting the same
	 * trustee again doesn't grow it. */
	{
		ACL *big = calloc(1, 0xfffc), *a1 = NULL, *a2 = NULL;

		CHECK(big && InitializeAcl(big, 0xfffc, ACL_REVISION));
		ea[0].grfAccessPermissions = KEY_READ;
		ea[0].grfAccessMode = SET_ACCESS;
		ea[0].grfInheritance = 0;
		BuildTrusteeWithSidA(&ea[0].Trustee, world);
		CHECK_EQ(SetEntriesInAclA(1, ea, big, &a1), 0);
		CHECK(a1 && a1->AceCount == 1 && a1->AclSize < 64);
		if (a1) {
			CHECK_EQ(SetEntriesInAclA(1, ea, a1, &a2), 0);
			CHECK(a2 && a2->AceCount == 1 &&
			      a2->AclSize == a1->AclSize);
			if (a2)
				local_free(a2);
			local_free(a1);
		}
		free(big);
	}

	/* Revoked: the trustee's ACEs go. */
	acl2 = NULL;
	ea[0].grfAccessMode = REVOKE_ACCESS;
	BuildTrusteeWithSidA(&ea[0].Trustee, world);
	CHECK_EQ(SetEntriesInAclA(1, ea, acl, &acl2), 0);
	CHECK(acl2 && acl2->AceCount == 0);
	if (acl2)
		local_free(acl2);

	/* Wide names; a name nobody has. */
	{
		static const WCHAR every[] = { 'E', 'v', 'e', 'r', 'y', 'o', 'n',
					       'e', 0 };
		EXPLICIT_ACCESS_W w;

		memset(&w, 0, sizeof(w));
		w.grfAccessPermissions = KEY_READ;
		w.grfAccessMode = GRANT_ACCESS;
		w.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
		w.Trustee.ptstrName = (LPWSTR)every;
		acl2 = NULL;
		CHECK_EQ(SetEntriesInAclW(1, &w, NULL, &acl2), 0);
		CHECK(acl2 && acl2->AceCount == 1);
		if (acl2) {
			CHECK(GetAce(acl2, 0, (void **)&ace));
			CHECK(EqualSid(&ace->SidStart, world));
			local_free(acl2);
		}
	}
	BuildExplicitAccessWithNameA(&ea[0], "Nobody At All", KEY_READ,
				     GRANT_ACCESS, 0);
	acl2 = (PACL)1;
	CHECK_EQ(SetEntriesInAclA(1, ea, NULL, &acl2), ERROR_NONE_MAPPED);
	CHECK(acl2 == NULL);
	local_free(acl);
}

/* SDDL text for a descriptor, parsed and written back. */
static char *sddl_round(const char *in, SECURITY_INFORMATION info)
{
	FN(BOOL, ConvertStringSecurityDescriptorToSecurityDescriptorA, LPCSTR,
	   DWORD, PSECURITY_DESCRIPTOR *, ULONG *);
	FN(BOOL, ConvertSecurityDescriptorToStringSecurityDescriptorA,
	   PSECURITY_DESCRIPTOR, DWORD, SECURITY_INFORMATION, LPSTR *, ULONG *);
	PSECURITY_DESCRIPTOR sd = NULL;
	ULONG size = 0, len = 0;
	LPSTR out = NULL;

	if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(in, 1, &sd,
								 &size))
		return strdup("parse failed");
	if (!ConvertSecurityDescriptorToStringSecurityDescriptorA(sd, 1, info,
								 &out, &len)) {
		local_free(sd);
		return strdup("print failed");
	}
	char *r = strdup(out);
	CHECK_EQ(len, strlen(out) + 1);
	local_free(out);
	local_free(sd);
	return r;
}

#define ALL_INFO (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | \
		  DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION)

#define CHECK_SDDL(in, info, want) do { \
	char *r_ = sddl_round(in, info); \
	CHECK_STR(r_, want); \
	free(r_); \
} while (0)

static void test_sddl(void)
{
	FN(BOOL, ConvertStringSecurityDescriptorToSecurityDescriptorA, LPCSTR,
	   DWORD, PSECURITY_DESCRIPTOR *, ULONG *);
	FN(BOOL, ConvertStringSecurityDescriptorToSecurityDescriptorW, LPCWSTR,
	   DWORD, PSECURITY_DESCRIPTOR *, ULONG *);
	FN(BOOL, ConvertSecurityDescriptorToStringSecurityDescriptorW,
	   PSECURITY_DESCRIPTOR, DWORD, SECURITY_INFORMATION, LPWSTR *, ULONG *);
	FN(BOOL, GetSecurityDescriptorDacl, PSECURITY_DESCRIPTOR, LPBOOL, PACL *,
	   LPBOOL);
	FN(BOOL, GetSecurityDescriptorOwner, PSECURITY_DESCRIPTOR, PSID *, LPBOOL);
	FN(BOOL, GetSecurityDescriptorControl, PSECURITY_DESCRIPTOR,
	   PSECURITY_DESCRIPTOR_CONTROL, LPDWORD);
	FN(DWORD, GetSecurityDescriptorLength, PSECURITY_DESCRIPTOR);
	FN(BOOL, GetAce, PACL, DWORD, LPVOID *);
	PSECURITY_DESCRIPTOR sd = NULL;
	ULONG size = 0;

	/* The parts, read back through the descriptor functions. */
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:BAG:SYD:P(A;OICI;FA;;;BA)(D;;0x1f;;;WD)", 1, &sd, &size));
	if (sd) {
		BOOL present = 0, def = 1;
		PACL dacl = NULL;
		PSID owner = NULL;
		SECURITY_DESCRIPTOR_CONTROL c = 0;
		DWORD rev = 0;
		CHECK_EQ(GetSecurityDescriptorLength(sd), size);
		CHECK(GetSecurityDescriptorControl(sd, &c, &rev));
		CHECK_EQ(c, SE_SELF_RELATIVE | SE_DACL_PRESENT | SE_DACL_PROTECTED);
		CHECK(GetSecurityDescriptorOwner(sd, &owner, &def));
		CHECK_SID(owner, "S-1-5-32-544");
		CHECK(GetSecurityDescriptorDacl(sd, &present, &dacl, &def));
		CHECK(present && dacl && dacl->AceCount == 2 &&
		      dacl->AclRevision == ACL_REVISION);
		ACCESS_ALLOWED_ACE *a = NULL;
		CHECK(GetAce(dacl, 0, (void **)&a));
		CHECK(a && a->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
		      a->Header.AceFlags == (OBJECT_INHERIT_ACE |
					     CONTAINER_INHERIT_ACE) &&
		      a->Mask == 0x1f01ff);
		CHECK(GetAce(dacl, 1, (void **)&a));
		CHECK(a && a->Header.AceType == ACCESS_DENIED_ACE_TYPE &&
		      a->Mask == 0x1f);
		CHECK_SID(&a->SidStart, "S-1-1-0");
		local_free(sd);
	}

	/* Round trips: rights as codes, hex where they aren't. */
	CHECK_SDDL("O:BAG:SYD:P(A;OICI;FA;;;BA)(D;;0x1f;;;WD)", ALL_INFO,
		   "O:BAG:SYD:P(A;OICI;FA;;;BA)(D;;CCDCLCSWRP;;;WD)");
	CHECK_SDDL("D:(A;;GRGX;;;AU)(A;;0x100000;;;BU)(A;;KR;;;SY)", ALL_INFO,
		   "D:(A;;GRGX;;;AU)(A;;0x100000;;;BU)(A;;KR;;;SY)");
	CHECK_SDDL("O:S-1-5-21-1-2-3-1000G:S-1-5-32-545D:AI(A;ID;0x1200a9;;;"
		   "S-1-5-21-1-2-3-1000)", ALL_INFO,
		   "O:S-1-5-21-1-2-3-1000G:BUD:AI(A;ID;0x1200a9;;;"
		   "S-1-5-21-1-2-3-1000)");
	/* Only what's asked for; a SACL with audits and a label. */
	CHECK_SDDL("O:BAG:BAD:(A;;FA;;;WD)S:(AU;SAFA;FA;;;WD)(ML;;NWNR;;;HI)",
		   DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION,
		   "D:(A;;FA;;;WD)S:(AU;SAFA;FA;;;WD)(ML;;NWNR;;;HI)");
	CHECK_SDDL("O:BA", GROUP_SECURITY_INFORMATION, "");
	/* A NULL DACL, and an empty one. */
	CHECK_SDDL("D:NO_ACCESS_CONTROL", ALL_INFO, "D:NO_ACCESS_CONTROL");
	CHECK_SDDL("D:P", ALL_INFO, "D:P");
	/* An object ACE keeps its GUIDs (and the ACL is a DS one). */
	CHECK_SDDL("D:(OA;CI;RPWP;bf967aba-0de6-11d0-a285-00aa003049e2;;AU)",
		   ALL_INFO,
		   "D:(OA;CI;RPWP;bf967aba-0de6-11d0-a285-00aa003049e2;;AU)");
	CHECK_SDDL("D:(OA;;CR;;4828cc14-1437-45bc-9b07-ad6f015e5f28;WD)",
		   ALL_INFO,
		   "D:(OA;;CR;;4828cc14-1437-45bc-9b07-ad6f015e5f28;WD)");

	/* Bad text. */
	static const char *const bad[] = {
		"X:BA", "O:ZZ", "O:BAO:SY", "D:(A;;FA;;;BA", "D:(Q;;FA;;;BA)",
		"D:(A;XX;FA;;;BA)", "D:(A;;QQ;;;BA)", "D:(A;;FA;;BA)",
		"D:(A;;FA;bf967aba;;BA)",
		"D:(A;;FA;bf967aba-0de6-11d0-a285-00aa003049e2;;BA)",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		sd = (void *)1;
		CHECK(!ConvertStringSecurityDescriptorToSecurityDescriptorA(
			bad[i], 1, &sd, NULL));
	}
	CHECK(!ConvertStringSecurityDescriptorToSecurityDescriptorA("O:BA", 2,
								    &sd, NULL));

	/* W forms. */
	static const WCHAR in[] = { 'O', ':', 'S', 'Y', 'D', ':', '(', 'A',
				    ';', ';', 'G', 'A', ';', ';', ';', 'W',
				    'D', ')', 0 };
	sd = NULL;
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(in, 1, &sd,
								   NULL));
	LPWSTR w = NULL;
	ULONG len = 0;
	CHECK(sd && ConvertSecurityDescriptorToStringSecurityDescriptorW(
		sd, 1, ALL_INFO, &w, &len));
	CHECK(w && len == 19 && !memcmp(w, in, sizeof(in)));
	local_free(w);
	local_free(sd);
}

#define FILE_READ_DATA  0x1
#define FILE_WRITE_DATA 0x2

static void test_access_check(void)
{
	FN(BOOL, ImpersonateSelf, SECURITY_IMPERSONATION_LEVEL);
	FN(BOOL, RevertToSelf, void);
	FN(BOOL, OpenThreadToken, HANDLE, DWORD, BOOL, PHANDLE);
	FN(BOOL, OpenProcessToken, HANDLE, DWORD, PHANDLE);
	FN(BOOL, AccessCheck, PSECURITY_DESCRIPTOR, HANDLE, DWORD,
	   PGENERIC_MAPPING, PPRIVILEGE_SET, LPDWORD, LPDWORD, LPBOOL);
	FN(void, MapGenericMask, PDWORD, PGENERIC_MAPPING);
	FN(BOOL, AreAllAccessesGranted, DWORD, DWORD);
	FN(BOOL, AreAnyAccessesGranted, DWORD, DWORD);
	FN(BOOL, ConvertStringSecurityDescriptorToSecurityDescriptorA, LPCSTR,
	   DWORD, PSECURITY_DESCRIPTOR *, ULONG *);
	FN(BOOL, AdjustTokenPrivileges, HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD,
	   PTOKEN_PRIVILEGES, PDWORD);
	FN(BOOL, GetSecurityDescriptorDacl, PSECURITY_DESCRIPTOR, LPBOOL, PACL *,
	   LPBOOL);
	FN(BOOL, ConvertSecurityDescriptorToStringSecurityDescriptorA,
	   PSECURITY_DESCRIPTOR, DWORD, SECURITY_INFORMATION, LPSTR *, ULONG *);
	DWORD (WINAPI *last_error)(void) = get_from(k32, "GetLastError");
	GENERIC_MAPPING map = { 0x120089, 0x120116, 0x1200a0, 0x1f01ff };
	DWORD m = GENERIC_READ | DELETE;

	MapGenericMask(&m, &map);
	CHECK_EQ(m, 0x120089 | DELETE);
	CHECK(AreAllAccessesGranted(7, 3) && !AreAllAccessesGranted(3, 7));
	CHECK(AreAnyAccessesGranted(1, 3) && !AreAnyAccessesGranted(4, 3));

	HANDLE proc = NULL, tok = NULL;
	BYTE privbuf[64];
	PPRIVILEGE_SET privs = (PPRIVILEGE_SET)privbuf;
	DWORD plen = sizeof(privbuf), granted = 0;
	BOOL status = 0;
	PSECURITY_DESCRIPTOR sd = NULL;

	/* A primary token won't do. */
	CHECK(OpenProcessToken((HANDLE)(LONG_PTR)-1, TOKEN_QUERY, &proc));
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:SYG:SYD:(A;;FR;;;BU)(D;;FW;;;BA)(A;;FA;;;WD)", 1, &sd, NULL));
	CHECK(!AccessCheck(sd, proc, FILE_READ_DATA, &map, privs, &plen,
			   &granted, &status));
	CHECK_EQ(last_error(), ERROR_NO_IMPERSONATION_TOKEN);
	close_handle(proc);

	CHECK(ImpersonateSelf(SecurityImpersonation));
	CHECK(OpenThreadToken((HANDLE)(LONG_PTR)-2, TOKEN_QUERY |
			      TOKEN_ADJUST_PRIVILEGES, TRUE, &tok));
	/* Read through Users; write denied to Administrators first. */
	CHECK(AccessCheck(sd, tok, FILE_READ_DATA, &map, privs, &plen,
			  &granted, &status));
	CHECK(status && granted == FILE_READ_DATA);
	CHECK(AccessCheck(sd, tok, FILE_WRITE_DATA, &map, privs, &plen,
			  &granted, &status));
	CHECK(!status && !granted && last_error() == ERROR_ACCESS_DENIED);
	/* Everything allowed but what was denied first. */
	CHECK(AccessCheck(sd, tok, MAXIMUM_ALLOWED, &map, privs, &plen,
			  &granted, &status));
	CHECK(status && granted == (0x1f01ff & ~(0x120116 & ~0x120089)));
	/* Generic rights must be mapped first. */
	CHECK(!AccessCheck(sd, tok, GENERIC_READ, &map, privs, &plen,
			   &granted, &status));
	CHECK_EQ(last_error(), ERROR_GENERIC_NOT_MAPPED);
	/* The privilege set must have room. */
	DWORD small = 4;
	CHECK(!AccessCheck(sd, tok, FILE_READ_DATA, &map, privs, &small,
			   &granted, &status));
	CHECK(last_error() == ERROR_INSUFFICIENT_BUFFER && small == 8);
	local_free(sd);

	/* No DACL: anything. The owner may read and change the DACL. */
	sd = NULL;
	BOOL converted = ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:BAG:BA", 1, &sd, NULL);
	CHECK(converted);
	if (converted) {
		CHECK(AccessCheck(sd, tok, DELETE | 0x1, &map, privs, &plen,
				  &granted, &status));
		CHECK(status && granted == (DELETE | 0x1));
		local_free(sd);
	}
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:BAG:BAD:", 1, &sd, NULL));
	CHECK(AccessCheck(sd, tok, READ_CONTROL | WRITE_DAC, &map, privs,
			  &plen, &granted, &status));
	CHECK(status && granted == (READ_CONTROL | WRITE_DAC));
	CHECK(AccessCheck(sd, tok, 0x1, &map, privs, &plen, &granted,
			  &status));
	CHECK(!status);
	/* ACCESS_SYSTEM_SECURITY: only with SeSecurityPrivilege on. */
	CHECK(AccessCheck(sd, tok, ACCESS_SYSTEM_SECURITY, &map, privs,
			  &plen, &granted, &status));
	CHECK(!status && last_error() == ERROR_PRIVILEGE_NOT_HELD);
	TOKEN_PRIVILEGES tp = { 1, { { { 8, 0 }, SE_PRIVILEGE_ENABLED } } };
	CHECK(AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL));
	plen = sizeof(privbuf);
	CHECK(AccessCheck(sd, tok, ACCESS_SYSTEM_SECURITY, &map, privs,
			  &plen, &granted, &status));
	CHECK(status && granted == ACCESS_SYSTEM_SECURITY &&
	      privs->PrivilegeCount == 1 &&
	      privs->Privilege[0].Luid.LowPart == 8);
	local_free(sd);
	/* Inherit-only ACEs don't count. */
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:SYG:SYD:(A;IO;FA;;;WD)", 1, &sd, NULL));
	CHECK(AccessCheck(sd, tok, FILE_READ_DATA, &map, privs, &plen,
			  &granted, &status));
	CHECK(!status);
	local_free(sd);

	/* An object ACE naming no object types is an ordinary one. */
	sd = NULL;
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:SYG:SYD:(OA;;FR;;;WD)", 1, &sd, NULL));
	if (sd) {
		PACL d = NULL;
		BOOL present = 0, dflt = 0;
		CHECK(GetSecurityDescriptorDacl(sd, &present, &d, &dflt) && d);
		CHECK(d && ((ACE_HEADER *)(d + 1))->AceType ==
		      ACCESS_ALLOWED_ACE_TYPE);
		CHECK(AccessCheck(sd, tok, FILE_READ_DATA, &map, privs, &plen,
				  &granted, &status));
		CHECK(status && granted == FILE_READ_DATA);
		/* A truncated ACE (no SID) makes the ACL invalid. */
		if (d) {
			((ACE_HEADER *)(d + 1))->AceSize = 8;
			CHECK(!AccessCheck(sd, tok, FILE_READ_DATA, &map, privs,
					   &plen, &granted, &status));
			CHECK_EQ(last_error(), ERROR_INVALID_ACL);
			LPSTR out = NULL;
			CHECK(!ConvertSecurityDescriptorToStringSecurityDescriptorA(
				sd, 1, DACL_SECURITY_INFORMATION, &out, NULL));
		}
		local_free(sd);
	}
	/* Every character of a GUID is checked. */
	sd = NULL;
	CHECK(!ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:SYG:SYD:(OA;;FR;bf967aba-0de6-11d0-a285-00aa003049eZ;;WD)", 1,
		&sd, NULL));
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:SYG:SYD:(OA;;FR;BF967ABA-0de6-11d0-a285-00aa003049e2;;WD)", 1,
		&sd, NULL));
	local_free(sd);
	/* OWNER RIGHTS (S-1-3-4) takes the place of the owner's own rights. */
	sd = NULL;
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:BAG:BAD:(A;;RC;;;S-1-3-4)", 1, &sd, NULL));
	if (sd) {
		CHECK(AccessCheck(sd, tok, READ_CONTROL, &map, privs, &plen,
				  &granted, &status));
		CHECK(status && granted == READ_CONTROL);
		CHECK(AccessCheck(sd, tok, WRITE_DAC, &map, privs, &plen,
				  &granted, &status));
		CHECK(!status);
		local_free(sd);
	}
	/* SeTakeOwnershipPrivilege grants WRITE_OWNER, and says so. */
	sd = NULL;
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"O:SYG:SYD:", 1, &sd, NULL));
	if (sd) {
		TOKEN_PRIVILEGES take = { 1, { { { 9, 0 }, SE_PRIVILEGE_ENABLED } } };
		CHECK(AdjustTokenPrivileges(tok, FALSE, &take, 0, NULL, NULL));
		DWORD tiny = 8;
		CHECK(!AccessCheck(sd, tok, WRITE_OWNER, &map, privs, &tiny,
				   &granted, &status));
		CHECK(last_error() == ERROR_INSUFFICIENT_BUFFER && tiny == 20);
		plen = sizeof(privbuf);
		CHECK(AccessCheck(sd, tok, WRITE_OWNER | ACCESS_SYSTEM_SECURITY,
				  &map, privs, &plen, &granted, &status));
		CHECK(status && privs->PrivilegeCount == 2 &&
		      privs->Privilege[0].Luid.LowPart == 8 &&
		      privs->Privilege[1].Luid.LowPart == 9);
		local_free(sd);
	}
	close_handle(tok);
	CHECK(RevertToSelf());
}

/* "a\0b\0\0" style lists from a string with | between. */
static void test_services(void)
{
	FN(SC_HANDLE, OpenSCManagerA, LPCSTR, LPCSTR, DWORD);
	FN(SC_HANDLE, OpenServiceA, SC_HANDLE, LPCSTR, DWORD);
	FN(SC_HANDLE, OpenServiceW, SC_HANDLE, LPCWSTR, DWORD);
	FN(SC_HANDLE, CreateServiceA, SC_HANDLE, LPCSTR, LPCSTR, DWORD, DWORD,
	   DWORD, DWORD, LPCSTR, LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR);
	FN(BOOL, CloseServiceHandle, SC_HANDLE);
	FN(BOOL, DeleteService, SC_HANDLE);
	FN(BOOL, StartServiceA, SC_HANDLE, DWORD, LPCSTR *);
	FN(BOOL, ControlService, SC_HANDLE, DWORD, LPSERVICE_STATUS);
	FN(BOOL, QueryServiceStatus, SC_HANDLE, LPSERVICE_STATUS);
	FN(BOOL, QueryServiceStatusEx, SC_HANDLE, DWORD, LPBYTE, DWORD, LPDWORD);
	FN(BOOL, QueryServiceConfigA, SC_HANDLE, LPQUERY_SERVICE_CONFIGA, DWORD,
	   LPDWORD);
	FN(BOOL, QueryServiceConfigW, SC_HANDLE, LPQUERY_SERVICE_CONFIGW, DWORD,
	   LPDWORD);
	FN(BOOL, ChangeServiceConfigA, SC_HANDLE, DWORD, DWORD, DWORD, LPCSTR,
	   LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR);
	FN(BOOL, GetServiceDisplayNameA, SC_HANDLE, LPCSTR, LPSTR, LPDWORD);
	FN(BOOL, GetServiceKeyNameA, SC_HANDLE, LPCSTR, LPSTR, LPDWORD);
	FN(BOOL, EnumServicesStatusA, SC_HANDLE, DWORD, DWORD,
	   LPENUM_SERVICE_STATUSA, DWORD, LPDWORD, LPDWORD, LPDWORD);
	FN(BOOL, EnumServicesStatusExW, SC_HANDLE, DWORD, DWORD, DWORD, LPBYTE,
	   DWORD, LPDWORD, LPDWORD, LPDWORD, LPCWSTR);
	FN(SC_LOCK, LockServiceDatabase, SC_HANDLE);
	FN(BOOL, UnlockServiceDatabase, SC_LOCK);
	FN(BOOL, StartServiceCtrlDispatcherA, const SERVICE_TABLE_ENTRYA *);
	DWORD (WINAPI *last_error)(void) = get_from(k32, "GetLastError");

	CHECK(!OpenSCManagerA("\\\\elsewhere", NULL, SC_MANAGER_CONNECT));
	CHECK_EQ(last_error(), ERROR_ACCESS_DENIED);
	/* This machine by any of its names. */
	char host[80] = "\\\\";
	DWORD hn = sizeof(host) - 2;
	BOOL (WINAPI *computer)(LPSTR, LPDWORD) =
		get_from(k32, "GetComputerNameA");
	CHECK(computer(host + 2, &hn));
	const char *locals[] = { ".", "\\\\.", "LOCALHOST", host, host + 2 };
	for (size_t i = 0; i < sizeof(locals) / sizeof(*locals); i++) {
		SC_HANDLE l = OpenSCManagerA(locals[i], NULL, SC_MANAGER_CONNECT);
		CHECK(l != NULL);
		if (l)
			CloseServiceHandle(l);
	}
	SC_HANDLE ro = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
	SC_HANDLE scm = OpenSCManagerA(NULL, "ServicesActive",
				       SC_MANAGER_ALL_ACCESS);
	CHECK(ro && scm);
	CHECK(!OpenServiceA(scm, "seancetest", SERVICE_ALL_ACCESS));
	CHECK_EQ(last_error(), ERROR_SERVICE_DOES_NOT_EXIST);
	CHECK(!CreateServiceA(ro, "seancetest", NULL, SERVICE_ALL_ACCESS,
			      SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
			      SERVICE_ERROR_NORMAL, "C:\\svc.exe", NULL, NULL,
			      NULL, NULL, NULL));
	CHECK_EQ(last_error(), ERROR_ACCESS_DENIED);
	/* No drivers. */
	CHECK(!CreateServiceA(scm, "seancedrv", NULL, SERVICE_ALL_ACCESS,
			      SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
			      SERVICE_ERROR_NORMAL, "drv.sys", NULL, NULL,
			      NULL, NULL, NULL));
	CHECK_EQ(last_error(), ERROR_ACCESS_DENIED);

	SC_HANDLE s = CreateServiceA(scm, "seancetest", "Seance Test",
				     SERVICE_ALL_ACCESS,
				     SERVICE_WIN32_OWN_PROCESS,
				     SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
				     "C:\\svc.exe -x", "grp", NULL,
				     "RpcSs\0Tcpip\0", NULL, NULL);
	CHECK(s != NULL);
	CHECK(!CreateServiceA(scm, "SEANCETEST", NULL, SERVICE_ALL_ACCESS,
			      SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
			      SERVICE_ERROR_NORMAL, "x.exe", NULL, NULL, NULL,
			      NULL, NULL));
	CHECK_EQ(last_error(), ERROR_SERVICE_EXISTS);
	CHECK(!CreateServiceA(scm, "other", "seance test", SERVICE_ALL_ACCESS,
			      SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
			      SERVICE_ERROR_NORMAL, "x.exe", NULL, NULL, NULL,
			      NULL, NULL));
	CHECK_EQ(last_error(), ERROR_DUPLICATE_SERVICE_NAME);

	/* Its configuration, A and W. */
	DWORD need = 0;
	CHECK(!QueryServiceConfigA(s, NULL, 0, &need));
	CHECK(last_error() == ERROR_INSUFFICIENT_BUFFER && need > 0);
	QUERY_SERVICE_CONFIGA *ca = malloc(need);
	int queried = ca && QueryServiceConfigA(s, ca, need, &need);
	CHECK(queried);
	if (queried) {
		CHECK_EQ(ca->dwServiceType, SERVICE_WIN32_OWN_PROCESS);
		CHECK_EQ(ca->dwStartType, SERVICE_DEMAND_START);
		CHECK_EQ(ca->dwErrorControl, SERVICE_ERROR_NORMAL);
		CHECK_STR(ca->lpBinaryPathName, "C:\\svc.exe -x");
		CHECK_STR(ca->lpLoadOrderGroup, "grp");
		CHECK(!memcmp(ca->lpDependencies, "RpcSs\0Tcpip\0", 13));
		CHECK_STR(ca->lpServiceStartName, "LocalSystem");
		CHECK_STR(ca->lpDisplayName, "Seance Test");
	}
	free(ca);
	SC_HANDLE s2 = OpenServiceW(scm, (const WCHAR[]){ 'S', 'e', 'a', 'n',
				    'c', 'e', 'T', 'e', 's', 't', 0 },
				    SERVICE_QUERY_CONFIG);
	CHECK(s2 != NULL);
	need = 0;
	CHECK(!QueryServiceConfigW(s2, NULL, 0, &need) && need > 0);
	QUERY_SERVICE_CONFIGW *cw = need ? malloc(need) : NULL;
	queried = cw && QueryServiceConfigW(s2, cw, need, &need);
	CHECK(queried);
	if (queried) {
		static const WCHAR path[] = { 'C', ':', '\\', 's', 'v', 'c', '.',
					      'e', 'x', 'e', ' ', '-', 'x', 0 };
		CHECK(!memcmp(cw->lpBinaryPathName, path, sizeof(path)));
		CHECK(((ULONG_PTR)cw->lpDisplayName & 1) == 0);
	}
	free(cw);
	/* Generic rights stand for the specific ones. */
	SC_HANDLE gr = OpenServiceA(scm, "seancetest", GENERIC_READ);
	SERVICE_STATUS gst;
	CHECK(gr && QueryServiceStatus(gr, &gst));
	if (gr)
		CloseServiceHandle(gr);
	SC_HANDLE gscm = OpenSCManagerA(NULL, NULL, GENERIC_READ);
	SC_HANDLE nope = gscm ? CreateServiceA(gscm, "seancetest2", NULL,
		SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
		SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, "C:\\x.exe", NULL,
		NULL, NULL, NULL, NULL) : NULL;
	CHECK(gscm && !nope && last_error() == ERROR_ACCESS_DENIED);
	if (gscm)
		CloseServiceHandle(gscm);
	/* Opened for config only: no status. */
	SERVICE_STATUS st;
	CHECK(!QueryServiceStatus(s2, &st));
	CHECK_EQ(last_error(), ERROR_ACCESS_DENIED);
	CHECK(CloseServiceHandle(s2));

	/* Names both ways. */
	char name[64];
	DWORD n = sizeof(name);
	CHECK(GetServiceDisplayNameA(scm, "seancetest", name, &n));
	CHECK(!strcmp(name, "Seance Test") && n == 11);
	n = 4;
	CHECK(!GetServiceKeyNameA(scm, "SEANCE TEST", name, &n));
	CHECK(last_error() == ERROR_INSUFFICIENT_BUFFER && n == 10);
	n = sizeof(name);
	CHECK(GetServiceKeyNameA(scm, "SEANCE TEST", name, &n));
	CHECK_STR(name, "seancetest");

	/* Stopped, and never starts. */
	CHECK(QueryServiceStatus(s, &st));
	CHECK(st.dwServiceType == SERVICE_WIN32_OWN_PROCESS &&
	      st.dwCurrentState == SERVICE_STOPPED);
	SERVICE_STATUS_PROCESS sp;
	CHECK(QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (BYTE *)&sp,
				   sizeof(sp), &need));
	CHECK(sp.dwCurrentState == SERVICE_STOPPED && !sp.dwProcessId);
	CHECK(!StartServiceA(s, 0, NULL));
	CHECK_EQ(last_error(), ERROR_SERVICE_REQUEST_TIMEOUT);
	CHECK(!ControlService(s, SERVICE_CONTROL_STOP, &st));
	CHECK_EQ(last_error(), ERROR_SERVICE_NOT_ACTIVE);
	CHECK(ChangeServiceConfigA(s, SERVICE_NO_CHANGE, SERVICE_DISABLED,
				   SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL,
				   NULL, NULL, "Renamed"));
	CHECK(!StartServiceA(s, 0, NULL));
	CHECK_EQ(last_error(), ERROR_SERVICE_DISABLED);
	n = sizeof(name);
	CHECK(GetServiceDisplayNameA(scm, "seancetest", name, &n));
	CHECK_STR(name, "Renamed");
	CHECK(!ChangeServiceConfigA(s, SERVICE_KERNEL_DRIVER, SERVICE_NO_CHANGE,
				    SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL,
				    NULL, NULL, NULL));

	/* Enumerated: all at once, or one at a time with resume. */
	BYTE buf[1024];
	DWORD count = 0, resume = 0;
	CHECK(EnumServicesStatusA(scm, SERVICE_WIN32, SERVICE_STATE_ALL,
				  (LPENUM_SERVICE_STATUSA)buf, sizeof(buf),
				  &need, &count, &resume));
	int found = 0;
	for (DWORD i = 0; i < count; i++) {
		ENUM_SERVICE_STATUSA *e = (ENUM_SERVICE_STATUSA *)buf + i;
		if (!strcmp(e->lpServiceName, "seancetest") &&
		    !strcmp(e->lpDisplayName, "Renamed") &&
		    e->ServiceStatus.dwCurrentState == SERVICE_STOPPED)
			found = 1;
	}
	CHECK(found);
	CHECK(EnumServicesStatusA(scm, SERVICE_WIN32, SERVICE_ACTIVE,
				  (LPENUM_SERVICE_STATUSA)buf, sizeof(buf),
				  &need, &count, NULL));
	CHECK_EQ(count, 0);
	CHECK(!EnumServicesStatusA(scm, SERVICE_WIN32, SERVICE_STATE_ALL,
				   (LPENUM_SERVICE_STATUSA)buf, 0, &need,
				   &count, NULL));
	CHECK(last_error() == ERROR_MORE_DATA && need > 0 && count == 0);
	static const WCHAR grp[] = { 'g', 'r', 'p', 0 };
	CHECK(EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
				    SERVICE_STATE_ALL, buf, sizeof(buf), &need,
				    &count, NULL, grp));
	CHECK_EQ(count, 1);

	/* The lock, once. */
	SC_LOCK lock = LockServiceDatabase(scm);
	CHECK(lock && !LockServiceDatabase(scm));
	CHECK(UnlockServiceDatabase(lock) && !UnlockServiceDatabase(lock));

	/* Deleted once the last handle goes. */
	s2 = OpenServiceA(scm, "seancetest", DELETE);
	CHECK(s2 && DeleteService(s2));
	CHECK(!DeleteService(s2));
	CHECK_EQ(last_error(), ERROR_SERVICE_MARKED_FOR_DELETE);
	CHECK(CloseServiceHandle(s2));
	CHECK(!CreateServiceA(scm, "seancetest", NULL, SERVICE_ALL_ACCESS,
			      SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
			      SERVICE_ERROR_NORMAL, "x.exe", NULL, NULL, NULL,
			      NULL, NULL));
	CHECK_EQ(last_error(), ERROR_SERVICE_MARKED_FOR_DELETE);
	CHECK(CloseServiceHandle(s));
	CHECK(!OpenServiceA(scm, "seancetest", SERVICE_QUERY_STATUS));
	CHECK_EQ(last_error(), ERROR_SERVICE_DOES_NOT_EXIST);
	CHECK(!CloseServiceHandle(s));

	CHECK(!StartServiceCtrlDispatcherA(NULL));
	CHECK_EQ(last_error(), ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
	CHECK(CloseServiceHandle(ro) && CloseServiceHandle(scm));
}

static void test_event_log(void)
{
	FN(HANDLE, RegisterEventSourceA, LPCSTR, LPCSTR);
	FN(HANDLE, RegisterEventSourceW, LPCWSTR, LPCWSTR);
	FN(BOOL, DeregisterEventSource, HANDLE);
	FN(BOOL, ReportEventA, HANDLE, WORD, WORD, DWORD, PSID, WORD, DWORD,
	   LPCSTR *, LPVOID);
	FN(BOOL, ReportEventW, HANDLE, WORD, WORD, DWORD, PSID, WORD, DWORD,
	   LPCWSTR *, LPVOID);
	FN(HANDLE, OpenEventLogA, LPCSTR, LPCSTR);
	FN(BOOL, CloseEventLog, HANDLE);
	FN(BOOL, ReadEventLogA, HANDLE, DWORD, DWORD, LPVOID, DWORD, DWORD *,
	   DWORD *);
	FN(BOOL, ReadEventLogW, HANDLE, DWORD, DWORD, LPVOID, DWORD, DWORD *,
	   DWORD *);
	FN(BOOL, GetNumberOfEventLogRecords, HANDLE, PDWORD);
	FN(BOOL, GetOldestEventLogRecord, HANDLE, PDWORD);
	FN(BOOL, ClearEventLogA, HANDLE, LPCSTR);
	FN(BOOL, ConvertStringSidToSidA, LPCSTR, PSID *);
	DWORD (WINAPI *last_error)(void) = get_from(k32, "GetLastError");

	HANDLE src = RegisterEventSourceA(NULL, "SeanceTest");
	CHECK(src != NULL);
	LPCSTR strs[] = { "first", "second" };
	BYTE data[3] = { 1, 2, 3 };
	PSID sid = NULL;
	CHECK(ConvertStringSidToSidA("S-1-5-32-544", &sid));
	CHECK(ReportEventA(src, EVENTLOG_WARNING_TYPE, 7, 0x1234, sid, 2,
			   sizeof(data), strs, data));
	static const WCHAR wsrc[] = { 'W', 's', 0 }, wtext[] = { 'w', 0 };
	HANDLE src2 = RegisterEventSourceW(NULL, wsrc);
	LPCWSTR ws[] = { wtext };
	CHECK(src2 && ReportEventW(src2, EVENTLOG_ERROR_TYPE, 0, 99, NULL, 1,
				   0, ws, NULL));
	CHECK(!ReportEventA(src, EVENTLOG_ERROR_TYPE, 0, 1, NULL, 1, 0, NULL,
			    NULL));

	HANDLE log = OpenEventLogA(NULL, "Application");
	CHECK(log != NULL);
	DWORD n = 0, oldest = 0;
	CHECK(GetNumberOfEventLogRecords(log, &n) && n == 2);
	CHECK(GetOldestEventLogRecord(log, &oldest) && oldest == 1);

	BYTE buf[1024];
	DWORD got = 0, need = 0;
	/* Too small: what the first needs. */
	CHECK(!ReadEventLogA(log, EVENTLOG_SEQUENTIAL_READ |
			     EVENTLOG_FORWARDS_READ, 0, buf, 8, &got, &need));
	CHECK(last_error() == ERROR_INSUFFICIENT_BUFFER && need > 8);
	CHECK(ReadEventLogA(log, EVENTLOG_SEQUENTIAL_READ |
			    EVENTLOG_FORWARDS_READ, 0, buf, sizeof(buf), &got,
			    &need));
	EVENTLOGRECORD *r = (EVENTLOGRECORD *)buf;
	CHECK(got > r->Length);	/* both records */
	CHECK(r->Reserved == 0x654c664c && r->RecordNumber == 1 &&
	      r->EventID == 0x1234 && r->EventType == EVENTLOG_WARNING_TYPE &&
	      r->EventCategory == 7 && r->NumStrings == 2 &&
	      r->DataLength == 3 && !(r->Length & 3));
	CHECK_STR((char *)(r + 1), "SeanceTest");
	CHECK_STR((char *)buf + r->StringOffset, "first");
	CHECK_STR((char *)buf + r->StringOffset + 6, "second");
	CHECK(!memcmp(buf + r->DataOffset, data, 3));
	CHECK_SID(buf + r->UserSidOffset, "S-1-5-32-544");
	/* The second record, if the first's length leaves room for it. */
	int fits = r->Length >= sizeof(*r) && r->Length < got &&
		   r->Length + sizeof(EVENTLOGRECORD) <= sizeof(buf);
	CHECK(fits);
	if (fits) {
		CHECK(!memcmp(buf + r->Length - 4, &r->Length, 4));
		EVENTLOGRECORD *r2 = (EVENTLOGRECORD *)(buf + r->Length);
		CHECK(r2->RecordNumber == 2 && r2->EventID == 99 &&
		      r2->UserSidLength == 0);
		if (r->Length + r2->StringOffset < sizeof(buf))
			CHECK_STR((char *)buf + r->Length + r2->StringOffset,
				  "w");
	}
	/* Then the end. */
	CHECK(!ReadEventLogA(log, EVENTLOG_SEQUENTIAL_READ |
			     EVENTLOG_FORWARDS_READ, 0, buf, sizeof(buf), &got,
			     &need));
	CHECK_EQ(last_error(), ERROR_HANDLE_EOF);
	/* W, seeking to record 2, then backwards. */
	CHECK(ReadEventLogW(log, EVENTLOG_SEEK_READ | EVENTLOG_BACKWARDS_READ,
			    2, buf, sizeof(buf), &got, &need));
	r = (EVENTLOGRECORD *)buf;
	CHECK(r->RecordNumber == 2 && got > r->Length);
	CHECK(((WCHAR *)(r + 1))[0] == 'W' && ((WCHAR *)(r + 1))[2] == 0);
	CHECK(((EVENTLOGRECORD *)(buf + r->Length))->RecordNumber == 1);
	CHECK(!ReadEventLogW(log, EVENTLOG_SEEK_READ | EVENTLOG_FORWARDS_READ,
			     9, buf, sizeof(buf), &got, &need));
	CHECK_EQ(last_error(), ERROR_INVALID_PARAMETER);

	/* Cleared, numbering goes on. */
	CHECK(ClearEventLogA(log, NULL));
	CHECK(GetNumberOfEventLogRecords(log, &n) && n == 0);
	CHECK(ReportEventA(src, EVENTLOG_INFORMATION_TYPE, 0, 5, NULL, 0, 0,
			   NULL, NULL));
	CHECK(GetOldestEventLogRecord(log, &oldest) && oldest == 3);
	/* Another log is separate. */
	HANDLE sys = OpenEventLogA(NULL, "System");
	CHECK(sys && GetNumberOfEventLogRecords(sys, &n) && n == 0);
	CHECK(CloseEventLog(sys) && CloseEventLog(log));
	CHECK(DeregisterEventSource(src) && DeregisterEventSource(src2));
	CHECK(!DeregisterEventSource(src));
	local_free(sid);
}

int main(void)
{
	const char *tmpdir = getenv("TMPDIR");
	snprintf(dir, sizeof(dir), "%s/vfw-sec-XXXXXX",
		 tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", dir, 1);
	setenv("VFW_REGISTRY", "", 1);

	CHECK_EQ(runtime_enter(), 0);
	adv = mod_load("advapi32.dll", NULL);
	k32 = mod_load("kernel32.dll", NULL);
	CHECK(adv && k32);
	if (!adv || !k32)
		return test_summary("test_security");

	test_tokens();
	test_sids();
	test_accounts();
	test_acls();
	test_entries();
	test_descriptors();
	test_objects();
	test_crypt();
	test_handles_full();
	test_sddl();
	test_access_check();
	test_services();
	test_event_log();

	mod_free(k32);
	mod_free(adv);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_security");
}
