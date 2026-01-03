// ======================= server.cpp =======================
// stdlib
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

// C++
#include <vector>
#include <map>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")

    typedef SOCKET socket_t;
    typedef WSAPOLLFD pollfd_t;
    typedef ULONG nfds_t;

    #define read(fd, buf, n)  recv((fd), (buf), (int)(n), 0)
    #define write(fd, buf, n) send((fd), (buf), (int)(n), 0)
    #define close             closesocket
#else
    #include <fcntl.h>
    #include <poll.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netinet/ip.h>

    typedef int socket_t;
    typedef struct pollfd pollfd_t;
    typedef unsigned long nfds_t;
#endif

static void msg(const char *m) {
    fprintf(stderr, "%s\n", m);
}

static void die(const char *m) {
#ifdef _WIN32
    int err = WSAGetLastError();
#else
    int err = errno;
#endif
    fprintf(stderr, "[%d] %s\n", err, m);
    abort();
}

// Windows/non-Windows "would block" detection
static bool would_block() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static void fd_set_nb(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        die("ioctlsocket(FIONBIO)");
    }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) die("fcntl(F_GETFL)");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) die("fcntl(F_SETFL)");
#endif
}

const size_t k_max_msg = 32 << 20;  // 32MB

struct Conn {
    socket_t fd = (socket_t)-1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

static Conn *handle_accept(socket_t listen_fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = (socklen_t)sizeof(client_addr);

    socket_t connfd = accept(listen_fd, (struct sockaddr *)&client_addr, &addrlen);

#ifdef _WIN32
    if (connfd == INVALID_SOCKET) {
        // WSAGetLastError() can be checked here if desired
        return NULL;
    }
#else
    if (connfd < 0) {
        return NULL;
    }
#endif

    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, (ip >> 24) & 255,
        ntohs(client_addr.sin_port)
    );

    fd_set_nb(connfd);

    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

static bool try_one_request(Conn *conn) {
    if (conn->incoming.size() < 4) return false;

    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);

    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;
    }

    if (4 + len > conn->incoming.size()) return false;

    const uint8_t *request = &conn->incoming[4];

    printf("client says: len:%u data:%.*s\n",
           len, (int)(len < 100 ? len : 100), (const char*)request);

    // echo response
    buf_append(conn->outgoing, (const uint8_t *)&len, 4);
    buf_append(conn->outgoing, request, len);

    buf_consume(conn->incoming, 4 + len);
    return true;
}

static void handle_write(Conn *conn) {
    assert(!conn->outgoing.empty());

    ssize_t rv = write(conn->fd, (const char *)conn->outgoing.data(), conn->outgoing.size());
    if (rv < 0) {
        if (would_block()) return;
        conn->want_close = true;
        return;
    }

    buf_consume(conn->outgoing, (size_t)rv);

    if (conn->outgoing.empty()) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];

    ssize_t rv = read(conn->fd, (char *)buf, sizeof(buf));
    if (rv < 0) {
        if (would_block()) return;
        conn->want_close = true;
        return;
    }

    if (rv == 0) {
        msg("client closed");
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    while (try_one_request(conn)) {}

    if (!conn->outgoing.empty()) {
        conn->want_read = false;
        conn->want_write = true;
        handle_write(conn); // opportunistic flush
    }
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        die("WSAStartup()");
    }
#endif

    socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (listen_fd == INVALID_SOCKET) die("socket()");
#else
    if (listen_fd < 0) die("socket()");
#endif

    int val = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);

    int rv = bind(listen_fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) die("bind()");

    fd_set_nb(listen_fd);

    rv = listen(listen_fd, SOMAXCONN);
    if (rv) die("listen()");

    std::map<socket_t, Conn *> fd2conn;
    std::vector<pollfd_t> poll_args;

    while (true) {
        poll_args.clear();

        // listening socket
        pollfd_t p0 = {};
        p0.fd = listen_fd;
        p0.events = POLLIN;
        poll_args.push_back(p0);

        // client sockets
        for (auto &pair : fd2conn) {
            Conn *conn = pair.second;
            if (!conn) continue;

            pollfd_t p = {};
            p.fd = conn->fd;
            p.events = POLLERR;
            if (conn->want_read)  p.events |= POLLIN;
            if (conn->want_write) p.events |= POLLOUT;
            poll_args.push_back(p);
        }

#ifdef _WIN32
        int pr = WSAPoll(poll_args.data(), (nfds_t)poll_args.size(), -1);
#else
        int pr = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
#endif
        if (pr < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            die("poll");
        }

        // accept new client
        if (poll_args[0].revents & POLLIN) {
            if (Conn *conn = handle_accept(listen_fd)) {
                fd2conn[conn->fd] = conn;
            }
        }

        // handle clients
        for (size_t i = 1; i < poll_args.size(); ++i) {
            uint32_t ready = poll_args[i].revents;
            if (!ready) continue;

            socket_t sock_fd = poll_args[i].fd;

            auto it = fd2conn.find(sock_fd);
            if (it == fd2conn.end()) continue;

            Conn *conn = it->second;

            if (ready & POLLIN) {
                if (conn->want_read) handle_read(conn);
            }
            if (ready & POLLOUT) {
                if (conn->want_write) handle_write(conn);
            }

            if ((ready & POLLERR) || conn->want_close) {
                close(conn->fd);
                fd2conn.erase(sock_fd);
                delete conn;
            }
        }
    }

    return 0;
}
