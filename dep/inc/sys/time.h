#pragma once
#include <stdint.h>
/*
 * Structure used in select() call, taken from the BSD file sys/time.h.
 */
struct _timeval {
	uint64_t    tv_sec;         /* seconds */
	uint64_t    tv_usec;        /* and microseconds */
};