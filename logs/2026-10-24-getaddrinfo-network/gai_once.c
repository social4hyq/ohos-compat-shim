#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo *res = NULL;
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	int rc = getaddrinfo(argv[1], NULL, &hints, &res);
	clock_gettime(CLOCK_MONOTONIC, &b);
	long us = (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000;
	printf("%ld %d\n", us / 1000, rc);
	if (rc == 0)
		freeaddrinfo(res);
	return 0;
}
