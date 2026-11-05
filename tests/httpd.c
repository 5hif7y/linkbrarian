/* Minimal fixture HTTP server for linkbrarian tests. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_REQ 8192
#define MAX_PATH 4096

static void send_all(int fd, const char *data, size_t len)
{
	while (len) {
		ssize_t n = send(fd, data, len, 0);

		if (n <= 0)
			return;
		data += n;
		len -= (size_t)n;
	}
}

static const char *mime_type(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (dot && !strcmp(dot, ".html"))
		return "text/html; charset=utf-8";
	if (dot && !strcmp(dot, ".txt"))
		return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

static void serve_file(int fd, const char *root, const char *path)
{
	char file[MAX_PATH];
	struct stat st;
	int in;
	char header[512];
	char buf[8192];
	ssize_t n;

	if (strstr(path, "..")) {
		send_all(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", strlen("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"));
		return;
	}
	if (snprintf(file, sizeof(file), "%s%s", root, path) >= (int)sizeof(file)) {
		send_all(fd, "HTTP/1.1 414 URI Too Long\r\nConnection: close\r\n\r\n", strlen("HTTP/1.1 414 URI Too Long\r\nConnection: close\r\n\r\n"));
		return;
	}
	if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
		{
			const char *msg =
				"HTTP/1.1 404 Not Found\r\n"
				"Content-Type: text/plain\r\n"
				"Content-Length: 10\r\n"
				"Connection: close\r\n\r\n"
				"not found\n";

			send_all(fd, msg, strlen(msg));
		}
		return;
	}
	in = open(file, O_RDONLY);
	if (in < 0) {
		send_all(fd, "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n", strlen("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n"));
		return;
	}
	snprintf(header, sizeof(header),
		 "HTTP/1.1 200 OK\r\n"
		 "Content-Type: %s\r\n"
		 "Content-Length: %lld\r\n"
		 "Connection: close\r\n\r\n",
		 mime_type(file), (long long)st.st_size);
	send_all(fd, header, strlen(header));
	while ((n = read(in, buf, sizeof(buf))) > 0)
		send_all(fd, buf, (size_t)n);
	close(in);
}

static void handle(int fd, const char *root)
{
	char req[MAX_REQ + 1];
	size_t len = 0;
	char method[16], path[MAX_PATH];
	ssize_t n;

	while (len < MAX_REQ) {
		n = read(fd, req + len, MAX_REQ - len);
		if (n <= 0)
			break;
		len += (size_t)n;
		req[len] = '\0';
		if (strstr(req, "\r\n\r\n"))
			break;
	}
	req[len] = '\0';
	if (sscanf(req, "%15s %4095s", method, path) != 2 ||
	    strcmp(method, "GET") != 0) {
		send_all(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", strlen("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"));
		return;
	}
	{
		char *q = strpbrk(path, "?#");

		if (q)
			*q = '\0';
	}
	serve_file(fd, root, path);
}

int main(int argc, char **argv)
{
	int port = 0, srv, one = 1;
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);
	const char *root;

	if (argc != 3) {
		fprintf(stderr, "usage: httpd PORT FIXTURE_DIR\n");
		return 2;
	}
	port = atoi(argv[1]);
	root = argv[2];

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((unsigned short)port);
	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		return 1;
	}
	if (listen(srv, 32) != 0) {
		perror("listen");
		return 1;
	}
	if (getsockname(srv, (struct sockaddr *)&addr, &alen) != 0) {
		perror("getsockname");
		return 1;
	}
	printf("%d\n", (int)ntohs(addr.sin_port));
	fflush(stdout);

	for (;;) {
		int fd = accept(srv, NULL, NULL);
		pid_t pid;

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			return 1;
		}
		pid = fork();
		if (pid == 0) {
			close(srv);
			handle(fd, root);
			close(fd);
			_exit(0);
		}
		close(fd);
	}
}
