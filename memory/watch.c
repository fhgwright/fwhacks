/*
 * watch.c -- program to watch a memory location
 *
 * NOTE: "-lrt" may be necessary to build this.
 */

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>

#ifndef MAP_NOCACHE
#define MAP_NOCACHE 0
#endif

#define MAX_SAMPLES 10000000
#define TIMING_TEST_SAMPLES 10

#define TIME_DIFF_MULT 10.0
#define TIME_DIFF_MIN 1.0E-3

#define RUN_PRIORITY -20

/* Defs for open-coded loops */
#define OC_SHIFT 3
#define OC_NUM (1 << OC_SHIFT)
#define OC_MASK (OC_NUM - 1)

typedef long long time_ns;
#define TIME_NS_MAX INT64_MAX

typedef unsigned long pointer_int_t;

typedef unsigned long long sample_t;

typedef struct sample_s {
	time_ns		time;
	sample_t	value;
} tsample_t;

static int page_size = 0;
static int page_mask;
static int mem_fd = -1;
static int mem_prot;
static void *mem_virt = NULL;
static void *mem_phys = NULL;
static uint32_t mapped_size = 0;
static int accessing = 0;
static jmp_buf access_err;
static int orig_prio;

static char *
getarg(const char *str, unsigned long long *val, int radix)
{
char *cp;

	*val = strtoull(str, &cp, radix);
	return cp;
}

static int
getnum(const char *str, unsigned long long *val, int radix)
{
	return *getarg(str, val, radix) != 0;
}

static int
getfloat(const char *str, double *val)
{
char *cp;

	*val = strtod(str, &cp);
	return *cp != 0;
}

static int
getadr(char *str, unsigned long long *val)
{
char *cp = str;
int sign = '+';
unsigned long long val2;

	*val = 0;
	while (1) {
		cp = getarg(cp, &val2, 16);
		if (sign == '-') *val -= val2; else *val += val2;
		sign = *cp++;
		if (!sign) return 0;
		if (sign != '+' && sign != '-') return 1;
	}
}

static int
getsize(int sizechar)
{
	switch (sizechar) {

	case 'b': case 'B':
		return 1;

	case 'w': case 'W':
		return 2;

	case 'l': case 'L':
		return 4;

	case 'q': case 'Q':
		return 8;
	}
	return 0;
}

static void
sig_handler(int sig, siginfo_t *info, void *uap)
{
	if (accessing) {
		accessing = 0;
		longjmp(access_err, sig);
	}
}


static const char *
set_signals(void)
{
int err;
struct sigaction sigact = {
	.sa_sigaction = sig_handler,
	.sa_mask = 0,
	.sa_flags = SA_SIGINFO,
};

	if (sigaction(SIGSEGV, &sigact, NULL)) return strerror(errno);
	if (sigaction(SIGBUS, &sigact, NULL)) return strerror(errno);

	return NULL;
}

static const char *
open_mem(int rw, int kmem)
{
const char *mname = kmem ? "/dev/kmem" : "/dev/mem";
static char errmsg[100];

	if ((page_size = sysconf(_SC_PAGESIZE)) < 0) {
		return "Can't get page size";
	}
	page_mask = page_size - 1;
	if ((mem_fd = open(mname, rw ? O_RDWR : O_RDONLY)) < 0) {
		sprintf(errmsg, "Can't open %s", mname);
		return errmsg;
	}
	mem_prot = rw ? PROT_WRITE : PROT_READ;
	return NULL;
}

static void
close_mem(void)
{
	if (mem_virt) {
		(void) munmap(mem_virt, mapped_size);
		mem_virt = NULL; mapped_size = 0;
	}
	if (mem_fd >= 0) {
		(void) close(mem_fd);
		mem_fd = -1;
	}
}

