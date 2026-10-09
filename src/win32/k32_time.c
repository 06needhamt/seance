/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: clocks, FILETIME/SYSTEMTIME conversion, time zones.
 *
 * The harness runs in UTC: local time is system time, and the time zone
 * has no bias. That keeps codec behaviour independent of the host's zone.
 *
 * VFW_DATE sets the date (and time) the clock reads at startup, for DLLs
 * whose behaviour depends on it; it runs on from there. Tick counts and
 * the performance counter are unaffected, as are file times.
 */
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

#include "win32/internal.h"

#define EPOCH_DIFF   11644473600ull	/* seconds from 1601 to 1970 */
#define TICKS_PER_S  10000000ull	/* FILETIME units: 100 ns */
#define QPC_FREQ     10000000ull

/* How far the clock is set from the host's (VFW_DATE), in FILETIME units. */
static LONGLONG clock_shift;

static ULONGLONG host_filetime_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return ((ULONGLONG)ts.tv_sec + EPOCH_DIFF) * TICKS_PER_S +
	       ts.tv_nsec / 100;
}

static ULONGLONG filetime_now(void)
{
	return host_filetime_now() + (ULONGLONG)clock_shift;
}

ULONGLONG win_filetime_now(void)
{
	return filetime_now();
}

long long win_unix_now(void)
{
	return (long long)(filetime_now() / TICKS_PER_S) - (long long)EPOCH_DIFF;
}

static void to_filetime(ULONGLONG v, LPFILETIME ft)
{
	ft->dwLowDateTime = (DWORD)v;
	ft->dwHighDateTime = (DWORD)(v >> 32);
}

static ULONGLONG from_filetime(const FILETIME *ft)
{
	return ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant). */
static LONGLONG days_from_civil(int y, unsigned m, unsigned d)
{
	y -= m <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (LONGLONG)era * 146097 + doe - 719468;
}

static void civil_from_days(LONGLONG z, int *y, unsigned *m, unsigned *d)
{
	z += 719468;
	LONGLONG era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = (int)(yoe + era * 400) + (*m <= 2);
}

int win_clock_set(const char *date)
{
	int y, n = 0;
	unsigned mo, d, h = 0, mi = 0, sec = 0;

	if (!date || !*date) {
		clock_shift = 0;
		return 0;
	}
	if (sscanf(date, "%4d-%2u-%2u%n", &y, &mo, &d, &n) != 3)
		return -1;
	date += n;
	if (*date == 'T' || *date == ' ') {
		n = 0;
		if (sscanf(date + 1, "%2u:%2u%n", &h, &mi, &n) != 2)
			return -1;
		date += 1 + n;
		if (*date == ':') {
			n = 0;
			if (sscanf(date + 1, "%2u%n", &sec, &n) != 1)
				return -1;
			date += 1 + n;
		}
	}
	if (*date || y < 1601 || mo < 1 || mo > 12 || d < 1 || h > 23 ||
	    mi > 59 || sec > 59)
		return -1;
	/* The day must exist in that month. */
	int ny;
	unsigned nm, nd;
	civil_from_days(days_from_civil(y, mo, d), &ny, &nm, &nd);
	if (nm != mo || nd != d)
		return -1;
	LONGLONG secs = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 +
			sec + (LONGLONG)EPOCH_DIFF;
	clock_shift = secs * (LONGLONG)TICKS_PER_S -
		      (LONGLONG)host_filetime_now();
	return 0;
}

static void filetime_to_systemtime(ULONGLONG v, LPSYSTEMTIME st)
{
	ULONGLONG secs = v / TICKS_PER_S;
	LONGLONG days = (LONGLONG)(secs / 86400) - (LONGLONG)(EPOCH_DIFF / 86400);
	DWORD rem = secs % 86400;
	int y;
	unsigned m, d;

	civil_from_days(days, &y, &m, &d);
	st->wYear = y;
	st->wMonth = m;
	st->wDay = d;
	/* 1970-01-01 was a Thursday (4). */
	st->wDayOfWeek = (WORD)(((days % 7) + 11) % 7);
	st->wHour = rem / 3600;
	st->wMinute = rem / 60 % 60;
	st->wSecond = rem % 60;
	st->wMilliseconds = (v % TICKS_PER_S) / 10000;
}

WINFN(DWORD) k32_GetTickCount(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (DWORD)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}

