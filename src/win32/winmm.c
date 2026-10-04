/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * winmm.dll: the installable-driver functions codecs use (DefDriverProc
 * above all), on top of src/vfw/driver.c, and the multimedia timer.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "common/unicode.h"
#include "loader/modules.h"
#include "runtime/crash.h"
#include "vfw/driver.h"
#include "vfw/registry.h"
#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

/* The longest timer delay and period, as timeGetDevCaps reports. */
#define TIMER_MAX 1000000
#define WAVE_MAGIC 0x57415645u

struct wave_out { DWORD magic; int paused; };

static struct wave_out *wave(HWAVEOUT h)
{
	struct wave_out *w = (struct wave_out *)h;
	return w && w->magic == WAVE_MAGIC ? w : NULL;
}

WINFN(UINT) mm_waveOutOpen(HWAVEOUT *out, UINT device, const WAVEFORMATEX *fmt,
	DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
	(void)callback; (void)instance; (void)flags;
	if (!out || !fmt || (device != WAVE_MAPPER && device != 0) ||
	    fmt->wFormatTag != WAVE_FORMAT_PCM) return MMSYSERR_INVALPARAM;
	struct wave_out *w = calloc(1, sizeof(*w));
	if (!w) return MMSYSERR_ALLOCATED;
	w->magic = WAVE_MAGIC; *out = (HWAVEOUT)w; return MMSYSERR_NOERROR;
}
WINFN(UINT) mm_waveOutClose(HWAVEOUT h)
{
	struct wave_out *w = wave(h); if (!w) return MMSYSERR_INVALHANDLE;
	w->magic = 0; free(w); return MMSYSERR_NOERROR;
}
WINFN(UINT) mm_waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT size)
{
	if (!wave(h)) return MMSYSERR_INVALHANDLE;
	if (!hdr || size < sizeof(*hdr)) return MMSYSERR_INVALPARAM;
	hdr->dwFlags |= WHDR_PREPARED; return MMSYSERR_NOERROR;
}
WINFN(UINT) mm_waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT size)
{
	if (!wave(h)) return MMSYSERR_INVALHANDLE;
	if (!hdr || size < sizeof(*hdr) || (hdr->dwFlags & WHDR_INQUEUE)) return MMSYSERR_INVALPARAM;
	hdr->dwFlags &= ~WHDR_PREPARED; return MMSYSERR_NOERROR;
}
WINFN(UINT) mm_waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT size)
{
	if (!wave(h)) return MMSYSERR_INVALHANDLE;
	if (!hdr || size < sizeof(*hdr) || !(hdr->dwFlags & WHDR_PREPARED)) return MMSYSERR_INVALPARAM;
	/* Audio is deliberately discarded, but completion is synchronous and
	 * observable exactly like an already played buffer. */
	hdr->dwFlags = (hdr->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
	return MMSYSERR_NOERROR;
}
WINFN(UINT) mm_waveOutPause(HWAVEOUT h) { struct wave_out *w=wave(h); if(!w)return MMSYSERR_INVALHANDLE; w->paused=1; return 0; }
WINFN(UINT) mm_waveOutRestart(HWAVEOUT h) { struct wave_out *w=wave(h); if(!w)return MMSYSERR_INVALHANDLE; w->paused=0; return 0; }
WINFN(UINT) mm_waveOutReset(HWAVEOUT h) { return wave(h) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE; }

/* Wine's DefDriverProc: what a driver gets for messages it doesn't handle. */
WINFN(LRESULT) mm_DefDriverProc(DWORD_PTR id, HDRVR h, UINT msg, LPARAM l1,
				LPARAM l2)
{
	switch (msg) {
	case DRV_LOAD:
	case DRV_FREE:
	case DRV_ENABLE:
	case DRV_DISABLE:
	case DRV_OPEN:
	case DRV_CLOSE:
		return 1;
	case DRV_QUERYCONFIGURE:
		return 0;
	case DRV_CONFIGURE:
		TRACE("DefDriverProc: the driver isn't configurable");
		return DRVCNF_OK;
	case DRV_INSTALL:
	case DRV_REMOVE:
		return DRVCNF_RESTART;
	default:
		return 0;
	}
}

/*
 * The DLL for a driver name: an alias such as "vidc.TST0" registered in
 * [drivers32] (the only section there is), or a DLL name or path.
 */
static HDRVR open_named(const char *name, const WCHAR *alias, LPARAM lp2)
{
	FOURCC type, handler;
	char *unix_path = NULL;
	HDRVR h;

	if (strchr(name, '.') && !vfw_parse_fourcc(name, &type, &handler)) {
		const struct vfw_codec *c = vfw_find(type, handler);
		if (c && c->proc)
			return drv_open_proc(c->proc, alias, lp2);
		if (c)
			return drv_open(c->path, alias, lp2);
	}
	if (strchr(name, ':') || strchr(name, '\\'))
		unix_path = path_to_unix(name);
	h = drv_open(unix_path ? unix_path : name, alias, lp2);
	free(unix_path);
	return h;
}

WINFN(HDRVR) mm_OpenDriver(LPCWSTR name, LPCWSTR section, LPARAM lp2)
{
	char utf8[1024];

	if (!name)
		return NULL;
	utf16_to_utf8(name, (size_t)-1, utf8, sizeof(utf8));
	HDRVR h = open_named(utf8, name, lp2);
	TRACE("OpenDriver(%s) -> %p", utf8, (void *)h);
	return h;
}

WINFN(HDRVR) mm_OpenDriverA(LPCSTR name, LPCSTR section, LPARAM lp2)
{
	WCHAR alias[260];
	size_t i = 0;

	if (!name)
		return NULL;
	for (; name[i] && i < 259; i++)
		alias[i] = (BYTE)name[i];
	alias[i] = 0;
	return open_named(name, alias, lp2);
}

WINFN(LRESULT) mm_CloseDriver(HDRVR h, LPARAM l1, LPARAM l2)
{
	return drv_close(h, l1, l2);
}

WINFN(LRESULT) mm_SendDriverMessage(HDRVR h, UINT msg, LPARAM l1, LPARAM l2)
{
	return drv_send(h, msg, l1, l2);
}

WINFN(HMODULE) mm_GetDriverModuleHandle(HDRVR h)
{
	struct module *m = drv_module(h);
	return m ? (HMODULE)m->base : NULL;
}

WINFN(DWORD) mm_timeGetTime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (DWORD)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * timeSetEvent's timers, after Wine's: one thread, started with the first
 * timer, runs every callback (on Windows too it is one thread of winmm's),
 * in the order they fall due. Callbacks run unlocked, so they may set and
 * kill timers, their own included.
 */
struct mm_timer {
	struct mm_timer *next;
	UINT id;
	UINT delay;
	UINT flags;
	LPTIMECALLBACK callback;	/* or the event, for TIME_CALLBACK_EVENT_* */
	DWORD_PTR user;
	uint64_t due;		/* ms, on CLOCK_MONOTONIC */
};

static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cond;	/* the list changed */
static pthread_cond_t timer_done;	/* a callback returned */
static struct mm_timer *timers;
static UINT timer_next_id = 1;
static UINT timer_running;		/* the id whose callback runs, or 0 */
static UINT timer_running_flags;
static DWORD timer_thread_id;
static int timer_started;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fire(const struct mm_timer *t)
{
	switch (t->flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE)) {
	case TIME_CALLBACK_EVENT_SET:
		event_signal((HANDLE)t->callback, 0);
		break;
	case TIME_CALLBACK_EVENT_PULSE:
		event_signal((HANDLE)t->callback, 1);
		break;
	default: {
		struct crash_frame saved;
		crash_enter("timeSetEvent callback", (const void *)t->callback,
			    0, NULL, &saved);
		t->callback(t->id, 0, t->user, 0, 0);
		crash_leave(&saved);
	}
	}
}

WINFN(DWORD) timer_main(LPVOID unused)
{
	pthread_mutex_lock(&timer_lock);
	timer_thread_id = thread_current_id();
	for (;;) {
		struct mm_timer **next = NULL;
		for (struct mm_timer **p = &timers; *p; p = &(*p)->next)
			if (!next || (*p)->due < (*next)->due)
				next = p;
		if (!next) {
			pthread_cond_wait(&timer_cond, &timer_lock);
			continue;
		}
		uint64_t now = now_ms();
		if ((*next)->due > now) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			uint64_t ns = (uint64_t)ts.tv_nsec +
				      ((*next)->due - now) * 1000000;
			ts.tv_sec += (time_t)(ns / 1000000000);
			ts.tv_nsec = (long)(ns % 1000000000);
			pthread_cond_timedwait(&timer_cond, &timer_lock, &ts);
			continue;
		}

		struct mm_timer t = **next;
		if (t.flags & TIME_PERIODIC) {
			/* A periodic timer that fell behind skips ahead. */
			(*next)->due += t.delay;
			if ((*next)->due <= now)
				(*next)->due = now + t.delay;
		} else {
			struct mm_timer *done = *next;
			*next = done->next;
			free(done);
		}
		timer_running = t.id;
		timer_running_flags = t.flags;
		pthread_mutex_unlock(&timer_lock);
		fire(&t);
		pthread_mutex_lock(&timer_lock);
		timer_running = 0;
		pthread_cond_broadcast(&timer_done);
	}
	return 0;
}