static const char *
map_adr(void *phys_adr, int size, void **virt_adrp)
{
uint32_t page_ofs = (pointer_int_t) phys_adr & page_mask;
uint32_t map_size = ((page_ofs + size - 1) | page_mask) + 1;
pointer_int_t page_base = (pointer_int_t) phys_adr - page_ofs;

	if ((pointer_int_t) mem_phys != page_base || map_size > mapped_size) {
		mem_virt = mmap(mem_virt, map_size, mem_prot,
				(mem_virt ? MAP_FIXED : 0)
				| MAP_SHARED | MAP_NOCACHE,
				mem_fd, page_base);
		if (mem_virt == MAP_FAILED) {
			mem_phys = NULL; mapped_size = 0;
			return strerror(errno);
		}
		mem_phys = (void *) page_base;
		mapped_size = map_size;
	}
	*virt_adrp = (void *) ((pointer_int_t) mem_virt + page_ofs);
	return NULL;
}

static const char *
set_priority(void)
{
	errno = 0;
	if ((orig_prio = getpriority(PRIO_PROCESS, 0)) < 0 && errno) {
		return "Can't get priority";
	}
	if (setpriority(PRIO_PROCESS, 0, RUN_PRIORITY)) {
		return "Can't set priority";
	}
	return NULL;
}

static void
restore_priority(void)
{
	(void) setpriority(PRIO_PROCESS, 0, orig_prio);
}

time_ns
get_time_ns(void)
{
time_t sec;
int32_t nsec;

#ifdef CLOCK_REALTIME
struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return -1;
  sec = ts.tv_sec; nsec = ts.tv_nsec;
#else
struct timeval tv;
  if (gettimeofday(&tv, NULL) < 0) return -1;
  sec = tv.tv_sec; nsec = tv.tv_usec * 1000;
#endif

  /* Treat 32-bit time_t as unsigned to extend range past 2038 */
  if (sizeof(time_t) <= 4) {
    return (uint32_t) sec * 1000000000LL + nsec;
  }
  return sec * 1000000000LL + nsec;
}

int
sleep_ns(time_ns delay)
{
struct timespec ts;

  ts.tv_sec = delay / 1000000000LL;
  ts.tv_nsec = delay % 1000000000LL;
  return nanosleep(&ts, NULL);
}

static time_ns
get_sample_time(void)
{
time_ns ts[TIMING_TEST_SAMPLES], *tp = &ts[0];
time_ns *te = &ts[TIMING_TEST_SAMPLES - 1];
time_ns last, diff, mindiff;

	while (tp <= te) {
		if ((*tp++ = get_time_ns()) < 0) return -1;
	}

	tp = &ts[0];
	mindiff = TIME_NS_MAX;
	while (tp < te) {
	  last = *tp++; diff = *tp - last;
		if (diff < mindiff) mindiff = diff;
	}

	return mindiff;
}

static const char *
collect_samples(unsigned long long adr, int size, tsample_t *buf, int num)
{
void *vadr;
const char *err;
int errsig;
tsample_t *end = buf + num;

	if ((err = map_adr((void *) (pointer_int_t) adr, size, &vadr))) {
		return err;
	}

	errsig = setjmp(access_err);
	if (errsig) return strsignal(errsig);

  if (sleep_ns(1000000) < 0) return "Initial sleep failed";

	accessing = 1;
	
	switch (size) {

	case 1:
		do {
		if ((buf->time = get_time_ns()) < 0) {
				return "Can't read clock";
			}
			buf->value = *((const volatile uint8_t *) vadr);
		} while (++buf < end);
		break;

	case 2:
		do {
		if ((buf->time = get_time_ns()) < 0) {
				return "Can't read clock";
			}
			buf->value = *((const volatile uint16_t *) vadr);
		} while (++buf < end);
		break;

	case 4:
		do {
		if ((buf->time = get_time_ns()) < 0) {
				return "Can't read clock";
			}
			buf->value = *((const volatile uint32_t *) vadr);
		} while (++buf < end);
		break;

	case 8:
		do {
		if ((buf->time = get_time_ns()) < 0) {
				return "Can't read clock";
			}
			buf->value = *((const volatile uint64_t *) vadr);
		} while (++buf < end);
	}

	accessing = 0;
	return NULL;
}

