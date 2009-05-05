#include <netinet/in.h>		/* sockaddr, sockaddr_in */
#include <arpa/inet.h>		/* hton, ntoh*/
#include <string.h>		/* memset*/
#include <sys/socket.h>		/* socket*/
#include <sys/types.h>		/* compatibilité BSD*/
#include <sys/stat.h>
#include <stdio.h>		/* perror, fopen, fileno*/
#include <stdlib.h>		/* exit*/
#include <unistd.h>		/* close*/
#include <fcntl.h>		/* fcntl*/
#include <sys/wait.h>		/* waitpid*/
#include <netdb.h>		/* getaddrinfo*/
#include <errno.h>
#include <confuse.h>
#include <sys/select.h>
#include <syslog.h>

#define DAEMON_NAME	"smNNTPd"
#define LISTENQ		32
#define NNTP_BUF	512
#define max(a, b)	((a) > (b) ? (a) : (b))
#define PROXY_ADDR	cfg_getstr(cfg, "proxy_addr")
#define PROXY_PORT	atoi(cfg_getstr(cfg, "proxy_port"))
#define SERVER_ADDR	cfg_getstr(cfg, "server_addr")
#define SERVER_PORT	cfg_getstr(cfg, "server_port")
#define USERNAME	cfg_getstr(cfg, "username")
#define PASSWORD	cfg_getstr(cfg, "password")
#define AUTH		cfg_getbool(cfg, "auth")

void die(const char *msg)
{
	perror(msg);
	exit(1);
}

/* Reap zombies */
void sigchld_handler(int s)
{
	while (waitpid(-1, NULL, WNOHANG) > 0) ;
}

/* Transfer data between a server and a client */
int proxy_transfert(const char who, FILE * src, FILE * dst)
{
	char buffer[NNTP_BUF];
	char *res;

	res = fgets(buffer, sizeof buffer, src);
	if (res == NULL)
		return -1;

	do {
		fputs(buffer, dst);
	} while (fgets(buffer, sizeof buffer, src) != NULL);
	return 0;
}

FILE *server_connect(const char *addr, const char *port)
{
	struct addrinfo hints, *res;
	struct sockaddr *server;
	FILE *server_stream;
	int status;
	int serverfd;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if ((status = getaddrinfo(addr, port, &hints, &res)) != 0) {
		gai_strerror(status);
		exit(1);
	}
	server = res->ai_addr;

	serverfd = socket(PF_INET, SOCK_STREAM, 0);
	if (serverfd == -1)
		die("socket");

	if (connect(serverfd, server, sizeof *server) == -1)
		die("connect");

	if ((server_stream = fdopen(serverfd, "r+")) == NULL)
		die("fdopen");

	freeaddrinfo(res);
	return server_stream;
}

int set_credential(FILE * server, const char *user, const char *pass)
{
	char msg[NNTP_BUF];
	char *id;

	id = (char *)calloc(strlen("AUTHINFO USER \r\n")
			    + strlen(user)
			    + 1, sizeof(char));
	sprintf(id, "AUTHINFO USER %s\r\n", user);
	fputs(id, server);
	free(id);

	fgets(msg, NNTP_BUF, server);

	id = (char *)calloc(strlen("AUTHINFO PASS \r\n")
			    + strlen(pass)
			    + 1, sizeof(char));
	sprintf(id, "AUTHINFO PASS %s\r\n", pass);
	fputs(id, server);
	free(id);

	fgets(msg, NNTP_BUF, server);
	return strncmp("281", msg, 3);
}

cfg_t *read_conf()
{
	cfg_t *cfg;
	cfg_opt_t opts[] = {
		CFG_STR("proxy_addr", "127.0.0.1", CFGF_NONE),
		CFG_STR("proxy_port", "119", CFGF_NONE),
		CFG_STR("server_addr", NULL, CFGF_NONE),
		CFG_STR("server_port", NULL, CFGF_NONE),
		CFG_STR("username", NULL, CFGF_NONE),
		CFG_STR("password", NULL, CFGF_NONE),
		CFG_BOOL("auth", cfg_false, CFGF_NONE),
		CFG_END()
	};

	cfg = cfg_init(opts, CFGF_NONE);
	if (cfg_parse(cfg, "smnntpd.conf") != CFG_SUCCESS) {
		printf("file couldn't be found!\n");
		exit(1);
	}
	return cfg;
}