static int timer_start(void)
{
	if (timer_started)
		return 0;
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&timer_cond, &attr);
	pthread_condattr_destroy(&attr);
	pthread_cond_init(&timer_done, NULL);
	HANDLE h = thread_create(timer_main, NULL);
	if (!h)
		return -1;
	handle_close(h);
	timer_started = 1;
	return 0;
}

WINFN(UINT) mm_timeSetEvent(UINT delay, UINT resolution,
			    LPTIMECALLBACK callback, DWORD_PTR user, UINT flags)
{
	struct mm_timer *t;

	if (!delay || delay > TIMER_MAX || !callback ||
	    !(t = calloc(1, sizeof(*t))))
		return 0;
	t->delay = delay;
	t->flags = flags;
	t->callback = callback;
	t->user = user;
	t->due = now_ms() + delay;

	pthread_mutex_lock(&timer_lock);
	if (timer_start()) {
		pthread_mutex_unlock(&timer_lock);
		free(t);
		return 0;
	}
	t->id = timer_next_id++;
	if (!timer_next_id)
		timer_next_id = 1;
	t->next = timers;
	timers = t;
	pthread_cond_signal(&timer_cond);
	UINT id = t->id;
	pthread_mutex_unlock(&timer_lock);
	TRACE("timeSetEvent(%u ms, %s) = %u", delay,
	      (flags & TIME_PERIODIC) ? "periodic" : "one-shot", id);
	return id;
}

