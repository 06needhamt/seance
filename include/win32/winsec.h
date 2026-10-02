/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Security definitions for advapi32: SIDs, tokens, privileges, access
 * control lists and security descriptors (winnt.h, accctrl.h), and the
 * CryptoAPI provider handles CryptGenRandom needs (wincrypt.h).
 */
#ifndef VFW_WIN32_WINSEC_H
#define VFW_WIN32_WINSEC_H

#include "types.h"

typedef DWORD ACCESS_MASK, *PACCESS_MASK;
typedef PVOID PSID;
typedef PVOID PSECURITY_DESCRIPTOR;
typedef WORD SECURITY_DESCRIPTOR_CONTROL, *PSECURITY_DESCRIPTOR_CONTROL;
typedef DWORD SECURITY_INFORMATION, *PSECURITY_INFORMATION;

/* Generic and standard access rights. */
#define DELETE                   0x00010000
#define READ_CONTROL             0x00020000
#define WRITE_DAC                0x00040000
#define WRITE_OWNER              0x00080000
#define SYNCHRONIZE              0x00100000
#define STANDARD_RIGHTS_REQUIRED 0x000f0000
#define STANDARD_RIGHTS_ALL      0x001f0000
#define MAXIMUM_ALLOWED          0x02000000

/* SIDs. */
typedef struct _SID_IDENTIFIER_AUTHORITY {
	BYTE Value[6];
} SID_IDENTIFIER_AUTHORITY, *PSID_IDENTIFIER_AUTHORITY;

typedef struct _SID {
	BYTE Revision;
	BYTE SubAuthorityCount;
	SID_IDENTIFIER_AUTHORITY IdentifierAuthority;
	DWORD SubAuthority[1];
} SID, *PISID;

#define SID_REVISION            1
#define SID_MAX_SUB_AUTHORITIES 15
#define SECURITY_MAX_SID_SIZE   68

/* Relative identifiers. */
#define SECURITY_NULL_RID               0x00000000
#define SECURITY_WORLD_RID              0x00000000
#define SECURITY_LOCAL_RID              0x00000000
#define SECURITY_CREATOR_OWNER_RID      0x00000000
#define SECURITY_CREATOR_GROUP_RID      0x00000001
#define SECURITY_DIALUP_RID             0x00000001
#define SECURITY_NETWORK_RID            0x00000002
#define SECURITY_BATCH_RID              0x00000003
#define SECURITY_INTERACTIVE_RID        0x00000004
#define SECURITY_SERVICE_RID            0x00000006
#define SECURITY_ANONYMOUS_LOGON_RID    0x00000007
#define SECURITY_PRINCIPAL_SELF_RID     0x0000000a
#define SECURITY_AUTHENTICATED_USER_RID 0x0000000b
#define SECURITY_LOCAL_SYSTEM_RID       0x00000012
#define SECURITY_LOCAL_SERVICE_RID      0x00000013
#define SECURITY_NETWORK_SERVICE_RID    0x00000014
#define SECURITY_NT_NON_UNIQUE          0x00000015
#define SECURITY_BUILTIN_DOMAIN_RID     0x00000020
#define DOMAIN_USER_RID_ADMIN           0x000001f4
#define DOMAIN_USER_RID_GUEST           0x000001f5
#define DOMAIN_GROUP_RID_USERS          0x00000201
#define DOMAIN_ALIAS_RID_ADMINS         0x00000220
#define DOMAIN_ALIAS_RID_USERS          0x00000221
#define DOMAIN_ALIAS_RID_GUESTS         0x00000222
#define DOMAIN_ALIAS_RID_POWER_USERS    0x00000223
#define SECURITY_MANDATORY_LOW_RID      0x00001000
#define SECURITY_MANDATORY_MEDIUM_RID   0x00002000
#define SECURITY_MANDATORY_HIGH_RID     0x00003000
#define SECURITY_MANDATORY_SYSTEM_RID   0x00004000

typedef enum _SID_NAME_USE {
	SidTypeUser = 1,
	SidTypeGroup,
	SidTypeDomain,
	SidTypeAlias,
	SidTypeWellKnownGroup,
	SidTypeDeletedAccount,
	SidTypeInvalid,
	SidTypeUnknown,
	SidTypeComputer,
	SidTypeLabel
} SID_NAME_USE, *PSID_NAME_USE;

