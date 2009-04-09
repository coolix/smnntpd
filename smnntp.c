#include <netinet/in.h> // sockaddr, sockaddr_in
#include <arpa/inet.h> // hton, ntoh
#include <string.h> // memset
#include <sys/socket.h> //socket
#include <sys/types.h> //compatibilité BSD
#include <stdio.h> //perror, fopen
#include <stdlib.h> //exit
#include <unistd.h> // close
#include <fcntl.h> // fcntl

#define PROXY_ADDRESS INADDR_ANY
#define PROXY_PORT    119
#define MAX_CLIENT_PENDING 5

#define SERVER_ADDRESS "212.27.60.40"
#define SERVER_PORT 119

void
die(const char *msg)
{
    perror(msg);
    exit(1);
}

int
proxy_transfert(char who, FILE *src, FILE *dst)
{
    char buffer[512];
    char *res;
    
    if ((res=fgets(buffer, sizeof buffer, 
        src)) == NULL)
        return -1;
    do
    {
        printf("[%c]: %s", who, buffer);
        fputs(buffer, dst);
        fflush(dst);
    }
    while ((res=fgets(buffer, sizeof buffer,
        src)) != NULL);
    return 0;
}

inline int
max_fd(int a, int b)
{
    return (a > b ? a : b);
}

int 
main(void)
{
    struct sockaddr_in proxy;

    proxy.sin_family = AF_INET;
    proxy.sin_port = htons(PROXY_PORT);
    proxy.sin_addr.s_addr = PROXY_ADDRESS;
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

    struct sockaddr remote;
    socklen_t remote_len = sizeof remote;
    int remotefd;

    for(;;)
    {
        if ((remotefd = accept(proxyfd, &remote, &remote_len)) == -1)
            die("accept");

        // Here comes a new challenger, let's fork() about sex
        if (!fork())
        {
            close(proxyfd);

            struct sockaddr_in server;
            server.sin_family = AF_INET;
            server.sin_port = htons(SERVER_PORT);
            server.sin_addr.s_addr = inet_addr(SERVER_ADDRESS);
            memset(server.sin_zero,'\0', sizeof server.sin_zero);

            int serverfd = socket(PF_INET, SOCK_STREAM, 0);
            if (serverfd == -1)
                die("socket");
    
            if (connect(serverfd, (struct sockaddr *)&server, sizeof server) == -1)
                die("connect");
            
            FILE *server_stream; 
            fcntl(serverfd, F_SETFL, O_NONBLOCK);
            if ((server_stream = fdopen(serverfd, "r+")) == NULL)
                die("fdopen");

            FILE *remote_stream;
            if (fcntl(remotefd, F_SETFL, O_NONBLOCK) < 0)
                die("fcntl");
            if ((remote_stream = fdopen(remotefd, "r+")) == NULL)
                die("fdopen");

            fd_set readfds, readfds_m;
            FD_ZERO(&readfds);
            FD_SET(remotefd, &readfds);
            FD_SET(serverfd, &readfds);

            for (;;)
            {
                readfds_m = readfds;
                select(max_fd(serverfd, remotefd)+1, &readfds_m, NULL, NULL, NULL);
                if (FD_ISSET(serverfd, &readfds_m))
                    if (proxy_transfert('S', server_stream, remote_stream))
                        break;
                if (FD_ISSET(remotefd, &readfds_m))
                {
                    if (proxy_transfert('C', remote_stream, server_stream))
                        break;
                }
            }
            fclose(server_stream);
            fclose(remote_stream);
            exit(0);
        }
        else
        {
            close(remotefd);
        }
    }
    exit(EXIT_SUCCESS);
}