/*
 * With TIME_KILL_SYNCHRONOUS, waits for a callback of the timer that is
 * running (a one-shot timer's included), unless the timer thread itself
 * is the caller.
 */
WINFN(UINT) mm_timeKillEvent(UINT id)
{
	struct mm_timer *t = NULL;
	UINT flags = 0;

	if (!id)
		return MMSYSERR_INVALPARAM;
	pthread_mutex_lock(&timer_lock);
	for (struct mm_timer **p = &timers; *p; p = &(*p)->next) {
		if ((*p)->id == id) {
			t = *p;
			*p = t->next;
			flags = t->flags;
			break;
		}
	}
	int found = t || timer_running == id;
	if (!t && found)
		flags = timer_running_flags;
	if (found && (flags & TIME_KILL_SYNCHRONOUS) &&
	    thread_current_id() != timer_thread_id)
		while (timer_running == id)
			pthread_cond_wait(&timer_done, &timer_lock);
	pthread_mutex_unlock(&timer_lock);
	free(t);
	return found ? TIMERR_NOERROR : MMSYSERR_INVALPARAM;
}

WINFN(UINT) mm_timeBeginPeriod(UINT period)
{
	return period ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

WINFN(UINT) mm_timeEndPeriod(UINT period)
{
	return period ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

WINFN(UINT) mm_timeGetDevCaps(UINT *caps, UINT size)
{
	if (!caps || size < 2 * sizeof(UINT))
		return TIMERR_NOCANDO;
	caps[0] = 1;		/* wPeriodMin */
	caps[1] = TIMER_MAX;	/* wPeriodMax */
	return TIMERR_NOERROR;
}

#define E(name) { #name, 0, (void *)mm_##name }

static const struct builtin_export exports[] = {
	E(CloseDriver),
	E(DefDriverProc),
	{ "DrvClose", 0, (void *)mm_CloseDriver },
	{ "DrvDefDriverProc", 0, (void *)mm_DefDriverProc },
	{ "DrvGetModuleHandle", 0, (void *)mm_GetDriverModuleHandle },
	{ "DrvOpen", 0, (void *)mm_OpenDriver },
	{ "DrvSendMessage", 0, (void *)mm_SendDriverMessage },
	E(GetDriverModuleHandle),
	E(OpenDriver),
	E(OpenDriverA),
	E(SendDriverMessage),
	E(timeBeginPeriod),
	E(timeEndPeriod),
	E(timeGetDevCaps),
	E(timeGetTime),
	E(timeKillEvent),
	E(timeSetEvent),
	E(waveOutClose),
	E(waveOutOpen),
	E(waveOutPause),
	E(waveOutPrepareHeader),
	E(waveOutReset),
	E(waveOutRestart),
	E(waveOutUnprepareHeader),
	E(waveOutWrite),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_winmm = { "winmm.dll", NULL, tables, 0 };