typedef enum {
	WinNullSid = 0,
	WinWorldSid = 1,
	WinLocalSid = 2,
	WinCreatorOwnerSid = 3,
	WinCreatorGroupSid = 4,
	WinCreatorOwnerServerSid = 5,
	WinCreatorGroupServerSid = 6,
	WinNtAuthoritySid = 7,
	WinDialupSid = 8,
	WinNetworkSid = 9,
	WinBatchSid = 10,
	WinInteractiveSid = 11,
	WinServiceSid = 12,
	WinAnonymousSid = 13,
	WinProxySid = 14,
	WinEnterpriseControllersSid = 15,
	WinSelfSid = 16,
	WinAuthenticatedUserSid = 17,
	WinRestrictedCodeSid = 18,
	WinTerminalServerSid = 19,
	WinRemoteLogonIdSid = 20,
	WinLogonIdsSid = 21,
	WinLocalSystemSid = 22,
	WinLocalServiceSid = 23,
	WinNetworkServiceSid = 24,
	WinBuiltinDomainSid = 25,
	WinBuiltinAdministratorsSid = 26,
	WinBuiltinUsersSid = 27,
	WinBuiltinGuestsSid = 28,
	WinBuiltinPowerUsersSid = 29,
	WinAccountAdministratorSid = 38,
	WinAccountGuestSid = 39,
	WinAccountDomainUsersSid = 42,
	WinMediumLabelSid = 67,
	WinHighLabelSid = 68
} WELL_KNOWN_SID_TYPE;

typedef struct _SID_AND_ATTRIBUTES {
	PSID Sid;
	DWORD Attributes;
} SID_AND_ATTRIBUTES, *PSID_AND_ATTRIBUTES;

/* SID_AND_ATTRIBUTES.Attributes of a group. */
#define SE_GROUP_MANDATORY          0x00000001
#define SE_GROUP_ENABLED_BY_DEFAULT 0x00000002
#define SE_GROUP_ENABLED            0x00000004
#define SE_GROUP_OWNER              0x00000008
#define SE_GROUP_USE_FOR_DENY_ONLY  0x00000010
#define SE_GROUP_INTEGRITY          0x00000020
#define SE_GROUP_INTEGRITY_ENABLED  0x00000040
#define SE_GROUP_LOGON_ID           0xc0000000

/* Privileges. */
typedef struct _LUID {
	DWORD LowPart;
	LONG HighPart;
} LUID, *PLUID;

typedef struct _LUID_AND_ATTRIBUTES {
	LUID Luid;
	DWORD Attributes;
} LUID_AND_ATTRIBUTES, *PLUID_AND_ATTRIBUTES;

#define SE_PRIVILEGE_ENABLED_BY_DEFAULT 0x00000001
#define SE_PRIVILEGE_ENABLED            0x00000002
#define SE_PRIVILEGE_REMOVED            0x00000004
#define SE_PRIVILEGE_USED_FOR_ACCESS    0x80000000
#define PRIVILEGE_SET_ALL_NECESSARY     1

typedef struct _PRIVILEGE_SET {
	DWORD PrivilegeCount;
	DWORD Control;
	LUID_AND_ATTRIBUTES Privilege[1];
} PRIVILEGE_SET, *PPRIVILEGE_SET;

/* Tokens. */
#define TOKEN_ASSIGN_PRIMARY    0x0001
#define TOKEN_DUPLICATE         0x0002
#define TOKEN_IMPERSONATE       0x0004
#define TOKEN_QUERY             0x0008
#define TOKEN_QUERY_SOURCE      0x0010
#define TOKEN_ADJUST_PRIVILEGES 0x0020
#define TOKEN_ADJUST_GROUPS     0x0040
#define TOKEN_ADJUST_DEFAULT    0x0080
#define TOKEN_ADJUST_SESSIONID  0x0100

typedef enum _TOKEN_TYPE {
	TokenPrimary = 1,
	TokenImpersonation
} TOKEN_TYPE, *PTOKEN_TYPE;

typedef enum _SECURITY_IMPERSONATION_LEVEL {
	SecurityAnonymous,
	SecurityIdentification,
	SecurityImpersonation,
	SecurityDelegation
} SECURITY_IMPERSONATION_LEVEL, *PSECURITY_IMPERSONATION_LEVEL;

typedef enum _TOKEN_ELEVATION_TYPE {
	TokenElevationTypeDefault = 1,
	TokenElevationTypeFull,
	TokenElevationTypeLimited
} TOKEN_ELEVATION_TYPE, *PTOKEN_ELEVATION_TYPE;

