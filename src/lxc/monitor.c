/* SPDX-License-Identifier: LGPL-2.1+ */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "af_unix.h"
#include "error.h"
#include "log.h"
#include "lxclock.h"
#include "macro.h"
#include "memory_utils.h"
#include "monitor.h"
#include "state.h"
#include "utils.h"

#if !HAVE_STRLCPY
#include "strlcpy.h"
#endif

lxc_log_define(monitor, lxc);

/* routines used by monitor publishers (containers) */
int lxc_monitor_fifo_name(const char *lxcpath, char *fifo_path, size_t fifo_path_sz,
			  int do_mkdirp)
{
	int ret;
	char *rundir;

	rundir = get_rundir();
	if (!rundir)
		return -1;

	if (do_mkdirp) {
		ret = strnprintf(fifo_path, fifo_path_sz, "%s/lxc/%s", rundir, lxcpath);
		if (ret < 0) {
			ERROR("rundir/lxcpath (%s/%s) too long for monitor fifo", rundir, lxcpath);
			free(rundir);
			return -1;
		}
		ret = lxc_mkdir_p(fifo_path, 0755);
		if (ret < 0) {
			ERROR("Unable to create monitor fifo directory %s", fifo_path);
			free(rundir);
			return ret;
		}
	}
	ret = strnprintf(fifo_path, fifo_path_sz, "%s/lxc/%s/monitor-fifo", rundir, lxcpath);
	if (ret < 0) {
		ERROR("rundir/lxcpath (%s/%s) too long for monitor fifo", rundir, lxcpath);
		free(rundir);
		return -1;
	}
	free(rundir);
	return 0;
}

static void lxc_monitor_fifo_send(struct lxc_msg *msg, const char *lxcpath)
{
	int fd,ret;
	char fifo_path[PATH_MAX];

	BUILD_BUG_ON(sizeof(*msg) > PIPE_BUF); /* write not guaranteed atomic */

	ret = lxc_monitor_fifo_name(lxcpath, fifo_path, sizeof(fifo_path), 0);
	if (ret < 0)
		return;

	/* Open the fifo nonblock in case the monitor is dead, we don't want the
	 * open to wait for a reader since it may never come.
	 */
	fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		/* It is normal for this open() to fail with ENXIO when there is
		 * no monitor running, so we don't log it.
		 */
		if (errno == ENXIO || errno == ENOENT)
			return;

		SYSWARN("Failed to open fifo to send message");
		return;
	}

	if (fcntl(fd, F_SETFL, O_WRONLY) < 0) {
		close(fd);
		return;
	}

	ret = lxc_write_nointr(fd, msg, sizeof(*msg));
	if (ret != sizeof(*msg)) {
		close(fd);
		SYSERROR("Failed to write to monitor fifo \"%s\"", fifo_path);
		return;
	}

	close(fd);
}

void lxc_monitor_send_state(const char *name, lxc_state_t state,
			    const char *lxcpath)
{
	struct lxc_msg msg = {.type = lxc_msg_state, .value = state};

	(void)strlcpy(msg.name, name, sizeof(msg.name));
	lxc_monitor_fifo_send(&msg, lxcpath);
}

void lxc_monitor_send_exit_code(const char *name, int exit_code,
				const char *lxcpath)
{
	struct lxc_msg msg = {.type = lxc_msg_exit_code, .value = exit_code};

	(void)strlcpy(msg.name, name, sizeof(msg.name));
	lxc_monitor_fifo_send(&msg, lxcpath);
}

/* routines used by monitor subscribers (lxc-monitor) */
int lxc_monitor_close(int fd)
{
	return close(fd);
}

/* Enforces \0-termination for the abstract unix socket. This is not required
 * but allows us to print it out.
 *
 * Older version of liblxc only allowed for 105 bytes to be used for the
 * abstract unix domain socket name because the code for our abstract unix
 * socket handling performed invalid checks. Since we \0-terminate we could now
 * have a maximum of 106 chars. But to not break backwards compatibility we keep
 * the limit at 105.
 */
int lxc_monitor_sock_name(const char *lxcpath, struct sockaddr_un *addr)
{
	__do_free char *path = NULL;
	size_t len;
	int ret;
	uint64_t hash;

	/* addr.sun_path is only 108 bytes, so we hash the full name and
	 * then append as much of the name as we can fit.
	 */
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;

	/* strlen("lxc/") + strlen("/monitor-sock") + 1 = 18 */
	len = strlen(lxcpath) + 18;
	path = must_realloc(NULL, len);
	ret = strnprintf(path, len, "lxc/%s/monitor-sock", lxcpath);
	if (ret < 0) {
		ERROR("Failed to create name for monitor socket");
		return -1;
	}

	/* Note: strnprintf() will \0-terminate addr->sun_path on the 106th byte
	 * and so the abstract socket name has 105 "meaningful" characters. This
	 * is absolutely intentional. For further info read the comment for this
	 * function above!
	 */
	len = sizeof(addr->sun_path) - 1;
	hash = fnv_64a_buf(path, ret, FNV1A_64_INIT);
	ret = strnprintf(addr->sun_path, len, "@lxc/%016" PRIx64 "/%s", hash, lxcpath);
	if (ret < 0) {
		ERROR("Failed to create hashed name for monitor socket");
		goto on_error;
	}

	/* replace @ with \0 */
	addr->sun_path[0] = '\0';
	INFO("Using monitor socket name \"%s\" (length of socket name %zu must be <= %zu)", &addr->sun_path[1], strlen(&addr->sun_path[1]), sizeof(addr->sun_path) - 3);

	return 0;

on_error:
	return -1;
}