static const char *
untimed_samples(unsigned long long adr, int size, sample_t *buf, int num,
                time_ns *before, time_ns *after)
{
void *vadr;
const char *err;
int errsig;
int count = num >> OC_SHIFT;

	if ((err = map_adr((void *) (pointer_int_t) adr, size, &vadr))) {
		return err;
	}

	errsig = setjmp(access_err);
	if (errsig) return strsignal(errsig);

  if (sleep_ns(1000000) < 0) return "Initial sleep failed";
  if ((*before = get_time_ns()) < 0) return "Can't read start time";

	accessing = 1;

  /* Note that number of open-coded fetches must match OC_NUM */

	switch (size) {

	case 1:
		do {
			*buf++ = *((const volatile uint8_t *) vadr);
			*buf++ = *((const volatile uint8_t *) vadr);
			*buf++ = *((const volatile uint8_t *) vadr);
			*buf++ = *((const volatile uint8_t *) vadr);
			*buf++ = *((const volatile uint8_t *) vadr);
			*buf++ = *((const volatile uint8_t *) vadr);
			*buf++ = *((const volatile uint8_t *) vadr);
			*buf++ = *((const volatile uint8_t *) vadr);
		} while (--count);
		break;

	case 2:
		do {
			*buf++ = *((const volatile uint16_t *) vadr);
			*buf++ = *((const volatile uint16_t *) vadr);
			*buf++ = *((const volatile uint16_t *) vadr);
			*buf++ = *((const volatile uint16_t *) vadr);
			*buf++ = *((const volatile uint16_t *) vadr);
			*buf++ = *((const volatile uint16_t *) vadr);
			*buf++ = *((const volatile uint16_t *) vadr);
			*buf++ = *((const volatile uint16_t *) vadr);
		} while (--count);
		break;

	case 4:
		do {
			*buf++ = *((const volatile uint32_t *) vadr);
			*buf++ = *((const volatile uint32_t *) vadr);
			*buf++ = *((const volatile uint32_t *) vadr);
			*buf++ = *((const volatile uint32_t *) vadr);
			*buf++ = *((const volatile uint32_t *) vadr);
			*buf++ = *((const volatile uint32_t *) vadr);
			*buf++ = *((const volatile uint32_t *) vadr);
			*buf++ = *((const volatile uint32_t *) vadr);
		} while (--count);
		break;

	case 8:
		do {
			*buf++ = *((const volatile uint64_t *) vadr);
			*buf++ = *((const volatile uint64_t *) vadr);
			*buf++ = *((const volatile uint64_t *) vadr);
			*buf++ = *((const volatile uint64_t *) vadr);
			*buf++ = *((const volatile uint64_t *) vadr);
			*buf++ = *((const volatile uint64_t *) vadr);
			*buf++ = *((const volatile uint64_t *) vadr);
			*buf++ = *((const volatile uint64_t *) vadr);
		} while (--count);
	}

	accessing = 0;

  if ((*after = get_time_ns()) < 0) return "Can't read end time";

	return NULL;
}

static void
print_results(unsigned long long mask, int size, tsample_t *buf, int num,
              int64_t maxdiff)
{
tsample_t *bp2, *end = buf + num - 1;
long long diff;

	do {
		bp2 = buf + 1;
		if ((diff = bp2->time - buf->time) > maxdiff
		    || (buf->value ^ bp2->value) & mask) {
			printf("%lld.%09d...%lld.%09d (%lld.%09d): "
			       " %0*llX->%0*llX\n",
			       buf->time / 1000000000LL, (int) (buf->time % 1000000000LL),
			       bp2->time / 1000000000LL, (int) (bp2->time % 1000000000LL),
			       diff / 1000000000LL, (int) (diff % 1000000000LL),
			       size * 2, buf->value,
			       size * 2, bp2->value);
		}
	} while ((buf = bp2) < end);
}

