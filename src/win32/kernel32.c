/*
 * kernel32.dll: shared helpers, errors, handles and the export table list.
 * The rest lives in k32_*.c, one file per area.
 */
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "loader/modules.h"
#include "win32/handle.h"
#include "win32/k32.h"
#include "win32/internal.h"

void win_set_error(DWORD err)
{
	struct win_teb *teb = teb_current();
	if (teb)
		teb->LastErrorValue = err;
}

DWORD win_get_error(void)
{
	struct win_teb *teb = teb_current();
	return teb ? teb->LastErrorValue : 0;
}

DWORD win_error_from_errno(int err)
{
	switch (err) {
	case 0:            return ERROR_SUCCESS;
	case ENOENT:       return ERROR_FILE_NOT_FOUND;
	case ENOTDIR:      return ERROR_PATH_NOT_FOUND;
	case EACCES:
	case EPERM:
	case EISDIR:
	case EROFS:        return ERROR_ACCESS_DENIED;
	case EEXIST:       return ERROR_FILE_EXISTS;
	case ENOMEM:       return ERROR_NOT_ENOUGH_MEMORY;
	case ENOSPC:       return ERROR_DISK_FULL;
	case EBADF:        return ERROR_INVALID_HANDLE;
	case EINVAL:       return ERROR_INVALID_PARAMETER;
	case ENOTEMPTY:    return ERROR_DIR_NOT_EMPTY;
	case EMFILE:
	case ENFILE:       return ERROR_TOO_MANY_OPEN_FILES;
	case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
	case EPIPE:        return ERROR_BROKEN_PIPE;
	case ETIMEDOUT:    return ERROR_TIMEOUT;
	default:           return ERROR_INVALID_FUNCTION;
	}
}

size_t wstrlen(const WCHAR *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

struct module *win_caller_module(const void *return_address)
{
	return mod_from_address(return_address);
}

/* Errors. */

WINFN(DWORD) k32_GetLastError(void)
{
	return win_get_error();
}

WINFN(void) k32_SetLastError(DWORD err)
{
	win_set_error(err);
}

/* Handles. */

#define PSEUDO_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_THREAD  ((HANDLE)(LONG_PTR)-2)

WINFN(BOOL) k32_CloseHandle(HANDLE h)
{
	if (h == PSEUDO_PROCESS || h == PSEUDO_THREAD)
		return TRUE;
	return handle_close(h);
}

WINFN(BOOL) k32_DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process,
				 LPHANDLE dst, DWORD access, BOOL inherit,
				 DWORD options)
{
	if (src_process != PSEUDO_PROCESS || dst_process != PSEUDO_PROCESS) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	struct kobject *obj;
	if (src == PSEUDO_THREAD) {
		/* A real handle to the current thread. */
		HANDLE h = thread_handle_current();
		if (!h)
			return FALSE;
		if (dst)
			*dst = h;
		else
			handle_close(h);
		return TRUE;
	}
	if (!(obj = handle_ref(src, KOBJ_ANY)))
		return FALSE;
	HANDLE h = handle_new(obj);
	if (options & DUPLICATE_CLOSE_SOURCE)
		handle_close(src);
	if (!h)
		return FALSE;
	if (dst)
		*dst = h;
	else
		handle_close(h);
	return TRUE;
}

WINFN(BOOL) k32_GetHandleInformation(HANDLE h, LPDWORD flags)
{
	struct kobject *obj = handle_ref(h, KOBJ_ANY);

	if (!obj)
		return FALSE;
	kobj_unref(obj);
	if (flags)
		*flags = 0;
	return TRUE;
}

WINFN(BOOL) k32_SetHandleInformation(HANDLE h, DWORD mask, DWORD flags)
{
	struct kobject *obj = handle_ref(h, KOBJ_ANY);

	if (!obj)
		return FALSE;
	kobj_unref(obj);
	return TRUE;
}

#define E(name) { #name, 0, (void *)k32_##name }

static const struct builtin_export core_exports[] = {
	E(CloseHandle),
	E(DuplicateHandle),
	E(GetHandleInformation),
	E(GetLastError),
	E(SetHandleInformation),
	E(SetLastError),
	BUILTIN_END
};

extern const struct builtin_export k32_memory_exports[];
extern const struct builtin_export k32_sync_exports[];
extern const struct builtin_export k32_thread_exports[];
extern const struct builtin_export k32_time_exports[];
extern const struct builtin_export k32_system_exports[];
extern const struct builtin_export k32_string_exports[];
extern const struct builtin_export k32_module_exports[];
extern const struct builtin_export k32_file_exports[];
extern const struct builtin_export k32_profile_exports[];

static const struct builtin_export *const tables[] = {
	core_exports,
	k32_memory_exports,
	k32_sync_exports,
	k32_thread_exports,
	k32_time_exports,
	k32_system_exports,
	k32_string_exports,
	k32_module_exports,
	k32_file_exports,
	k32_profile_exports,
	NULL
};

const struct builtin_dll builtin_kernel32 = { "kernel32.dll", NULL, tables, 0 };
