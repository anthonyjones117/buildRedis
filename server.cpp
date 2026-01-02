#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define read(fd, buf, n) recv(fd, buf, n, 0)
    #define write(fd, buf, n) send(fd, buf, n, 0)
    #define close closesocket
    typedef int socklen_t;
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netinet/ip.h>
#endif

const size_t k_max_msg = 4096;


struct Conn {
    int f = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len){
    buf.insert(buf.end(), data, data + len);
}


static void buf_consume(std::vector<uint8_t> &buf, size_t n){
    buf.erase(buf.begin(), buf.begin() + n);
}

// static void do_something(int connfd) {
//     char rbuf[64] = {};
//     ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
//     if (n < 0) {
//         msg("read() error");
//         return;
//     }
//     fprintf(stderr, "client says: %s\n", rbuf);

//     char wbuf[] = "world";
//     write(connfd, wbuf, strlen(wbuf));
// }

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0){
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0){
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all( int fd, const char *buf, size_t n){
    while (n>0){
        ssize_t rv = write(fd,buf,n);
        if (rv <= 0 ){
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t one_request(int connfd) {
    char rbuf[4 + k_max_msg];
    errno = 0;
    int32_t err = read_full(connfd, rbuf, 4);
    if (err) {
        msg(errno == 0 ? "EOF" : "read() error");
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }
    err = read_full(connfd, &rbuf[4], len);
    if (err) {
        msg("read() error:");
        return err;
    }

    printf("client says: %.*s\n", len, &rbuf[4]);
    const char reply[] = "world";
    char wbuf[ 4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], reply, len);
    return write_all(connfd, wbuf, 4 + len);

}

static void fd_set_nb(int fd){
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static bool try_one_request(Conn *conn){
    if (conn->incoming.size() < 4){
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg){
        conn-> want_close = true;
        return false;
    }
    if (4 + len > conn->incoming.size()){
        return false;
    }
    const uint8_t *request = &conn->incoming[4];

    buf_append(conn->outgoing, (const uint8_t *)&len, 4);
    buf_append(conn->outgoing, request, len);
    buf_consume(conn->incoming, 4+ len);
    return true;

}

static Conn *handle_accept(int fd){
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if ( connfd < 0 ){
        return NULL;
    }
    fd_set_nb(connfd);
    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;

}

static void handle_read(Conn *conn){
    uint8_t buf[64 * 1024];
    ssize_t rv =read(conn->fd, buf, sizeof(buf));
    if (rv <= 0){
        conn->want_close = true;
        return ;
    }
    buf_append(conn->incoming, buf, (size_t)rv);
    try_one_request(conn);
    if (conn->outgoing.size() > 0){
        conn->want_read = false;
        conn->want_write = true;
    }
}

static void handle_write(Conn *conn){
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
    if (rv < 0){
        conn->want_close = true;
        return;
    }
    buf_consume(conn->outgoing, (size_t)rv);
    if (conn->outgoing.size() == 0){
        conn->want_read = true;
        conn->want_write = false;
    }
}


int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        die("WSAStartup()");
    }
#endif

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    // this is needed for most server applications
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // listen
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    // while (true) {
    //     // accept
    //     struct sockaddr_in client_addr = {};
    //     socklen_t addrlen = sizeof(client_addr);
    //     int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    //     if (connfd < 0) {
    //         continue;   // error
    //     }

    //     while (true){
    //         int32_t err = one_request(connfd);
    //         if (err){
    //             break;
    //         }
    //     }

    //     close(connfd);
    // }

    // Rewriting echo server into event loop


    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;
    while (true){
        poll_args.clear();
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        for (Conn *conn : fd2conn){
            if (!conn) {
                continue;
            }
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read) {
                pfd.events |= POLLIN;
            }
            if (conn->want_write){
                pfd.events |= POLLOUT;
            }
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (rv < 0 && errno == EINTR){
            continue;
        }
        if (rv < 0){
            die("poll");
        }

        if (poll_args[0].revents){
            if (Conn *conn = handle_accept(fd)){
                if (fd2conn.size() <= (size_t)conn->fd){
                    fd2conn.resize(conn->fd +1);
                }
                fd2conn[conn->fd] = conn;
            }
        }

        for (size_t i = 1; i<poll_args.size(); ++i){
            uint32_t ready = poll_args[i].revents;
            Conn *conn = fd2conn[poll_args[i].fd];
            if (ready & POLLIN){
                handle_read(conn);
            }
            if (ready & POLLOUT){
                handle_write(conn);
            }

            if ((ready & POLLERR) || conn->want_close){
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }

        }


    }

    return 0;
}