typedef enum _TOKEN_INFORMATION_CLASS {
	TokenUser = 1,
	TokenGroups,
	TokenPrivileges,
	TokenOwner,
	TokenPrimaryGroup,
	TokenDefaultDacl,
	TokenSource,
	TokenType,
	TokenImpersonationLevel,
	TokenStatistics,
	TokenRestrictedSids,
	TokenSessionId,
	TokenGroupsAndPrivileges,
	TokenSessionReference,
	TokenSandBoxInert,
	TokenAuditPolicy,
	TokenOrigin,
	TokenElevationType,
	TokenLinkedToken,
	TokenElevation,
	TokenHasRestrictions,
	TokenAccessInformation,
	TokenVirtualizationAllowed,
	TokenVirtualizationEnabled,
	TokenIntegrityLevel
} TOKEN_INFORMATION_CLASS, *PTOKEN_INFORMATION_CLASS;

typedef struct _TOKEN_USER {
	SID_AND_ATTRIBUTES User;
} TOKEN_USER, *PTOKEN_USER;

typedef struct _TOKEN_GROUPS {
	DWORD GroupCount;
	SID_AND_ATTRIBUTES Groups[1];
} TOKEN_GROUPS, *PTOKEN_GROUPS;

typedef struct _TOKEN_PRIVILEGES {
	DWORD PrivilegeCount;
	LUID_AND_ATTRIBUTES Privileges[1];
} TOKEN_PRIVILEGES, *PTOKEN_PRIVILEGES;

typedef struct _TOKEN_OWNER {
	PSID Owner;
} TOKEN_OWNER, *PTOKEN_OWNER;

typedef struct _TOKEN_PRIMARY_GROUP {
	PSID PrimaryGroup;
} TOKEN_PRIMARY_GROUP, *PTOKEN_PRIMARY_GROUP;

typedef struct _TOKEN_ELEVATION {
	DWORD TokenIsElevated;
} TOKEN_ELEVATION, *PTOKEN_ELEVATION;

typedef struct _TOKEN_MANDATORY_LABEL {
	SID_AND_ATTRIBUTES Label;
} TOKEN_MANDATORY_LABEL, *PTOKEN_MANDATORY_LABEL;

typedef struct _TOKEN_STATISTICS {
	LUID TokenId;
	LUID AuthenticationId;
	LARGE_INTEGER ExpirationTime;
	TOKEN_TYPE TokenType;
	SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;
	DWORD DynamicCharged;
	DWORD DynamicAvailable;
	DWORD GroupCount;
	DWORD PrivilegeCount;
	LUID ModifiedId;
} TOKEN_STATISTICS, *PTOKEN_STATISTICS;

/* Access control lists. */
typedef struct _ACL {
	BYTE AclRevision;
	BYTE Sbz1;
	WORD AclSize;
	WORD AceCount;
	WORD Sbz2;
} ACL, *PACL;

#define ACL_REVISION    2
#define ACL_REVISION_DS 4

typedef struct _ACE_HEADER {
	BYTE AceType;
	BYTE AceFlags;
	WORD AceSize;
} ACE_HEADER, *PACE_HEADER;

#define ACCESS_ALLOWED_ACE_TYPE 0x0
#define ACCESS_DENIED_ACE_TYPE  0x1
#define SYSTEM_AUDIT_ACE_TYPE   0x2

#define OBJECT_INHERIT_ACE       0x01
#define CONTAINER_INHERIT_ACE    0x02
#define NO_PROPAGATE_INHERIT_ACE 0x04
#define INHERIT_ONLY_ACE         0x08
#define INHERITED_ACE            0x10
#define VALID_INHERIT_FLAGS      0x1f

typedef struct _ACCESS_ALLOWED_ACE {
	ACE_HEADER Header;
	ACCESS_MASK Mask;
	DWORD SidStart;
} ACCESS_ALLOWED_ACE, *PACCESS_ALLOWED_ACE;

typedef struct _ACCESS_DENIED_ACE {
	ACE_HEADER Header;
	ACCESS_MASK Mask;
	DWORD SidStart;
} ACCESS_DENIED_ACE, *PACCESS_DENIED_ACE;

typedef enum _ACL_INFORMATION_CLASS {
	AclRevisionInformation = 1,
	AclSizeInformation
} ACL_INFORMATION_CLASS;

