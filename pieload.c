#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

extern char **environ;

static int verbose;
static size_t pgsz;

#define LOG(...)                                                              \
	do {                                                                  \
		if (verbose)                                                  \
			fprintf(stderr, "pieload: " __VA_ARGS__);             \
	} while (0)

#define PGDOWN(x) ((x) & ~(unsigned long)(pgsz - 1))
#define PGUP(x) (((x) + pgsz - 1) & ~(unsigned long)(pgsz - 1))

static void die(const char *m)
{
	fprintf(stderr, "pieload: %s: %s\n", m, strerror(errno));
	_exit(127);
}

struct image {
	unsigned long base, entry, phdr, phnum, phent;
};

static struct image map_elf(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		die(path);

	Elf64_Ehdr eh;
	if (pread(fd, &eh, sizeof eh, 0) != sizeof eh)
		die("read ehdr");
	if (memcmp(eh.e_ident, ELFMAG, SELFMAG)) {
		fprintf(stderr, "pieload: %s: not an ELF\n", path);
		_exit(127);
	}

	size_t psz = (size_t)eh.e_phentsize * eh.e_phnum;
	Elf64_Phdr *ph = malloc(psz);
	if (!ph || pread(fd, ph, psz, eh.e_phoff) != (ssize_t)psz)
		die("read phdrs");

	unsigned long lo = ~0UL, hi = 0;
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_vaddr < lo)
			lo = ph[i].p_vaddr;
		if (ph[i].p_vaddr + ph[i].p_memsz > hi)
			hi = ph[i].p_vaddr + ph[i].p_memsz;
	}
	if (lo > hi)
		die("no PT_LOAD");

	unsigned long span = PGUP(hi) - PGDOWN(lo), base = 0;
	if (eh.e_type == ET_DYN) {
		void *r = mmap(NULL, span, PROT_NONE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (r == MAP_FAILED)
			die("reserve interp");
		base = (unsigned long)r - PGDOWN(lo);
	} else {
		void *r = mmap((void *)PGDOWN(lo), span, PROT_NONE,
			       MAP_PRIVATE | MAP_ANONYMOUS |
				       MAP_FIXED_NOREPLACE,
			       -1, 0);
		if (r == MAP_FAILED || r != (void *)PGDOWN(lo))
			die("address range for ET_EXEC is taken");
	}
	LOG("%s base=%#lx span=%#lx\n", path, base, span);

	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		unsigned long va = base + ph[i].p_vaddr;
		unsigned long vs = PGDOWN(va), delta = va - vs;
		unsigned long flen = PGUP(ph[i].p_filesz + delta);
		unsigned long mlen = PGUP(ph[i].p_memsz + delta);
		int prot = (ph[i].p_flags & PF_R ? PROT_READ : 0) |
			   (ph[i].p_flags & PF_W ? PROT_WRITE : 0) |
			   (ph[i].p_flags & PF_X ? PROT_EXEC : 0);

		if (flen &&
		    mmap((void *)vs, flen, prot, MAP_PRIVATE | MAP_FIXED, fd,
			 PGDOWN(ph[i].p_offset)) == MAP_FAILED)
			die("mmap segment");
		if (mlen > flen &&
		    mmap((void *)(vs + flen), mlen - flen, prot,
			 MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1,
			 0) == MAP_FAILED)
			die("mmap bss");

		if ((prot & PROT_WRITE) && ph[i].p_memsz > ph[i].p_filesz) {
			unsigned long bss = va + ph[i].p_filesz;
			if (PGUP(bss) > bss)
				memset((void *)bss, 0, PGUP(bss) - bss);
		}
		LOG("  seg %d -> %#lx len %#lx prot %d\n", i, vs, mlen, prot);
	}

	struct image im = { base, base + eh.e_entry, base + eh.e_phoff,
			    eh.e_phnum, eh.e_phentsize };
	for (int i = 0; i < eh.e_phnum; i++)
		if (ph[i].p_type == PT_PHDR)
			im.phdr = base + ph[i].p_vaddr;
	free(ph);
	close(fd);
	return im;
}

static unsigned long own_auxv[128];
static int own_n;

static void read_auxv(void)
{
	int fd = open("/proc/self/auxv", O_RDONLY);
	if (fd < 0)
		die("open auxv");
	ssize_t r = read(fd, own_auxv, sizeof own_auxv);
	close(fd);
	if (r <= 0)
		die("read auxv");
	own_n = r / 8;
}

#define SYS_faccessat2 439
#define NR_faccessat 48

static long raw6(long nr, unsigned long a, unsigned long b, unsigned long c,
		 unsigned long d, unsigned long e, unsigned long f)
{
	register long x0 __asm__("x0") = (long)a;
	register unsigned long x1 __asm__("x1") = b;
	register unsigned long x2 __asm__("x2") = c;
	register unsigned long x3 __asm__("x3") = d;
	register unsigned long x4 __asm__("x4") = e;
	register unsigned long x5 __asm__("x5") = f;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile("svc 0"
			 : "+r"(x0)
			 : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
			 : "memory");
	return x0;
}

