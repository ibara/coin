/*
 * Copyright (c) 2020 Brian Callahan <bcallah@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * coin -- command interpreter
 */

#define NULL	(void *) 0

extern void *_syscall(void *n, void *a, void *b, void *c, void *d, void *e);

static char *path[9] = {
	"/bin",
	"/sbin",
	"/usr/bin",
	"/usr/sbin",
	"/usr/X11R6/bin",
	"/usr/local/bin",
	"/usr/local/sbin",
	"/usr/games",
	NULL
};

static char *envp[4] = {
	"HOME=" HOME,
	"PATH=" PATH,
	"TERM=" TERM,
	NULL
};

static char *com[2048], *previous[2048];

static char cwd[1024], oldcwd[1024];

static int caught_sigint, loop, ret;

union sigval {
	int sival_int;
	void *sival_ptr;
};

typedef struct {
	int si_signo;
	int si_code;
	int si_errno;
	union {
		int _pad[29];
		struct {
			int _pid;
			union {
				struct {
					unsigned int _uid;
					union sigval _value;
				} _kill;
				struct {
					long long _utime;
					long long _stime;
					int _status;
				} _cld;
			} _pdata;
		} _proc;
		struct {
			void *_addr;
			int _trapno;
		} _fault;
	} _data;
} siginfo_t;

struct sigaction {
	union {
		void (*__sa_handler)(int);
		void (*__sa_sigaction)(int, siginfo_t *, void *);
	} __sigaction_u;
	unsigned int sa_mask;
	int sa_flags;
};

static void
_exit(int status)
{

	_syscall((void *) 1, (void *) status, NULL, NULL, NULL, NULL);
}

static long
read(int d, void *buf, unsigned long nbytes)
{

	return (long) _syscall((void *) 3, (void *) d, (void *) buf, (void *) nbytes, NULL, NULL);
}

static void
write(int d, const void *buf, unsigned long nbytes)
{

	_syscall((void *) 4, (void *) d, (void *) buf, (void *) nbytes, NULL, NULL);
}

static long
wait4(int wpid, int *status, int options, void *rusage)
{

	return (long) _syscall((void *) 11, (void *) wpid, (void *) status, (void *) options, (void *) rusage, NULL);
}

static int
chdir(const char *path)
{

	return (int) _syscall((void *) 12, (void *) path, NULL, NULL, NULL, NULL);
}

static int
access(const char *path, int amode)
{

	return (int) _syscall((void *) 33, (void *) path, (void *) amode, NULL, NULL, NULL);
}

static void
sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{

	_syscall((void *) 46, (void *) sig, (void *) act, (void *) oact, NULL, NULL);
}

static int
execve(const char *path, char *const argv[], char *const envp[])
{

	return (int) _syscall((void *) 59, (void *) path, (void *) argv, (void *) envp, NULL, NULL);
}

static long
vfork(void)
{

	return (long) _syscall((void *) 66, NULL, NULL, NULL, NULL, NULL);
}

static void
__getcwd(char *buf, unsigned long size)
{

	_syscall((void *) 304, (void *) buf, (void *) size, NULL, NULL, NULL);
}

static int
strcmp(const char *s1, const char *s2)
{

	while (*s1 == *s2++) {
		if (*s1++ == '\0')
			return 0;
	}

	return 1;
}

static unsigned long
strlen(const char *s)
{
	char *t;

	t = (char *) s;
	while (*t != '\0')
		t++;

	return t - s;
}

static void
set_handler(void (*cb)(int))
{
	struct sigaction action;

	action.sa_mask = 0;
	action.sa_flags = 0;
	action.__sigaction_u.__sa_handler = cb;

	sigaction(2, &action, NULL);
}

static void
catch_sigint(int signo)
{

	caught_sigint = 1;
}

static void
create_full_path(void)
{
	static char buf[4096];
	int i, j, k;

	if (access(com[0], 0x01) == 0)
		return;

	for (i = 0; path[i] != NULL; i++) {
		k = 0;
		for (j = 0; j < strlen(path[i]); j++)
			buf[j] = path[i][j];
		buf[j++] = '/';
		for (; j < sizeof(buf) - j - 1; j++)
			buf[j] = com[0][k++];
		buf[j] = '\0';

		if (access(buf, 0x01) == 0) {
			com[0] = buf;
			break;
		}
	}
}

static void
dputi(int n, int fd)
{
	char num[4];
	int i;

	i = 0;
	do {
		num[i++] = n %10 + '0';
	} while ((n /= 10) > 0);

	for (i--; i >= 0; i--)
		write(fd, &num[i], 1);
}

