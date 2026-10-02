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
	test_descriptors();
	test_objects();
	test_crypt();
	test_handles_full();

	mod_free(k32);
	mod_free(adv);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_security");
}
