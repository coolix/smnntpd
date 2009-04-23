#include <netinet/in.h>   // sockaddr, sockaddr_in
#include <arpa/inet.h>    // hton, ntoh
#include <string.h>       // memset
#include <sys/socket.h>   // socket
#include <sys/types.h>    // compatibilité BSD
#include <stdio.h>        // perror, fopen, fileno
#include <stdlib.h>       // exit
#include <unistd.h>       // close
#include <fcntl.h>        // fcntl
#include <sys/wait.h>     // waitpid
#include <netdb.h>        // getaddrinfo
#include <errno.h>
#include <confuse.h>

#define MAX_CLIENT_PENDING 5
#define NNTP_BUF 512

void
die(const char *msg)
{
    perror(msg);
    exit(1);
}

/* Reap zombies */
void
sigchld_handler(int s)
{
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* Transfer data between a server and a client */
int
proxy_transfert(const char who, FILE *src, FILE *dst)
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

inline int
max_fd(int a, int b)
{
    return (a > b ? a : b);
}

FILE *
server_connect(const char *addr, const char *port)
{
    struct addrinfo hints, *res;
    int status;

    struct sockaddr *server;
    int serverfd;
    FILE *server_stream;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ((status = getaddrinfo(
                   addr, port, &hints, &res )
        ) != 0)
    {   
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

int
set_credential(FILE *server, const char *user, const char *pass)
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
    fputs(id,server);
    free(id);

    fgets(msg, NNTP_BUF, server);
    return strncmp("281", msg, 3);
}

int 
main(void)
{
    struct sockaddr_in proxy;
    struct sigaction   sa; 

    cfg_opt_t opts[] = { 
        CFG_STR("proxy_addr", "INADDR_ANY", CFGF_NONE),
        CFG_INT("proxy_port", 119, CFGF_NONE),
        CFG_STR("server_addr", NULL, CFGF_NONE),
        CFG_STR("server_port", NULL, CFGF_NONE),
        CFG_STR("username", NULL, CFGF_NONE),
        CFG_STR("password", NULL, CFGF_NONE),
        CFG_BOOL("auth", cfg_false, CFGF_NONE)
    };
    cfg_t *cfg;

    cfg = cfg_init(opts, CFGF_NONE);
    if (cfg_parse(cfg, "smnntpd.conf") != CFG_SUCCESS) {
        printf("file couldn't be found!\n");
        exit(1);
    }
     

    proxy.sin_family = AF_INET;
    proxy.sin_port = htons(cfg_getint(cfg, "proxy_port"));
    proxy.sin_addr.s_addr = inet_addr(
                                cfg_getstr(cfg, "proxy_addr"));
    memset(proxy.sin_zero,'\0', sizeof proxy.sin_zero);
    
    int proxyfd = socket(PF_INET, SOCK_STREAM, 0);   
    if (proxyfd == -1)
        die("socket");

    // regain port socket if it has been left unbinded
    int yes = 1;
    setsockopt(proxyfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    if (bind(proxyfd, (struct sockaddr *)&proxy, sizeof proxy) == -1)
        die("bind");
    
    if (listen(proxyfd, MAX_CLIENT_PENDING) == -1)
        die("listen");

    struct sockaddr client;
    socklen_t client_len = sizeof client;
    int clientfd;

    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags=SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
        die("sigaction");

    for(;;)
    {
        if ((clientfd = accept(proxyfd, &client, &client_len)) == -1)
            die("accept");

        // Here comes a new challenger, let's fork() about sex
        if (!fork())
        {
            FILE *server_stream, *client_stream;
            int  serverfd;
            char srv_msg[NNTP_BUF];
            struct timeval timeout = {0, 1000};

            close(proxyfd);
            server_stream = server_connect(
                             cfg_getstr(cfg, "server_addr"),
                             cfg_getstr(cfg, "server_port")
                            );
            serverfd      = fileno(server_stream);

            if ((client_stream = fdopen(clientfd, "r+")) == NULL)
                die("fdopen");
            
            setlinebuf(server_stream);
            setlinebuf(client_stream);

            if (cfg_getbool(cfg, "auth"))
            {
                fgets(srv_msg, NNTP_BUF, server_stream);
                if (set_credential(server_stream,
                                   cfg_getstr(cfg, "username"),
                                   cfg_getstr(cfg, "password")
                                  )
                   )
                {
                    fprintf(stderr, "couldn't set creds\n");
                    exit(EXIT_FAILURE);
                }
                fputs(srv_msg, client_stream);
            }

            setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof timeout);
            setsockopt(serverfd, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof timeout);

            fd_set readfds, readfds_m;
            FD_ZERO(&readfds);
            FD_SET(clientfd, &readfds);
            FD_SET(serverfd, &readfds);

            for (;;)
            {
                readfds_m = readfds;
                select(max_fd(serverfd, clientfd)+1, &readfds_m, NULL, NULL, NULL);
                if (FD_ISSET(serverfd, &readfds_m))
                    if (proxy_transfert('S', server_stream, client_stream))
                        break;
                if (FD_ISSET(clientfd, &readfds_m))
                {
                    if (proxy_transfert('C', client_stream, server_stream))
                        break;
                }
            }
            fclose(server_stream);
            fclose(client_stream);
            exit(0);
        }
        else
        {
            close(clientfd);
        }
    }
    exit(EXIT_SUCCESS);
}
