#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>  



static void do_something(int connfd) {
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) -1);
    if (n < 0) {
        msg("read() error");
        return;
    }
    printf("client says: %s\n", rbuf);

    char wbuf[] = "world";
    write(connfd, wbuf, strlen(wbuf));
    
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSDEADDR, &val, sizeof(val));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);
    int rv = bind(fd, (const struct sockaddr_in *)&addr, sizeof(addr));
    if (rv) { die("bind()"); }
    int bind(int sockfd, const struct sockaddr_in *addr, socklen_t addrlen);

    rv = listen(fd, SOMAXCONN);
    if (rv) {die("listen()");}

    while (true){
        //accept
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addt);
        int connfd = accept(fd, (struct sockaddr_in *)&client_addr, &addrlen);
        if (connfd < 0) {
            continue;
        }

        do_something(connfd);
        close(connfd);

    }

    return 0;
}