WINFN(ULONGLONG) k32_GetTickCount64(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

WINFN(BOOL) k32_QueryPerformanceCounter(LARGE_INTEGER *out)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	out->QuadPart = ts.tv_sec * QPC_FREQ + ts.tv_nsec / (1000000000ull / QPC_FREQ);
	return TRUE;
}

WINFN(BOOL) k32_QueryPerformanceFrequency(LARGE_INTEGER *out)
{
	out->QuadPart = QPC_FREQ;
	return TRUE;
}

WINFN(void) k32_GetSystemTimeAsFileTime(LPFILETIME ft)
{
	to_filetime(filetime_now(), ft);
}

WINFN(void) k32_GetSystemTime(LPSYSTEMTIME st)
{
	filetime_to_systemtime(filetime_now(), st);
}

WINFN(void) k32_GetLocalTime(LPSYSTEMTIME st)
{
	filetime_to_systemtime(filetime_now(), st);
}

WINFN(BOOL) k32_SetSystemTime(const SYSTEMTIME *st)
{
	win_set_error(ERROR_ACCESS_DENIED);
	return FALSE;
}

WINFN(BOOL) k32_SetLocalTime(const SYSTEMTIME *st)
{
	win_set_error(ERROR_ACCESS_DENIED);
	return FALSE;
}

WINFN(BOOL) k32_FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
	ULONGLONG v = from_filetime(ft);

	if (v >> 63) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	filetime_to_systemtime(v, st);
	return TRUE;
}

WINFN(BOOL) k32_SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
	static const BYTE mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if (st->wYear < 1601 || st->wYear > 30827 || st->wMonth < 1 ||
	    st->wMonth > 12 || st->wDay < 1 || st->wDay > mdays[st->wMonth - 1] ||
	    st->wHour > 23 || st->wMinute > 59 || st->wSecond > 59 ||
	    st->wMilliseconds > 999) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	int leap = (st->wYear % 4 == 0 && st->wYear % 100) || st->wYear % 400 == 0;
	if (st->wMonth == 2 && st->wDay == 29 && !leap) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	LONGLONG days = days_from_civil(st->wYear, st->wMonth, st->wDay) +
			(LONGLONG)(EPOCH_DIFF / 86400);
	ULONGLONG secs = (ULONGLONG)days * 86400 + st->wHour * 3600u +
			 st->wMinute * 60u + st->wSecond;
	to_filetime(secs * TICKS_PER_S + st->wMilliseconds * 10000ull, ft);
	return TRUE;
}

WINFN(BOOL) k32_FileTimeToLocalFileTime(const FILETIME *in, LPFILETIME out)
{
	*out = *in;
	return TRUE;
}

WINFN(BOOL) k32_LocalFileTimeToFileTime(const FILETIME *in, LPFILETIME out)
{
	*out = *in;
	return TRUE;
}

WINFN(LONG) k32_CompareFileTime(const FILETIME *a, const FILETIME *b)
{
	ULONGLONG x = from_filetime(a), y = from_filetime(b);
	return x < y ? -1 : x > y;
}

