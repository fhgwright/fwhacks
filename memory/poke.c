/*
 * poke.c -- program to store memory location(s)
 */

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#ifndef MAP_NOCACHE
#define MAP_NOCACHE 0
#endif

typedef unsigned long pointer_int_t;

static int page_size = 0;
static int page_mask;
static int mem_fd = -1;
static int mem_prot;
static void *mem_virt = NULL;
static void *mem_phys = NULL;
static uint32_t mapped_size = 0;
static int accessing = 0;
static jmp_buf access_err;

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
dopoke(unsigned long long *adrp, unsigned long long val, int size, int stride)
{
void *adr;
const char *err;
int errsig;

	if ((err = map_adr((void *) (pointer_int_t) *adrp, size, &adr))) {
		return err;
	}

	errsig = setjmp(access_err);
	if (errsig) return strsignal(errsig);

	accessing = 1;
	
	switch (size) {

	case 1:
		*((volatile uint8_t *) adr) = val;
		break;

	case 2:
		*((volatile uint16_t *) adr) = val;
		break;

	case 4:
		*((volatile uint32_t *) adr) = val;
		break;

	case 8:
		*((volatile uint64_t *) adr) = val;
		break;
	}

	accessing = 0;
	*adrp += size * stride;
	return NULL;
}

int
main(int argc, char *argv[])
{
unsigned long long adr, val;
unsigned long long count = 1;
unsigned long long stride = 1;
int size, kmem = 0, mask, num = 3;
const char *sizarg = argv[1], *errmsg;

	if (argc < 4) {
		fprintf(stderr,
			"Usage is: "
			"poke [K](B|W|L|Q)[stride] "
			"address(hex) data(hex)...\n");
		return 1;
	}

	if (getadr(argv[2], &adr)) {
		fprintf(stderr, "Bad address\n");
		return 2;
	}
	if (*sizarg == 'k' || *sizarg == 'K') {
		kmem = 1;
		++sizarg;
	}
	if (!(size = getsize(*sizarg))) {
		fprintf(stderr, "Bad size\n");
		return 3;
	}
	++sizarg;
	if (*sizarg && getnum(sizarg, &stride, 0)) {
		fprintf(stderr, "Bad stride\n");
		return 4;
	}

	if ((errmsg = set_signals())) {
		fprintf(stderr, "Signal setup failed: %s\n", errmsg);
		return 8;
	}

	if ((errmsg = open_mem(1, kmem))) {
		perror(errmsg);
		return 6;
	}
	while (num < argc) {
		if (getnum(argv[num], &val, 16)) {
			fprintf(stderr, "Bad value: %s\n", argv[num]);
			close_mem();
			return 5;
		}
		if ((errmsg = dopoke(&adr, val, size, stride))) {
			fprintf(stderr, "%s at %llX\n", errmsg, adr);
			close_mem();
			return 7;
		}
		++num;
	}
	close_mem();
	return 0;
}
