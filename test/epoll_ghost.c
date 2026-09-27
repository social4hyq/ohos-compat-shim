/* Deterministic kernel fault injection: an ONESHOT event keeps arriving
 * after DEL, including when DEL reports that the entry is already gone. */
#include "../src/ohos_compat_shim.c"

static unsigned wait_calls;
static struct epoll_event ghost;

static int ghost_wait(int epfd, struct epoll_event *events, int maxevents,
                      int timeout, const sigset_t *mask)
{
	(void)epfd;
	(void)maxevents;
	(void)timeout;
	(void)mask;
	if (++wait_calls > 1000) {
		errno = ELOOP;
		return -1;
	}
	events[0] = ghost;
	return 1;
}

static int run_case(int timeout, int already_deleted)
{
	int p[2];
	if (pipe(p) != 0)
		return 1;
	int epfd = epoll_create1(EPOLL_CLOEXEC);
	ghost = (struct epoll_event){ .events = EPOLLOUT,
		.data.u64 = 0x12345678 };
	struct epoll_event registration = ghost;
	registration.events |= EPOLLONESHOT;
	if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, p[1], &registration) != 0)
		return 1;
	int (*kernel_ctl)(int, int, int, struct epoll_event *) =
		dlsym(RTLD_NEXT, "epoll_ctl");
	if (already_deleted && kernel_ctl(epfd, EPOLL_CTL_DEL, p[1], NULL) != 0)
		return 1;

	struct epoll_event event;
	wait_calls = 0;
	int first = ep_shim_wait(epfd, &event, 1, 0, ghost_wait, NULL);
	long long before = ep_now_ms();
	int result = ep_shim_wait(epfd, &event, 1, timeout, ghost_wait, NULL);
	long long elapsed = ep_now_ms() - before;
	int failed = first != 1 || result != 0 || elapsed < timeout || wait_calls > 300;
	printf("%s timeout=%d already_deleted=%d first=%d result=%d calls=%u elapsed=%lldms\n",
	       failed ? "FAIL" : "PASS", timeout, already_deleted,
	       first, result, wait_calls, elapsed);
	close(epfd);
	close(p[0]);
	close(p[1]);
	return failed;
}

int main(void)
{
	int failed = 0;
	failed += run_case(30, 0);
	failed += run_case(30, 1);
	failed += run_case(300, 0);
	failed += run_case(300, 1);
	return failed != 0;
}
