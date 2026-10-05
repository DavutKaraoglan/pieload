#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: seccomp-probe <syscall-nr>...\n");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		long nr = strtol(argv[i], NULL, 0);
		fflush(NULL);

		pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			long r = syscall(nr, -1, 0, 0, 0, 0, 0);
			_exit(r < 0 && errno == ENOSYS ? 2 : 0);
		}

		int st;
		waitpid(pid, &st, 0);
		if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS)
			printf("%ld blocked by seccomp (SIGSYS)\n", nr);
		else if (WIFEXITED(st) && WEXITSTATUS(st) == 2)
			printf("%ld unknown to kernel (ENOSYS)\n", nr);
		else
			printf("%ld reaches the kernel\n", nr);
	}
	return 0;
}
