#include <pthread.h>

#include "common/log.h"
#include "runtime/runtime.h"
#include "runtime/teb.h"

static pthread_once_t once = PTHREAD_ONCE_INIT;

static void process_init(void)
{
	vfw_log_init();
	peb_get();
}

int runtime_enter(void)
{
	pthread_once(&once, process_init);
	return teb_init_thread() ? 0 : -1;
}
