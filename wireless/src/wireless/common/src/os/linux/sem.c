#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <os_net_utils.h>
#include <os_net_sem.h>

os_net_status_t os_net_sem_create(os_net_sem_t *sem, uint32_t init_count, uint32_t max_count)
{
    if (sem_init(sem, 0, init_count) != 0)
        return OS_NET_STATUS_NOMEM;
    return OS_NET_STATUS_OK;
}

os_net_status_t os_net_sem_delete(os_net_sem_t *sem)
{
    if (sem_destroy(sem) != 0) {
        return OS_NET_STATUS_FAILED;
    }
    return OS_NET_STATUS_OK;
}

os_net_status_t os_net_sem_wait(os_net_sem_t *sem, os_net_time_t wait_ms)
{
	struct timespec ts;
	int eintr_count;

	if(wait_ms == 0 || wait_ms == OS_NET_WAIT_FOREVER) {
		/* Retry on signal interruption; only a real error fails. */
		eintr_count = 0;
		while (sem_wait(sem) != 0) {
			if (errno != EINTR) {
				OS_NET_ERROR("sem_wait(forever) real error errno=%d(%s), return FAILED", errno, strerror(errno));
				return OS_NET_STATUS_FAILED;
			}
			eintr_count++;
		}
		if (eintr_count > 0)
			OS_NET_WARN("sem_wait(forever) interrupted by signal %d time(s), retried and succeeded", eintr_count);
		return OS_NET_STATUS_OK;
	}

	if(clock_gettime(CLOCK_REALTIME, &ts) < 0) {
		return OS_NET_STATUS_FAILED;
	}

	ts.tv_sec += wait_ms / 1000;
	ts.tv_nsec += wait_ms % 1000 * 1000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec += ts.tv_nsec / 1000000000;
		ts.tv_nsec %= 1000000000;
	}

	/* Retry on signal interruption; only a real timeout/error fails. */
	eintr_count = 0;
	while (sem_timedwait(sem, (const struct timespec *) &ts) != 0) {
		if (errno != EINTR) {
			OS_NET_ERROR("sem_timedwait real error errno=%d(%s), return FAILED", errno, strerror(errno));
			return OS_NET_STATUS_FAILED;
		}
		eintr_count++;
	}
	if (eintr_count > 0)
		OS_NET_WARN("sem_timedwait interrupted by signal %d time(s), retried and succeeded", eintr_count);

    return OS_NET_STATUS_OK;
}

os_net_status_t os_net_sem_release(os_net_sem_t *sem)
{
    if (sem_post(sem) != 0) {
        return OS_NET_STATUS_FAILED;
    }
    return OS_NET_STATUS_OK;
}