WINFN(BOOL) k32_FileTimeToDosDateTime(const FILETIME *ft, LPWORD date,
				      LPWORD time)
{
	SYSTEMTIME st;

	filetime_to_systemtime(from_filetime(ft), &st);
	if (st.wYear < 1980 || st.wYear > 2107) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*date = ((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay;
	*time = (st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2);
	return TRUE;
}

WINFN(BOOL) k32_DosDateTimeToFileTime(WORD date, WORD time, LPFILETIME ft)
{
	SYSTEMTIME st = {
		.wYear = 1980 + (date >> 9), .wMonth = (date >> 5) & 15,
		.wDay = date & 31, .wHour = time >> 11,
		.wMinute = (time >> 5) & 63, .wSecond = (time & 31) * 2,
	};
	return k32_SystemTimeToFileTime(&st, ft);
}

WINFN(DWORD) k32_GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz)
{
	static const char name[] = "Coordinated Universal Time";

	memset(tz, 0, sizeof(*tz));
	for (size_t i = 0; i < sizeof(name) - 1; i++)
		tz->StandardName[i] = tz->DaylightName[i] = name[i];
	return TIME_ZONE_ID_UNKNOWN;
}

WINFN(BOOL) k32_GetSystemTimeAdjustment(PDWORD adjustment, PDWORD increment,
					PBOOL disabled)
{
	/* The clock is never slewed: 15.625 ms ticks, adjustment disabled. */
	*adjustment = *increment = 156250;
	*disabled = TRUE;
	return TRUE;
}

static void duration_filetime(const struct timespec *ts, LPFILETIME ft)
{
	to_filetime((ULONGLONG)ts->tv_sec * TICKS_PER_S + ts->tv_nsec / 100, ft);
}

/* The creation time is when /proc/self appeared; the exit time is unset. */
static void start_time(LPFILETIME ft)
{
	struct stat st;

	if (stat("/proc/self", &st)) {
		to_filetime(filetime_now(), ft);
		return;
	}
	to_filetime(((ULONGLONG)st.st_ctim.tv_sec + EPOCH_DIFF) * TICKS_PER_S +
		    st.st_ctim.tv_nsec / 100 + (ULONGLONG)clock_shift, ft);
}

WINFN(BOOL) k32_GetProcessTimes(HANDLE process, LPFILETIME creation,
				LPFILETIME exit, LPFILETIME kernel,
				LPFILETIME user)
{
	struct rusage ru;

	if (process != (HANDLE)(LONG_PTR)-1 || getrusage(RUSAGE_SELF, &ru)) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	start_time(creation);
	to_filetime(0, exit);
	to_filetime(((ULONGLONG)ru.ru_stime.tv_sec * 1000000 +
		     ru.ru_stime.tv_usec) * 10, kernel);
	to_filetime(((ULONGLONG)ru.ru_utime.tv_sec * 1000000 +
		     ru.ru_utime.tv_usec) * 10, user);
	return TRUE;
}

/*
 * The running thread's times are real (all of its CPU time is counted as user
 * time); any other thread's are not tracked, so read as zero.
 */
WINFN(BOOL) k32_GetThreadTimes(HANDLE thread, LPFILETIME creation,
			       LPFILETIME exit, LPFILETIME kernel,
			       LPFILETIME user)
{
	struct timespec ts = { 0, 0 };

	if (thread == (HANDLE)(LONG_PTR)-2)
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	start_time(creation);
	to_filetime(0, exit);
	to_filetime(0, kernel);
	duration_filetime(&ts, user);
	return TRUE;
}

WINFN(BOOL) k32_SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *tz,
						const SYSTEMTIME *utc,
						LPSYSTEMTIME local)
{
	FILETIME ft;
	LONG bias = tz ? tz->Bias + tz->StandardBias : 0;

	if (!k32_SystemTimeToFileTime(utc, &ft))
		return FALSE;
	to_filetime(from_filetime(&ft) - (LONGLONG)bias * 60 * TICKS_PER_S, &ft);
	return k32_FileTimeToSystemTime(&ft, local);
}

WINFN(BOOL) k32_TzSpecificLocalTimeToSystemTime(const TIME_ZONE_INFORMATION *tz,
						const SYSTEMTIME *local,
						LPSYSTEMTIME utc)
{
	FILETIME ft;
	LONG bias = tz ? tz->Bias + tz->StandardBias : 0;

	if (!k32_SystemTimeToFileTime(local, &ft))
		return FALSE;
	to_filetime(from_filetime(&ft) + (LONGLONG)bias * 60 * TICKS_PER_S, &ft);
	return k32_FileTimeToSystemTime(&ft, utc);
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_time_exports[] = {
	E(CompareFileTime),
	E(DosDateTimeToFileTime),
	E(FileTimeToDosDateTime),
	E(FileTimeToLocalFileTime),
	E(FileTimeToSystemTime),
	E(GetLocalTime),
	E(GetProcessTimes),
	E(GetSystemTime),
	E(GetSystemTimeAdjustment),
	E(GetSystemTimeAsFileTime),
	E(GetThreadTimes),
	E(GetTickCount),
	E(GetTickCount64),
	E(GetTimeZoneInformation),
	E(LocalFileTimeToFileTime),
	E(QueryPerformanceCounter),
	E(QueryPerformanceFrequency),
	E(SetLocalTime),
	E(SetSystemTime),
	E(SystemTimeToFileTime),
	E(SystemTimeToTzSpecificLocalTime),
	E(TzSpecificLocalTimeToSystemTime),
	BUILTIN_END
};