static void sigsys(int sig, siginfo_t *si, void *ucv)
{
	unsigned long *r = (unsigned long *)((ucontext_t *)ucv)->uc_mcontext.regs;

	switch ((long)r[8]) {
	case SYS_faccessat2:
		r[0] = (unsigned long)raw6(NR_faccessat, r[0], r[1], r[2], 0, 0,
					   0);
		break;
	default:
		r[0] = (unsigned long)-ENOSYS;
		break;
	}
}

static void shim_seccomp(void)
{
	struct sigaction sa = { 0 };
	sa.sa_sigaction = sigsys;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigaction(SIGSYS, &sa, NULL);
}

static int overridden(unsigned long t)
{
	return t == AT_PHDR || t == AT_PHENT || t == AT_PHNUM ||
	       t == AT_BASE || t == AT_ENTRY || t == AT_EXECFN ||
	       t == AT_FLAGS || t == AT_NULL;
}

int main(int argc, char **argv)
{
	pgsz = (size_t)sysconf(_SC_PAGESIZE);
	verbose = getenv("PIELOAD_VERBOSE") != NULL;

	if (argc < 3) {
		fprintf(stderr,
			"usage: pieload <interpreter> <program> [args...]\n");
		return 2;
	}
	const char *interp_path = argv[1];
	const char *prog_path = argv[2];
	int nargs = argc - 2;
	char **args = argv + 2;

	read_auxv();
	struct image prog = map_elf(prog_path);
	struct image interp = map_elf(interp_path);
	LOG("prog entry=%#lx phdr=%#lx phnum=%lu interp entry=%#lx base=%#lx\n",
	    prog.entry, prog.phdr, prog.phnum, interp.entry, interp.base);

	int nenv = 0;
	for (char **e = environ; *e; e++)
		if (strncmp(*e, "LD_PRELOAD=", 11))
			nenv++;

	size_t strsz = 0;
	for (int i = 0; i < nargs; i++)
		strsz += strlen(args[i]) + 1;
	for (char **e = environ; *e; e++)
		if (strncmp(*e, "LD_PRELOAD=", 11))
			strsz += strlen(*e) + 1;

	size_t naux = 0;
	for (int i = 0; i + 1 < own_n && own_auxv[i]; i += 2)
		if (!overridden(own_auxv[i]))
			naux++;
	naux += 7;

	size_t words = 1 + (nargs + 1) + (nenv + 1) + 2 * (naux + 1);
	size_t need = words * 8 + strsz + 64;

	size_t stksz = 32UL << 20;
	if (need + pgsz > stksz)
		stksz = PGUP(need) + pgsz;
	void *stk = mmap(NULL, stksz, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stk == MAP_FAILED)
		die("mmap stack");

	char *strp = (char *)stk + stksz - 16 - strsz;
	unsigned long *sp = (unsigned long *)(((unsigned long)strp -
					       words * 8) &
					      ~15UL);

	unsigned long *w = sp;
	*w++ = (unsigned long)nargs;
	char *s = strp;
	for (int i = 0; i < nargs; i++) {
		size_t l = strlen(args[i]) + 1;
		memcpy(s, args[i], l);
		*w++ = (unsigned long)s;
		s += l;
	}
	*w++ = 0;
	for (char **e = environ; *e; e++) {
		if (!strncmp(*e, "LD_PRELOAD=", 11))
			continue;
		size_t l = strlen(*e) + 1;
		memcpy(s, *e, l);
		*w++ = (unsigned long)s;
		s += l;
	}
	*w++ = 0;
	char *execfn = (char *)sp[1];

	for (int i = 0; i + 1 < own_n && own_auxv[i]; i += 2) {
		if (overridden(own_auxv[i]))
			continue;
		*w++ = own_auxv[i];
		*w++ = own_auxv[i + 1];
	}
	*w++ = AT_PHDR;
	*w++ = prog.phdr;
	*w++ = AT_PHENT;
	*w++ = prog.phent;
	*w++ = AT_PHNUM;
	*w++ = prog.phnum;
	*w++ = AT_BASE;
	*w++ = interp.base;
	*w++ = AT_ENTRY;
	*w++ = prog.entry;
	*w++ = AT_FLAGS;
	*w++ = 0;
	*w++ = AT_EXECFN;
	*w++ = (unsigned long)execfn;
	*w++ = AT_NULL;
	*w++ = 0;

	LOG("stack=%p sp=%p argc=%d nenv=%d\n", stk, (void *)sp, nargs, nenv);
	fflush(NULL);
	shim_seccomp();

	register unsigned long x8 __asm__("x8") = interp.entry;
	register unsigned long x9 __asm__("x9") = (unsigned long)sp;
	__asm__ volatile("mov sp, x9\n"
			 "mov x0, xzr\n"
			 "mov x1, xzr\n"
			 "mov x2, xzr\n"
			 "mov x3, xzr\n"
			 "mov x29, xzr\n"
			 "mov x30, xzr\n"
			 "br x8\n"
			 :
			 : "r"(x8), "r"(x9)
			 : "memory");
	__builtin_unreachable();
}
