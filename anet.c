// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// anet.c: Basic TCP socket stuff made a bit less boring
//
// Copyright (c) 2019 Michael Wolf <michael@mictronics.de>
//
// This code is based on a detached fork of dump1090-fa.
//
// Copyright (c) 2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// This file incorporates work covered by the following copyright and
// license:
//
// Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//   * Neither the name of Redis nor the names of its contributors may be used
//     to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/resource.h>

#ifdef __APPLE__
#include "compat/apple/net_compat.h"
#endif

#include "anet.h"

#define _UNUSED(V) ((void) V)

static void anetSetError(char *err, const char *fmt, ...)
{
    va_list ap;

    if (!err) return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

int anetNonBlock(char *err, int fd)
{
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
        return ANET_ERR;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        anetSetError(err, "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno));
        return ANET_ERR;
    }

    return ANET_OK;
}

int anetTcpNoDelay(char *err, int fd)
{
    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void*)&yes, sizeof(yes)) == -1)
    {
        anetSetError(err, "setsockopt TCP_NODELAY: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetSetSendBuffer(char *err, int fd, int buffsize)
{
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void*)&buffsize, sizeof(buffsize)) == -1)
    {
        anetSetError(err, "setsockopt SO_SNDBUF: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

int anetTcpKeepAlive(char *err, int fd)
{
    int yes = 1;

    int idle = 20;
    int interval = 2;
    int count = 3;

    #ifdef __APPLE__
    _UNUSED(yes);
    if (set_tcp_keepalive(fd, idle, interval, count) == -1) {
        anetSetError(err, "setsockopt tcp keepalive: %s", strerror(errno));
        return ANET_ERR;
    }
    #else
    if (
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void*) &yes, sizeof(yes))
            || setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, (void*) &idle, sizeof(idle))
            || setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, (void*) &interval, sizeof(interval))
            || setsockopt(fd, SOL_TCP, TCP_KEEPCNT, (void*) &count, sizeof(count))
       ) {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }
    #endif
    return ANET_OK;
}

static inline void emfileError() {
    struct rlimit limits;
    getrlimit(RLIMIT_NOFILE, &limits);
    fprintf(stderr, "<3>EMFILE: Out of file descriptors (either there is a leak that needs fixing or you need to increase ulimit if you need more than %d connections)!\n", (int) limits.rlim_cur);
}

int anetCreateSocket(char *err, int domain, int typeFlags)
{
    int s, on = 1;

    if ((s = socket(domain, SOCK_STREAM | typeFlags, 0)) == -1) {
        if (errno == EMFILE) {
            emfileError();
        }
        anetSetError(err, "creating socket: %s", strerror(errno));
        return ANET_ERR;
    }

    /* Make sure connection-intensive things like the redis benckmark
     * will be able to close/open sockets a zillion of times */
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*)&on, sizeof(on)) == -1) {
        anetSetError(err, "setsockopt SO_REUSEADDR: %s", strerror(errno));
        anetCloseSocket(s);
        return ANET_ERR;
    }
    return s;
}

#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1
static int anetTcpGenericConnect(char *err, char *addr, char *service, int flags, struct sockaddr_storage *ss)
{
    int s;
    struct addrinfo gai_hints;
    struct addrinfo *gai_result, *p;
    int gai_error;

    gai_hints.ai_family = AF_UNSPEC;
    gai_hints.ai_socktype = SOCK_STREAM;
    gai_hints.ai_protocol = 0;
    gai_hints.ai_flags = 0;
    gai_hints.ai_addrlen = 0;
    gai_hints.ai_addr = NULL;
    gai_hints.ai_canonname = NULL;
    gai_hints.ai_next = NULL;

    gai_error = getaddrinfo(addr, service, &gai_hints, &gai_result);
    if (gai_error != 0) {
        anetSetError(err, "can't resolve %s: %s", addr, gai_strerror(gai_error));
        return ANET_ERR;
    }

    for (p = gai_result; p != NULL; p = p->ai_next) {
        int nonBlock = (flags & ANET_CONNECT_NONBLOCK) ? SOCK_NONBLOCK : 0;
        if ((s = anetCreateSocket(err, p->ai_family, nonBlock)) == ANET_ERR)
            continue;

        if (connect(s, p->ai_addr, p->ai_addrlen) >= 0 || (errno == EINPROGRESS && (flags & ANET_CONNECT_NONBLOCK))
           ) {
            // If we were passed a place to toss the sockaddr info, save it
            if (ss) {
                memcpy(ss, p->ai_addr, sizeof(*ss));
            }
            if (gai_result) {
                freeaddrinfo(gai_result);
                gai_result = NULL;
            }
            return s;
        }

        anetSetError(err, "connect: %s", strerror(errno));
        anetCloseSocket(s);
    }

    if (gai_result) {
        freeaddrinfo(gai_result);
        gai_result = NULL;
    }
    return ANET_ERR;
}

