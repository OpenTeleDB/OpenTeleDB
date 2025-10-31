
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <odyssey.h>
#include <string.h>
#include <signal.h>

static char pid_file_path[PATH_MAX] = {0};

static int check_pid_file()
{
	int fd = open(pid_file_path, O_RDONLY);
	if (fd == -1)
		return -1;

	char buffer[128];
	ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
	if (bytes_read == -1) {
		close(fd);
		return -1;
	}
	else if (bytes_read == 0) {
		close(fd);
		return 0;
	}

	buffer[bytes_read] = '\0';
	pid_t pid = (pid_t)strtol(buffer, NULL, 10);
	close(fd);
	return pid == getpid() ? 0 : -1;
}

static void atexit_handler(void)
{
	if (pid_file_path[0] == '\0'){
		/* do nothing if pid file path is empty */
	}
	else if (check_pid_file() == 0) {
		if (od_pid_unlink(NULL, pid_file_path) == 0) {
			printf("PID file removed successfully.\n");
		} else {
			perror("Failed to remove PID file");
		}
	}
}

static void signal_handler(int signum)
{
	atexit_handler();
	/* raise SIGSEGV to produce CORE file */
	if (signum == SIGSEGV) {
		signal(SIGSEGV, SIG_DFL);
		raise(SIGSEGV);
	} else {
		exit(EXIT_SUCCESS);
	}
}

static void unlink_pid_file_atexit(char *path)
{
	strncpy(pid_file_path, path, PATH_MAX);
	signal(SIGSEGV, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGQUIT, signal_handler);
	atexit(atexit_handler);
}

void od_pid_init(od_pid_t *pid)
{
	pid->pid = getpid();
	pid->pid_len = od_snprintf(pid->pid_sz, sizeof(pid->pid_sz), "%d",
				   (int)pid->pid);
}

int od_pid_create(od_pid_t *pid, char *path)
{
	char buffer[32];
	int size = od_snprintf(buffer, sizeof(buffer), "%d\n", pid->pid);
	int rc;
	rc = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (rc == -1)
		return -1;
	int fd = rc;
	rc = write(fd, buffer, size);
	if (rc != size) {
		close(fd);
		return -1;
	}
	rc = close(fd);

	unlink_pid_file_atexit(path);

	return rc;
}

int od_pid_unlink(od_pid_t *pid, char *path)
{
	(void)pid;
	int rc = unlink(path);
	return rc;
}
