typedef unsigned long size_t;
typedef long ssize_t;
typedef int pid_t;

#define RTLD_NEXT ((void *)-1)
#define O_RDONLY 0
#define O_CLOEXEC 02000000
#define X_OK 1

extern char **environ;
void *dlsym(void *, const char *);
int open(const char *, int, ...);
ssize_t read(int, void *, size_t);
int close(int);
int access(const char *, int);
size_t strlen(const char *);
int strncmp(const char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
void *memcpy(void *, const void *, size_t);

#define LINKER "/system/bin/linker64"
#define SAVED "PIELOAD_TERMUX_PRELOAD="
#define MAXARG 4096
#define PATHMAX 4096

typedef int (*execve_fn)(const char *, char *const[], char *const[]);
typedef int (*spawn_fn)(pid_t *, const char *, const void *, const void *,
			char *const[], char *const[]);

static int starts(const char *s, const char *p)
{
	return !strncmp(s, p, strlen(p));
}

static int system_path(const char *p)
{
	return starts(p, "/system/") || starts(p, "/apex/") ||
	       starts(p, "/vendor/") || starts(p, "/product/") ||
	       starts(p, "/odm/");
}

static const char *env_get(char *const *envp, const char *key)
{
	size_t n = strlen(key);
	for (; envp && *envp; envp++)
		if (!strncmp(*envp, key, n))
			return *envp + n;
	return 0;
}

static size_t count(char *const *v)
{
	size_t n = 0;
	while (v && v[n])
		n++;
	return n;
}

struct plan {
	const char *path;
	char *argv[MAXARG];
	char *envp[MAXARG];
	char preload[PATHMAX + 16];
	char interp[256];
	char mapped[PATHMAX];
};

static void fix_env(struct plan *pl, char *const *envp)
{
	const char *saved = env_get(envp, SAVED);
	size_t k = 0;

	for (size_t i = 0; envp && envp[i] && k + 2 < MAXARG; i++) {
		if (starts(envp[i], "LD_PRELOAD=") ||
		    starts(envp[i], "LD_LIBRARY_PATH="))
			continue;
		pl->envp[k++] = envp[i];
	}
	if (saved && *saved && strlen(saved) < PATHMAX) {
		memcpy(pl->preload, "LD_PRELOAD=", 11);
		memcpy(pl->preload + 11, saved, strlen(saved) + 1);
		pl->envp[k++] = pl->preload;
	}
	pl->envp[k] = 0;
}

static const char *map_interp(struct plan *pl, const char *interp,
			      char *const *envp)
{
	const char *prefix = env_get(envp, "PREFIX=");
	const char *rest = 0;

	if (starts(interp, "/usr/bin/"))
		rest = interp + 9;
	else if (starts(interp, "/bin/"))
		rest = interp + 5;
	if (!rest || !prefix || strlen(prefix) + strlen(rest) + 6 > PATHMAX)
		return interp;
	size_t n = strlen(prefix);
	memcpy(pl->mapped, prefix, n);
	memcpy(pl->mapped + n, "/bin/", 5);
	memcpy(pl->mapped + n + 5, rest, strlen(rest) + 1);
	return pl->mapped;
}

static int read_shebang(struct plan *pl, const char *path, char **arg)
{
	char buf[256];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t r = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (r < 3 || buf[0] != '#' || buf[1] != '!')
		return 0;
	buf[r] = 0;
	char *nl = strchr(buf, '\n');
	if (nl)
		*nl = 0;
	char *s = buf + 2;
	while (*s == ' ' || *s == '\t')
		s++;
	char *e = s;
	while (*e && *e != ' ' && *e != '\t')
		e++;
	char *a = 0;
	if (*e) {
		*e++ = 0;
		while (*e == ' ' || *e == '\t')
			e++;
		if (*e) {
			a = e;
			char *t = a + strlen(a);
			while (t > a && (t[-1] == ' ' || t[-1] == '\t' ||
					 t[-1] == '\r'))
				*--t = 0;
		}
	}
	if (!*s)
		return 0;
	size_t n = strlen(s);
	if (n >= sizeof pl->interp)
		return 0;
	memcpy(pl->interp, s, n + 1);
	if (a) {
		size_t m = strlen(a);
		if (n + 1 + m + 1 > sizeof pl->interp)
			return 0;
		memcpy(pl->interp + n + 1, a, m + 1);
		*arg = pl->interp + n + 1;
	} else {
		*arg = 0;
	}
	return 1;
}

static int build(struct plan *pl, const char *path, char *const *argv,
		 char *const *envp)
{
	size_t argc = count(argv);
	char *sarg = 0;
	const char *prog = path;
	size_t k = 0;

	fix_env(pl, envp);
	if (system_path(path)) {
		if (argc + 1 > MAXARG)
			return -1;
		for (size_t i = 0; i <= argc; i++)
			pl->argv[i] = argv[i];
		pl->path = path;
		return 0;
	}
	if (read_shebang(pl, path, &sarg))
		prog = map_interp(pl, pl->interp, envp);
	if (argc + 5 > MAXARG)
		return -1;

	if (system_path(prog)) {
		pl->path = prog;
		pl->argv[k++] = (char *)prog;
	} else {
		pl->path = LINKER;
		pl->argv[k++] = LINKER;
		pl->argv[k++] = (char *)prog;
	}
	if (prog != path) {
		if (sarg)
			pl->argv[k++] = sarg;
		pl->argv[k++] = (char *)path;
	}
	for (size_t i = 1; i < argc; i++)
		pl->argv[k++] = argv[i];
	pl->argv[k] = 0;
	return 0;
}

static const char *search(char *buf, const char *file, char *const *envp)
{
	if (strchr(file, '/'))
		return file;
	const char *p = env_get(envp, "PATH=");
	if (!p)
		p = "/system/bin";
	size_t fl = strlen(file);
	while (*p) {
		const char *c = strchr(p, ':');
		size_t dl = c ? (size_t)(c - p) : strlen(p);
		if (dl && dl + 1 + fl + 1 <= PATHMAX) {
			memcpy(buf, p, dl);
			buf[dl] = '/';
			memcpy(buf + dl + 1, file, fl + 1);
			if (!access(buf, X_OK))
				return buf;
		}
		if (!c)
			break;
		p = c + 1;
	}
	return file;
}

static execve_fn real_execve(void)
{
	return (execve_fn)dlsym(RTLD_NEXT, "execve");
}

int execve(const char *path, char *const argv[], char *const envp[])
{
	struct plan pl;
	if (build(&pl, path, argv, envp))
		return real_execve()(path, argv, envp);
	return real_execve()(pl.path, pl.argv, pl.envp);
}

int execv(const char *path, char *const argv[])
{
	return execve(path, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
	char buf[PATHMAX];
	return execve(search(buf, file, envp), argv, envp);
}

int execvp(const char *file, char *const argv[])
{
	return execvpe(file, argv, environ);
}

int posix_spawn(pid_t *pid, const char *path, const void *fa, const void *attr,
		char *const argv[], char *const envp[])
{
	spawn_fn real = (spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
	struct plan pl;
	if (build(&pl, path, argv, envp))
		return real(pid, path, fa, attr, argv, envp);
	return real(pid, pl.path, fa, attr, pl.argv, pl.envp);
}

int posix_spawnp(pid_t *pid, const char *file, const void *fa,
		 const void *attr, char *const argv[], char *const envp[])
{
	char buf[PATHMAX];
	return posix_spawn(pid, search(buf, file, envp), fa, attr, argv, envp);
}