typedef struct _ACL_REVISION_INFORMATION {
	DWORD AclRevision;
} ACL_REVISION_INFORMATION, *PACL_REVISION_INFORMATION;

typedef struct _ACL_SIZE_INFORMATION {
	DWORD AceCount;
	DWORD AclBytesInUse;
	DWORD AclBytesFree;
} ACL_SIZE_INFORMATION, *PACL_SIZE_INFORMATION;

/* Security descriptors. */
typedef struct _SECURITY_DESCRIPTOR {
	BYTE Revision;
	BYTE Sbz1;
	SECURITY_DESCRIPTOR_CONTROL Control;
	PSID Owner;
	PSID Group;
	PACL Sacl;
	PACL Dacl;
} SECURITY_DESCRIPTOR, *PISECURITY_DESCRIPTOR;

/* The self-relative form: offsets from its start instead of pointers. */
typedef struct _SECURITY_DESCRIPTOR_RELATIVE {
	BYTE Revision;
	BYTE Sbz1;
	SECURITY_DESCRIPTOR_CONTROL Control;
	DWORD Owner;
	DWORD Group;
	DWORD Sacl;
	DWORD Dacl;
} SECURITY_DESCRIPTOR_RELATIVE, *PISECURITY_DESCRIPTOR_RELATIVE;

#define SECURITY_DESCRIPTOR_REVISION   1
#define SECURITY_DESCRIPTOR_MIN_LENGTH 20

#define SE_OWNER_DEFAULTED       0x0001
#define SE_GROUP_DEFAULTED       0x0002
#define SE_DACL_PRESENT          0x0004
#define SE_DACL_DEFAULTED        0x0008
#define SE_SACL_PRESENT          0x0010
#define SE_SACL_DEFAULTED        0x0020
#define SE_DACL_AUTO_INHERIT_REQ 0x0100
#define SE_SACL_AUTO_INHERIT_REQ 0x0200
#define SE_DACL_AUTO_INHERITED   0x0400
#define SE_SACL_AUTO_INHERITED   0x0800
#define SE_DACL_PROTECTED        0x1000
#define SE_SACL_PROTECTED        0x2000
#define SE_SELF_RELATIVE         0x8000

#define OWNER_SECURITY_INFORMATION 0x00000001
#define GROUP_SECURITY_INFORMATION 0x00000002
#define DACL_SECURITY_INFORMATION  0x00000004
#define SACL_SECURITY_INFORMATION  0x00000008
#define LABEL_SECURITY_INFORMATION 0x00000010

/* GetNamedSecurityInfo's object types (accctrl.h). */
typedef enum _SE_OBJECT_TYPE {
	SE_UNKNOWN_OBJECT_TYPE = 0,
	SE_FILE_OBJECT,
	SE_SERVICE,
	SE_PRINTER,
	SE_REGISTRY_KEY,
	SE_LMSHARE,
	SE_KERNEL_OBJECT,
	SE_WINDOW_OBJECT,
	SE_DS_OBJECT,
	SE_DS_OBJECT_ALL,
	SE_PROVIDER_DEFINED_OBJECT,
	SE_WMIGUID_OBJECT,
	SE_REGISTRY_WOW64_32KEY
} SE_OBJECT_TYPE;

/* CryptoAPI providers (wincrypt.h), for CryptGenRandom. */
typedef ULONG_PTR HCRYPTPROV;

#define PROV_RSA_FULL        1
#define PROV_RSA_SIG         2
#define PROV_DSS             3
#define PROV_RSA_SCHANNEL    12
#define PROV_DSS_DH          13
#define PROV_DH_SCHANNEL     18
#define PROV_RSA_AES         24
#define CRYPT_VERIFYCONTEXT  0xf0000000
#define CRYPT_NEWKEYSET      0x00000008
#define CRYPT_DELETEKEYSET   0x00000010
#define CRYPT_MACHINE_KEYSET 0x00000020
#define CRYPT_SILENT         0x00000040

#define NTE_BAD_FLAGS     ((HRESULT)0x80090009)
#define NTE_BAD_KEYSET    ((HRESULT)0x80090016)
#define NTE_BAD_PROV_TYPE ((HRESULT)0x80090014)
#define NTE_EXISTS        ((HRESULT)0x8009000f)

#endif /* VFW_WIN32_WINSEC_H */