int lxc_monitor_open(const char *lxcpath)
{
	struct sockaddr_un addr;
	int fd;
	size_t retry;
	int backoff_ms[] = {10, 50, 100};

	if (lxc_monitor_sock_name(lxcpath, &addr) < 0)
		return -1;

	DEBUG("Opening monitor socket %s with len %zu", &addr.sun_path[1], strlen(&addr.sun_path[1]));

	for (retry = 0; retry < sizeof(backoff_ms) / sizeof(backoff_ms[0]); retry++) {
		fd = lxc_abstract_unix_connect(addr.sun_path);
		if (fd != -1 || errno != ECONNREFUSED)
			break;

		SYSERROR("Failed to connect to monitor socket. Retrying in %d ms", backoff_ms[retry]);
		usleep(backoff_ms[retry] * 1000);
	}

	if (fd < 0) {
		SYSERROR("Failed to connect to monitor socket");
		return -1;
	}

	return fd;
}

int lxc_monitor_read_fdset(struct pollfd *fds, nfds_t nfds, struct lxc_msg *msg,
			   int timeout)
{
	int ret;

	ret = poll(fds, nfds, timeout * 1000);
	if (ret == -1)
		return -1;
	else if (ret == 0)
		return -2;  /* timed out */

	/* Only read from the first ready fd, the others will remain ready for
	 * when this routine is called again.
	 */
	for (size_t i = 0; i < nfds; i++) {
		if (fds[i].revents != 0) {
			fds[i].revents = 0;
			ret = recv(fds[i].fd, msg, sizeof(*msg), 0);
			if (ret <= 0) {
				SYSERROR("Failed to receive message. Did monitord die?");
				return -1;
			}
			return ret;
		}
	}

	SYSERROR("No ready fd found");

	return -1;
}

int lxc_monitor_read_timeout(int fd, struct lxc_msg *msg, int timeout)
{
	struct pollfd fds;

	fds.fd = fd;
	fds.events = POLLIN | POLLPRI;
	fds.revents = 0;

	return lxc_monitor_read_fdset(&fds, 1, msg, timeout);
}

int lxc_monitor_read(int fd, struct lxc_msg *msg)
{
	return lxc_monitor_read_timeout(fd, msg, -1);
}

#define LXC_MONITORD_PATH LIBEXECDIR "/lxc/lxc-monitord"

/* Used to spawn a monitord either on startup of a daemon container, or when
 * lxc-monitor starts.
 */
int lxc_monitord_spawn(const char *lxcpath)
{
	int ret;
	int pipefd[2];
	char pipefd_str[INTTYPE_TO_STRLEN(int)];
	pid_t pid1, pid2;

	char *const args[] = {
		LXC_MONITORD_PATH,
		(char *)lxcpath,
		pipefd_str,
		NULL,
	};

	/* double fork to avoid zombies when monitord exits */
	pid1 = fork();
	if (pid1 < 0) {
		SYSERROR("Failed to fork()");
		return -1;
	}

	if (pid1) {
		DEBUG("Going to wait for pid %d", pid1);

		if (waitpid(pid1, NULL, 0) != pid1)
			return -1;

		DEBUG("Finished waiting on pid %d", pid1);
		return 0;
	}

	if (pipe(pipefd) < 0) {
		SYSERROR("Failed to create pipe");
		_exit(EXIT_FAILURE);
	}

	pid2 = fork();
	if (pid2 < 0) {
		SYSERROR("Failed to fork()");
		_exit(EXIT_FAILURE);
	}

	if (pid2) {
		DEBUG("Trying to sync with child process");
		char c;
		/* Wait for daemon to create socket. */
		close(pipefd[1]);

		/* Sync with child, we're ignoring the return from read
		 * because regardless if it works or not, either way we've
		 * synced with the child process. the if-empty-statement
		 * construct is to quiet the warn-unused-result warning.
		 */
		if (lxc_read_nointr(pipefd[0], &c, 1)) {
			;
		}

		close(pipefd[0]);

		DEBUG("Successfully synced with child process");
		_exit(EXIT_SUCCESS);
	}

	if (setsid() < 0) {
		SYSERROR("Failed to setsid()");
		_exit(EXIT_FAILURE);
	}

	lxc_check_inherited(NULL, true, &pipefd[1], 1);
	if (null_stdfds() < 0) {
		SYSERROR("Failed to dup2() standard file descriptors to /dev/null");
		_exit(EXIT_FAILURE);
	}

	close(pipefd[0]);

	ret = strnprintf(pipefd_str, sizeof(pipefd_str), "%d", pipefd[1]);
	if (ret < 0) {
		ERROR("Failed to create pid argument to pass to monitord");
		_exit(EXIT_FAILURE);
	}

	DEBUG("Using pipe file descriptor %d for monitord", pipefd[1]);

	execvp(args[0], args);
	SYSERROR("Failed to exec lxc-monitord");

	_exit(EXIT_FAILURE);
}