int anetTcpConnect(char *err, char *addr, char *service, struct sockaddr_storage *ss)
{
    return anetTcpGenericConnect(err,addr,service,ANET_CONNECT_NONE, ss);
}

int anetTcpNonBlockConnect(char *err, char *addr, char *service, struct sockaddr_storage *ss)
{
    return anetTcpGenericConnect(err,addr,service,ANET_CONNECT_NONBLOCK, ss);
}

int anetGetaddrinfo(char *err, char *addr, char *service, struct addrinfo **gai_result)
{
    struct addrinfo gai_hints;
    int gai_error;

    gai_hints.ai_family = AF_UNSPEC;
    gai_hints.ai_socktype = SOCK_STREAM;
    gai_hints.ai_protocol = 0;
    gai_hints.ai_flags = 0;
    gai_hints.ai_addrlen = 0;
    gai_hints.ai_addr = NULL;
    gai_hints.ai_canonname = NULL;
    gai_hints.ai_next = NULL;

    gai_error = getaddrinfo(addr, service, &gai_hints, gai_result);
    if (gai_error != 0) {
        anetSetError(err, "can't resolve %s: %s", addr, gai_strerror(gai_error));
        return ANET_ERR;
    }
    return 0;
}

int anetTcpNonBlockConnectAddr(char *err, struct addrinfo *p)
{
    int s;

    if ((s = anetCreateSocket(err, p->ai_family, SOCK_NONBLOCK)) == ANET_ERR)
        return ANET_ERR;

    if (connect(s, p->ai_addr, p->ai_addrlen) >= 0) {
        return s;
    }

    if (errno == EINPROGRESS) {
        return s;
    }

    anetSetError(err, "connect: %s", strerror(errno));
    anetCloseSocket(s);
    return ANET_ERR;
}

/* Like read(2) but make sure 'count' is read before to return
 * (unless error or EOF condition is encountered) */
int anetRead(int fd, char *buf, int count)
{
    int nread, totlen = 0;
    while(totlen < count) {
        nread = read(fd, buf, (size_t) (count - totlen));
        if (nread == 0) return totlen;
        if (nread == -1) return -1;
        totlen += nread;
        buf += nread;
    }
    return totlen;
}

/* Like write(2) but make sure 'count' is read before to return
 * (unless error is encountered) */
int anetWrite(int fd, char *buf, int count)
{
    int nwritten, totlen = 0;
    while(totlen != count) {
        nwritten = write(fd, buf, (size_t) (count - totlen));
        if (nwritten == 0) return totlen;
        if (nwritten == -1) return -1;
        totlen += nwritten;
        buf += nwritten;
    }
    return totlen;
}

