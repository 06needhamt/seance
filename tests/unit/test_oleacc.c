/* SPDX-License-Identifier: GPL-2.0-only */
/* oleacc.dll's headless accessibility behavior. */
#include "test.h"

#include "runtime/builtin.h"

typedef HRESULT (WINAPI *create_fn)(HWND, LONG, const GUID *, void **);
typedef LRESULT (WINAPI *result_fn)(const GUID *, WPARAM, void *);

int main(void)
{
	const struct builtin_dll *dll = builtin_find("OLEACC.DLL");
	CHECK(dll != NULL);
	if (!dll)
		return test_summary("test_oleacc");

	create_fn create = builtin_export(dll, "CreateStdAccessibleObject");
	result_fn result = builtin_export(dll, "LresultFromObject");
	CHECK(create && result);
	if (create && result) {
		void *object = (void *)1;
		CHECK_EQ(create(NULL, 0, NULL, &object), E_NOTIMPL);
		CHECK(object == NULL);
		CHECK_EQ(result(NULL, 0, NULL), (LRESULT)E_NOTIMPL);
	}

	return test_summary("test_oleacc");
}