static void
dputs(const char *s, int fd)
{

	write(fd, s, strlen(s));
}

static int
dgets(char *s, int size, int fd)
{
	int i;

	for (i = 0; i < size - 1; i++) {
		if (read(fd, &s[i], 1) < 1)
			return 0;

		if (caught_sigint) {
			dputs("\n", 1);
			caught_sigint = 0;
			return 1;
		}

		if (s[i] == '\n') {
			++i;
			break;
		}
	}
	s[i] = '\0';

	return strlen(s);
}

static int
execute(void)
{
	int pid, ppid, status;

	switch ((ppid = vfork())) {
	case -1:
		_exit(1);
	case 0:
		_exit(execve(com[0], com, envp));
	}

	do {
		pid = wait4(ppid, &status, 0, NULL);
	} while (pid == -1);

	for (pid = 0; com[pid] != NULL; pid++)
		previous[pid] = com[pid];
	previous[pid] = NULL;

	return (int)(((unsigned)(status) >> 8) & 0xff);
}

static void
tokenify(char *s, int len)
{
	int c, i;

	for (i = 0; i < len; i++) {
		if (s[i] == ' ')
			s[i] = '\0';
	}

	c = 0;
	for (i = 0; i < len; i++) {
		if (s[i] == '\0')
			continue;

		com[c++] = &s[i];
		while (s[i] != '\0')
			++i;
	}
	com[c] = NULL;
}

static int
builtin(void)
{
	char buf[1024];
	int i;

	if (!strcmp(com[0], "exit")) {
		loop = 0;

		return 1;
	} else if (!strcmp(com[0], "cd")) {
cd:
		if (com[1] == NULL || (!strcmp(com[1], "~") && com[2] == NULL)) {
			__getcwd(oldcwd, sizeof(oldcwd));
			com[1] = HOME;
		} else if (!strcmp(com[1], "-") && com[2] == NULL) {
			__getcwd(buf, sizeof(buf));
			com[1] = oldcwd;
			i = -1;
		} else {
			__getcwd(oldcwd, sizeof(oldcwd));
		}
		if (chdir(com[1]) != 0) {
			dputs("coin: cd: ", 1);
			dputs(com[1], 1);
			dputs(" - could not change directory\n", 1);
			ret = 1;

			return 1;
		}

		__getcwd(cwd, sizeof(cwd));

		previous[0] = "cd";
		if (i == -1) {
			for (i = 0; i < strlen(buf); i++)
				oldcwd[i] = buf[i];
			if (i == sizeof(oldcwd))
				i = sizeof(oldcwd) - 1;
			oldcwd[i] = '\0';
			previous[1] = "-";
		} else {
			previous[1] = com[1];
		}
		previous[2] = NULL;

		ret = 0;

		return 1;
	} else if (!strcmp(com[0], "echo")) {
echo:
		if (!strcmp(com[1], "$?") && com[2] == NULL) {
			dputi(ret, 1);
			dputs("\n", 1);

			previous[0] = "echo";
			previous[1] = "$?";
			previous[2] = NULL;

			ret = 0;

			return 1;
		}
	} else if (!strcmp(com[0], "!!")) {
		if (previous[0] == NULL)
			return 1;
		for (i = 0; previous[i] != NULL; i++)
			com[i] = previous[i];
		com[i] = NULL;
		for (i = 0; com[i] != NULL; i++) {
			dputs(com[i], 1);
			if (com[i + 1] != NULL)
				dputs(" ", 1);
		}
		dputs("\n", 1);

		if (!strcmp(com[0], "cd"))
			goto cd;
		else if (!strcmp(com[0], "echo"))
			goto echo;
	}

	return 0;
}

static void
coin(void)
{
	char buf[4096];
	int i;

	dputs(cwd, 1);
	dputs("> ", 1);
	if ((i = dgets(buf, sizeof(buf), 0)) == 0) {
		loop = 0;
		return;
	}
	buf[--i] = '\0';

	if (i == 0)
		return;

	tokenify(buf, i);

	if (builtin() == 1)
		return;

	create_full_path();

	ret = execute();
}

int
main(int argc, char *argv[])
{
	int i;

	(void) argc, (void) argv;

	set_handler(catch_sigint);

	__getcwd(oldcwd, sizeof(oldcwd));
	__getcwd(cwd, sizeof(cwd));
	chdir(cwd);

	loop = 1;
	while (loop)
		coin();

	return 0;
}