int start_proxy(cfg_t *cfg)
{
	struct sockaddr_in proxy;
	int proxyfd;
	int yes = 1;

	proxy.sin_family = AF_INET;
	proxy.sin_port = htons(PROXY_PORT);
	proxy.sin_addr.s_addr = inet_addr(PROXY_ADDR);
	memset(proxy.sin_zero, '\0', sizeof proxy.sin_zero);

	proxyfd = socket(PF_INET, SOCK_STREAM, 0);
	if (proxyfd == -1)
		die("socket");

	/* regain port socket if it has been left unbinded */
	setsockopt(proxyfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

	if (bind(proxyfd, (struct sockaddr *)&proxy, sizeof proxy) == -1)
		die("bind");

	if (listen(proxyfd, LISTENQ) == -1)
		die("listen");

	return proxyfd;
}

static void daemonize()
{
	pid_t pid;

	if (getppid() == 1) return;
	
	pid = fork();
	
	if (pid < 0)
		die("fork");

	if (pid > 0)
		exit(EXIT_SUCCESS);
	
	umask(0222);

	if (setsid() < 0)
		die("setsid()");

	if (chdir("/") < 0)
		die("chdir");

	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);
}

int main(void)
{
	struct sockaddr client;
	struct sigaction sa;
	int proxyfd, clientfd;
	socklen_t clientlen;
	cfg_t *cfg = read_conf();


	proxyfd = start_proxy(cfg);
	clientlen = sizeof client;

	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;

	openlog(DAEMON_NAME, LOG_PID, LOG_LOCAL5);
	syslog(LOG_INFO, "starting");
	daemonize();

	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		die("sigaction");

	for (;;) {
		if ((clientfd = accept(proxyfd, &client, &clientlen)) == -1)
			die("accept");

		/* Here comes a new challenger, let's fork() about sex */
		if (fork()) {
			close(clientfd);
			continue;
		}

		/* Child */
		FILE *server_stream, *client_stream;
		int serverfd;
		char srv_msg[NNTP_BUF];
		struct timeval timeout = { 0, 1000 };
		fd_set readfds, readfds_m;

		close(proxyfd);
		server_stream = server_connect(SERVER_ADDR, SERVER_PORT);
		serverfd = fileno(server_stream);

		if ((client_stream = fdopen(clientfd, "r+")) == NULL)
			die("fdopen");

		setlinebuf(server_stream);
		setlinebuf(client_stream);

		if (AUTH) {
			fgets(srv_msg, NNTP_BUF, server_stream);
			if (set_credential(server_stream,
					   USERNAME,
					   PASSWORD
			    )
			    ) {
				fprintf(stderr, "couldn't set creds\n");
				exit(EXIT_FAILURE);
			}
			fputs(srv_msg, client_stream);
		}

		setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO,
			   &timeout, sizeof timeout);
		setsockopt(serverfd, SOL_SOCKET, SO_RCVTIMEO,
			   &timeout, sizeof timeout);

		FD_ZERO(&readfds);
		FD_SET(clientfd, &readfds);
		FD_SET(serverfd, &readfds);

		for (;;) {
			readfds_m = readfds;
			select(max(serverfd, clientfd) + 1,
			       &readfds_m, NULL, NULL, NULL);
			if (FD_ISSET(serverfd, &readfds_m))
				if (proxy_transfert
				    ('S', server_stream, client_stream))
					break;
			if (FD_ISSET(clientfd, &readfds_m)) {
				if (proxy_transfert
				    ('C', client_stream, server_stream))
					break;
			}
		}
		fclose(server_stream);
		fclose(client_stream);
		exit(0);
	}
	syslog(LOG_NOTICE, "terminated");
	exit(EXIT_SUCCESS);
}
