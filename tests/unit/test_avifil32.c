/* SPDX-License-Identifier: GPL-2.0-only */
/* avifil32.dll's unsupported high-level AVI interfaces. */
#include "test.h"

#include "runtime/builtin.h"

typedef HRESULT (WINAPI *open_fn)(void **, const char *, UINT, const GUID *);
typedef HRESULT (WINAPI *stream_fn)(void *, void **, DWORD, LONG);
typedef HRESULT (WINAPI *format_fn)(void *, LONG, void *, LONG *);
typedef HRESULT (WINAPI *read_fn)(void *, LONG, LONG, void *, LONG, LONG *,
				  LONG *);
typedef LONG (WINAPI *long_fn)(void *);
typedef LONG (WINAPI *time_fn)(void *, LONG);
typedef ULONG (WINAPI *release_fn)(void *);

int main(void)
{
	const struct builtin_dll *dll = builtin_find("AVIFIL32.DLL");
	CHECK(dll != NULL);
	if (!dll)
		return test_summary("test_avifil32");

	open_fn open = builtin_export(dll, "AVIFileOpenA");
	stream_fn get_stream = builtin_export(dll, "AVIFileGetStream");
	format_fn format = builtin_export(dll, "AVIStreamReadFormat");
	read_fn read = builtin_export(dll, "AVIStreamRead");
	long_fn length = builtin_export(dll, "AVIStreamLength");
	time_fn time = builtin_export(dll, "AVIStreamSampleToTime");
	release_fn file_release = builtin_export(dll, "AVIFileRelease");
	release_fn stream_release = builtin_export(dll, "AVIStreamRelease");
	CHECK(open && get_stream && format && read && length && time &&
	      file_release && stream_release);
	if (open && get_stream && format && read && length && time &&
	    file_release && stream_release) {
		void *object = (void *)1;
		CHECK_EQ(open(&object, "clip.avi", 0, NULL), AVIERR_UNSUPPORTED);
		CHECK(object == NULL);
		object = (void *)1;
		CHECK_EQ(get_stream(NULL, &object, 0, 0), AVIERR_UNSUPPORTED);
		CHECK(object == NULL);
		LONG size = 42;
		CHECK_EQ(format(NULL, 0, NULL, &size), AVIERR_UNSUPPORTED);
		CHECK_EQ(size, 0);
		LONG bytes = 42, samples = 42;
		CHECK_EQ(read(NULL, 0, 1, NULL, 0, &bytes, &samples),
			 AVIERR_UNSUPPORTED);
		CHECK_EQ(bytes, 0);
		CHECK_EQ(samples, 0);
		CHECK_EQ(length(NULL), -1);
		CHECK_EQ(time(NULL, 0), -1);
		CHECK_EQ(file_release(NULL), 0u);
		CHECK_EQ(stream_release(NULL), 0u);
	}

	return test_summary("test_avifil32");
}
