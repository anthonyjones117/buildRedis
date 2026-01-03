// ======================= client.cpp =======================
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")

    typedef SOCKET socket_t;

    #define read(fd, buf, n)  recv((fd), (buf), (int)(n), 0)
    #define write(fd, buf, n) send((fd), (buf), (int)(n), 0)
    #define close             closesocket
#else
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netinet/ip.h>

    typedef int socket_t;
#endif

#include <string>
#include <vector>

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

static int32_t read_full(socket_t fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, (char *)buf, n);
        if (rv <= 0) return -1;
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(socket_t fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, (const char *)buf, n);
        if (rv <= 0) return -1;
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

const size_t k_max_msg = 32 << 20;

static int32_t send_req(socket_t fd, const uint8_t *text, uint32_t len) {
    if (len > k_max_msg) return -1;

    std::vector<uint8_t> wbuf;
    buf_append(wbuf, (const uint8_t *)&len, 4);
    buf_append(wbuf, text, len);

    return write_all(fd, wbuf.data(), wbuf.size());
}

static int32_t read_res(socket_t fd) {
    std::vector<uint8_t> rbuf(4);

    int32_t err = read_full(fd, rbuf.data(), 4);
    if (err) {
        msg("read failed / EOF");
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf.data(), 4);
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    rbuf.resize(4 + len);
    err = read_full(fd, rbuf.data() + 4, len);
    if (err) {
        msg("read body failed");
        return err;
    }

    printf("len:%u data:%.*s\n", len, (int)(len < 100 ? len : 100), (char*)rbuf.data() + 4);
    return 0;
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        die("WSAStartup()");
    }
#endif

    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) die("socket()");
#else
    if (fd < 0) die("socket()");
#endif

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) die("connect()");

    std::vector<std::string> query_list = {
        "hello1", "hello2", "hello3",
        std::string((size_t)k_max_msg, 'z'),
        "hello5",
    };

    for (const std::string &s : query_list) {
        uint32_t len = (uint32_t)s.size();
        int32_t e = send_req(fd, (const uint8_t *)s.data(), len);
        if (e) goto L_DONE;
    }

    for (size_t i = 0; i < query_list.size(); ++i) {
        int32_t e = read_res(fd);
        if (e) goto L_DONE;
    }

L_DONE:
    close(fd);
    return 0;
}
