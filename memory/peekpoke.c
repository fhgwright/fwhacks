/*
 * peekpoke.c -- program to read and write memory location(s)
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>

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

static int
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
      mem_phys = NULL; mem_virt = NULL; mapped_size = 0;
      /* Return -1 if mmap() is unsupported for this dev */
      return errno == ENODEV ? -1 : errno;
    }
    mem_phys = (void *) page_base;
    mapped_size = map_size;
  }
  *virt_adrp = (void *) ((pointer_int_t) mem_virt + page_ofs);
  return 0;
}

static const char *
doread(int fd, void *valp, size_t size, uint64_t adr)
{
int ret;
static char errbuf[100];

  *((uint64_t *) valp) = 0xDEADBEEFDEADBEEFULL;
  ret = pread(fd, valp, size, adr);

  if (ret == size) return NULL;
  if (ret < 0) return strerror(errno);
  snprintf(errbuf, sizeof(errbuf), "pread result %d != %d", ret, (int) size);
  return errbuf;
}

static const char *
dowrite(int fd, const void *valp, size_t size, uint64_t adr)
{
int ret;
static char errbuf[100];

  ret = pwrite(fd, valp, size, adr);

  if (ret == size) return NULL;
  if (ret < 0) return strerror(errno);
  snprintf(errbuf, sizeof(errbuf), "pwrite result %d != %d", ret, (int) size);
  return errbuf;
}

static const char *
dopeek(unsigned long long *adrp, unsigned long long *valp, int size, int stride)
{
void *adr;
int err;
int errsig;
const char *errmsg;

  if ((err = map_adr((void *) (pointer_int_t) *adrp, size, &adr))) {
    if (err == -1) {
      /* If mmap() doesn't work, try pread() */
      if ((errmsg = doread(mem_fd, valp, size, (uint64_t) *adrp))) {
        return errmsg;
      }
    *adrp += size * stride;
    return NULL;
    }
    return strerror(err);
  }

  errsig = setjmp(access_err);
  if (errsig) return strsignal(errsig);

  accessing = 1;
  
  switch (size) {

  case 1:
    *valp = *((const volatile uint8_t *) adr);
    break;

  case 2:
    *valp = *((const volatile uint16_t *) adr);
    break;

  case 4:
    *valp = *((const volatile uint32_t *) adr);
    break;

  case 8:
    *valp = *((const volatile uint64_t *) adr);
    break;
  }

  accessing = 0;
  *adrp += size * stride;
  return NULL;
}

static const char *
dopoke(unsigned long long *adrp, unsigned long long *valp, int size, int stride)
{
void *adr;
int err;
int errsig;
const char *errmsg;

  if ((err = map_adr((void *) (pointer_int_t) *adrp, size, &adr))) {
    if (err == -1) {
      /* If mmap() doesn't work, try pwrite() */
      if ((errmsg = doread(mem_fd, valp, size, (uint64_t) *adrp))) {
        return errmsg;
      }
    *adrp += size * stride;
    return NULL;
    }
    return strerror(err);
  }

  errsig = setjmp(access_err);
  if (errsig) return strsignal(errsig);

  accessing = 1;
  
  switch (size) {

  case 1:
    *((volatile uint8_t *) adr) = *valp;
    break;

  case 2:
    *((volatile uint16_t *) adr) = *valp;
    break;

  case 4:
    *((volatile uint32_t *) adr) = *valp;
    break;

  case 8:
    *((volatile uint64_t *) adr) = *valp;
    break;
  }

  accessing = 0;
  *adrp += size * stride;
  return NULL;
}

static void
usage(FILE *out, int mode, const char *prog)
{
  const char *dtext = mode < 0 ? "data(hex)..." : "[count]";
  if (!mode) {
    fprintf(out, "Program name '%s' must be 'peek' or 'poke'\n", prog);
  } else {
    fprintf(out,
            "Usage is: %s [K](B|W|L|Q)[stride] address(hex) %s\n",
            prog, dtext);
  }
}

int
main(int argc, char *argv[])
{
unsigned long long adr, val;
unsigned long long count = 1;
unsigned long long stride = 1;
int mode = 0, minargs, maxargs, size, kmem = 0, mask, idx;
const char *sizarg = argv[1], *errmsg;
const char *prog = basename(argv[0]);

  if (!strcmp(prog, "peek")) {
    mode = 1; minargs = 3; maxargs = 4;
  }
  if (!strcmp(prog, "poke")) {
    mode = -1; minargs = 4; maxargs = INT_MAX;
  }
  if (!mode || argc < minargs || argc > maxargs) {
    usage(stderr, mode, prog);
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
  if (mode > 0 && argc == 4) {
    if (getnum(argv[3], &count, 0)) {
      fprintf(stderr, "Bad count\n");
      return 5;
    }
  }

  if ((errmsg = set_signals())) {
    fprintf(stderr, "Signal setup failed: %s\n", errmsg);
    return 6;
  }

  if ((errmsg = open_mem(mode < 0, kmem))) {
    perror(errmsg);
    return 7;
  }

  if (mode > 0) {
    mask = size > 4 ? 3 : size > 2 ? 7 : 15;
    idx = 0;
    while (count--) {
      if (idx && !(idx & mask)) printf("\n");
      if ((errmsg = dopeek(&adr, &val, size, stride))) {
        fprintf(stderr, "%s at %llX\n", errmsg, adr);
        close_mem();
        return 8;
      }
      val &= (1 << (size * 8)) - 1;
      printf(" %0*llX", size * 2, val);
      ++idx;
    }
    printf("\n");
  } else {
    idx = minargs - 1;
    while (idx < argc) {
      if (getnum(argv[idx], &val, 16)) {
        fprintf(stderr, "Bad value: %s\n", argv[idx]);
        close_mem();
        return 9;
      }
      if ((errmsg = dopoke(&adr, &val, size, stride))) {
        fprintf(stderr, "%s at %llX\n", errmsg, adr);
        close_mem();
        return 10;
      }
      ++idx;
    }
  }

  close_mem();
  return 0;
}