static void
print_untimed(unsigned long long mask, int size, sample_t *buf, int num)
{
int	adrmask = size > 4 ? 3 : size > 2 ? 7 : 15, ofs = 0;

	while (num--) {
		if (ofs && !(ofs & adrmask)) printf("\n");
		printf(" %0*llX", size * 2, *buf++);
		++ofs;
	}
	printf("\n");
}

int
main(int argc, char *argv[])
{
unsigned long long adr, val, mask;
unsigned long long duration = 2;
int size, untimed = 0, kmem = 0, num, retval = 0;
const char *sizarg = argv[1], *errmsg;
time_ns sample_time, before, after, delta;
double maxdiff, mindiff = TIME_DIFF_MIN;
tsample_t *tsamples = NULL;
sample_t *samples = NULL;

	switch (argc) {

	case 6:
		if (getnum(argv[5], &duration, 0) || !duration) {
			fprintf(stderr, "Bad duration\n");
			return 2;
		}
	case 5:
		if (getfloat(argv[4], &mindiff)) {
			fprintf(stderr, "Bad min time diff\n");
			return 3;
		}
	case 4:
		if (getadr(argv[2], &adr)) {
			fprintf(stderr, "Bad address\n");
			return 4;
		}
		if (*sizarg == 'u' || *sizarg == 'U') {
			untimed = 1;
			++sizarg;
		}
		if (*sizarg == 'k' || *sizarg == 'K') {
			kmem = 1;
			++sizarg;
		}
		if (!(size = getsize(*sizarg)) || *(sizarg + 1)) {
			fprintf(stderr, "Bad size\n");
			return 5;
		}
		if (getnum(argv[3], &mask, 16)) {
			fprintf(stderr, "Bad mask\n");
			return 6;
		}
		break;

	default:
		fprintf(stderr,
			"Usage is: "
			"watch [U][K](B|W|L|Q) address(hex) mask(hex)"
			" [min_time_diff [duration]]\n");
		return 1;
	}

	if ((errmsg = set_signals())) {
		fprintf(stderr, "Signal setup failed: %s\n", errmsg);
		return 7;
	}

	if ((errmsg = open_mem(0, kmem))) {
		perror(errmsg);
		return 8;
	}
	do {
    if ((errmsg = set_priority())) {
      perror(errmsg);
      retval = 9;
      break;
    }
    if ((sample_time = get_sample_time()) <= 0) {
      perror("Can't measure timing");
      retval = 10;
      break;
    }

		if (!untimed) {
      num = duration * 1000000000LL / sample_time;
      num = (num + OC_MASK) & ~OC_MASK;
      if (!(tsamples = malloc(num * sizeof(*tsamples)))) {
        perror("Can't allocate buffer");
        retval = 11;
        break;
      }
      if ((errmsg = collect_samples(adr, size, tsamples, num))) {
        fprintf(stderr, "%s at %llX\n", errmsg, adr);
        retval = 12;
        break;
      }
      restore_priority();
      maxdiff = sample_time / 1.0E9 * TIME_DIFF_MULT;
      if (maxdiff < mindiff) maxdiff = mindiff;
      print_results(mask, size, tsamples, num, maxdiff * 1.0E9);
    } else {
      num = duration * 8;
      num = (num + OC_MASK) & ~OC_MASK;
      if (!(samples = malloc(num * sizeof(*samples)))) {
        perror("Can't allocate buffer");
        retval = 11;
        break;
      }
      errmsg = untimed_samples(adr, size, samples, num, &before, &after);
      if (errmsg) {
        fprintf(stderr, "%s at %llX\n", errmsg, adr);
        retval = 12;
        break;
      }
      restore_priority();
      print_untimed(mask, size, samples, num);
      delta = after - before - sample_time;
      printf("Took %lld ns for %d samples, average = %d\n",
             delta, num, (int) (delta / num));
		}
	} while(0);
	if (tsamples) free(tsamples);
	if (samples) free(samples);
	close_mem();
	return retval;
}