static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len) {
    if (sa->sa_family == AF_INET6) {
        int on = 1;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
    }

    if (bind(s,sa,len) == -1) {
        anetSetError(err, "bind: %s", strerror(errno));
        anetCloseSocket(s);
        return ANET_ERR;
    }

    // no real drawback to using a large backlog, will usually be capped to 4096 by the kernel
    if (listen(s, 65535) == -1) {
        anetSetError(err, "listen: %s", strerror(errno));
        anetCloseSocket(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static void anetSetBuffers(int fd, int sndsize, int rcvsize) {
    if (sndsize > 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void*)&sndsize, sizeof(sndsize)) == -1) {
        fprintf(stderr, "setsockopt SO_SNDBUF: %s", strerror(errno));
    }

    if (rcvsize > 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void*)&rcvsize, sizeof(rcvsize)) == -1) {
        fprintf(stderr, "setsockopt SO_RCVBUF: %s", strerror(errno));
    }
}

int anetTcpServer(char *err, char *service, char *bindaddr, int *fds, int nfds, int flags, int sndsize, int rcvsize)
{
    int s;
    int i = 0;
    struct addrinfo gai_hints;
    struct addrinfo *gai_result, *p;
    int gai_error;

    gai_hints.ai_family = AF_UNSPEC;
    gai_hints.ai_socktype = SOCK_STREAM;
    gai_hints.ai_protocol = 0;
    gai_hints.ai_flags = AI_PASSIVE;
    gai_hints.ai_addrlen = 0;
    gai_hints.ai_addr = NULL;
    gai_hints.ai_canonname = NULL;
    gai_hints.ai_next = NULL;

    gai_error = getaddrinfo(bindaddr, service, &gai_hints, &gai_result);
    if (gai_error != 0) {
        anetSetError(err, "can't resolve %s: %s", bindaddr, gai_strerror(gai_error));
        return ANET_ERR;
    }

    for (p = gai_result; p != NULL && i < nfds; p = p->ai_next) {
        if ((s = anetCreateSocket(err, p->ai_family, flags)) == ANET_ERR)
            continue;

        anetSetBuffers(s, sndsize, rcvsize);

        if (anetListen(err, s, p->ai_addr, p->ai_addrlen) == ANET_ERR) {
            continue;
        }

        fds[i++] = s;
    }

    if (gai_result) {
        freeaddrinfo(gai_result);
        gai_result = NULL;
    }
    return (i > 0 ? i : ANET_ERR);
}
int anetUnixSocket(char *err, char *path, int flags)
{
    int s;
    if ((s = anetCreateSocket(err, AF_UNIX, flags)) == ANET_ERR) {
        return ANET_ERR;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        anetSetError(err, "bind: %s", strerror(errno));
        anetCloseSocket(s);
        return ANET_ERR;
    }

    // explicitely setting tcp buffers causes failure of linux tcp window auto tuning ... it just doesn't work well without the auto tuning
    // anetSetBuffers(s, 512 * 1024, 64 * 1024);

    // no real drawback to using a large backlog, will usually be capped to 4096 by the kernel
    if (listen(s, 65535) == -1) {
        anetSetError(err, "listen: %s", strerror(errno));
        anetCloseSocket(s);
        return ANET_ERR;
    }

    return s;
}

int anetGenericAccept(char *err, int s, struct sockaddr *sa, socklen_t *len, int flags)
{
    int fd;

    fd = accept4(s, sa, len, flags);

    if (fd == -1) {
        if (errno == EMFILE) {
            emfileError();
        }
        if (errno != EINTR) {
            anetSetError(err, "Generic accept error: %s %d", strerror(errno), fd);
        }
        return ANET_ERR;
    }

    return fd;
}

static inline int get_socket_error(int fd) {
    int err = 1;
    socklen_t len = sizeof err;
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &err, &len)) {
        fprintf(stderr, "Get client socket error failed.\n");
    }
    if (err) {
        errno = err; // Set errno to the socket SO_ERROR
    }
    return err;
}

void anetCloseSocket(int fd) {
    if (fd < 0) {
        return;
    }
    get_socket_error(fd); // First clear any errors, which can cause close to fail
    if (shutdown(fd, SHUT_RDWR) < 0) { // Secondly, terminate the reliable delivery
        if (errno != ENOTCONN && errno != EINVAL) { // SGI causes EINVAL
            fprintf(stderr, "Shutdown client socket failed.\n");
        }
    }
    close(fd);
}
