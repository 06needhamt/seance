/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * crypt32.dll: certificate stores, all of them empty. FFmpeg's network
 * code opens the system stores for its TLS support; with nothing in them
 * it finds no certificate and carries on without one, which is right for
 * a harness that has no network.
 */
#include <stdlib.h>

#include "win32/internal.h"

#define CRYPT_E_NOT_FOUND ((DWORD)0x80092004)

/* A store handle: a tag, so closing something else is refused. */
#define STORE_MAGIC 0x45524f53u		/* "SORE" */

struct store {
	DWORD magic;
};

WINFN(HANDLE) crypt_CertOpenStore(LPCSTR provider, DWORD encoding,
				  ULONG_PTR prov, DWORD flags, const void *para)
{
	struct store *s = malloc(sizeof(*s));

	if (!s) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	s->magic = STORE_MAGIC;
	return s;
}

WINFN(BOOL) crypt_CertCloseStore(HANDLE h, DWORD flags)
{
	struct store *s = h;

	if (!s || s->magic != STORE_MAGIC) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	s->magic = 0;
	free(s);
	return TRUE;
}

/* Nothing is ever found, so no context is ever handed out. */

WINFN(const void *) crypt_CertEnumCertificatesInStore(HANDLE store,
						      const void *prev)
{
	win_set_error(CRYPT_E_NOT_FOUND);
	return NULL;
}

WINFN(const void *) crypt_CertEnumCRLsInStore(HANDLE store, const void *prev)
{
	win_set_error(CRYPT_E_NOT_FOUND);
	return NULL;
}

WINFN(const void *) crypt_CertFindCertificateInStore(HANDLE store,
						     DWORD encoding, DWORD flags,
						     DWORD type, const void *para,
						     const void *prev)
{
	win_set_error(CRYPT_E_NOT_FOUND);
	return NULL;
}

WINFN(BOOL) crypt_CertFreeCertificateContext(const void *ctx)
{
	return TRUE;
}

WINFN(BOOL) crypt_CertDeleteCertificateFromStore(const void *ctx)
{
	return TRUE;
}

WINFN(BOOL) crypt_CertGetCertificateContextProperty(const void *ctx, DWORD id,
						    void *data, DWORD *size)
{
	win_set_error(CRYPT_E_NOT_FOUND);
	return FALSE;
}

/* Importing a PKCS #12 file needs the cryptography that isn't here. */
WINFN(HANDLE) crypt_PFXImportCertStore(const void *pfx, LPCWSTR password,
				       DWORD flags)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return NULL;
}

#define E(name) { #name, 0, (void *)crypt_##name }

static const struct builtin_export exports[] = {
	E(CertCloseStore),
	E(CertDeleteCertificateFromStore),
	E(CertEnumCRLsInStore),
	E(CertEnumCertificatesInStore),
	E(CertFindCertificateInStore),
	E(CertFreeCertificateContext),
	E(CertGetCertificateContextProperty),
	E(CertOpenStore),
	E(PFXImportCertStore),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_crypt32 = { "crypt32.dll", NULL, tables, 0 };
