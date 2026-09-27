/*
 * msvcrt: pe_crt.dll imports math and string functions, the way a codec
 * with a dynamic CRT (/MD) would; the harness's own builtin_msvcrt serves
 * them, forwarding to the host's libm/libc.
 */
#include <math.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"

int main(void)
{
	const char *dll = fixture("pe_crt.dll");

	CHECK_EQ(runtime_enter(), 0);
	struct module *m = mod_load(dll, NULL);
	CHECK(m != NULL);
	if (m) {
		double (*math_check)(void) = mod_get_proc(m, "crt_math_check");
		int (*string_check)(void) = mod_get_proc(m, "crt_string_check");
		CHECK(math_check != NULL);
		CHECK(string_check != NULL);
		if (math_check) {
			double want = sin(0.0) + cos(0.0) + sqrt(4.0) +
				      pow(2.0, 10.0) + fabs(-3.5) + floor(2.7) +
				      ceil(2.1) + atan2(1.0, 1.0) + log(1.0) +
				      exp(0.0) + fmod(10.0, 3.0);
			CHECK(math_check() == want);
		}
		if (string_check) {
			int r = string_check();
			if (r)
				fprintf(stderr, "crt_string_check failed at %d\n", r);
			CHECK_EQ(r, 0);
		}
		mod_free(m);
	}

	return test_summary("test_crt");
}
