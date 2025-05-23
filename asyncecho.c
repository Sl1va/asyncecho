/*
 * Copyright (c) 2025 Emil Latypov <emillatypov9335@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of asyncecho nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/******************************************************************************
 * DEFINES, CONSTS, ENUMS
 ******************************************************************************/

#define LISTEN_PORT 5000
#define BUFFER_SIZE 1024

/******************************************************************************
 * STRUCTURES
 ******************************************************************************/

typedef struct {
    size_t len;
    uint8_t buf[BUFFER_SIZE];
} buff_t;

/******************************************************************************
 * PRIVATE FUNCTION PROTOTYPES
 ******************************************************************************/

static ev_io *alloc_client();
static void dealloc_client(ev_io *client_watcher);
static inline void destroy_client_watcher(struct ev_loop *loop,
                                          ev_io *client_watcher);
static int setup_listener_socket();
static int socket_set_nonblocking(int sockfd);
static int socket_set_reusable(int sockfd);
static char *addr2str(struct sockaddr_in *addr);
static inline int is_async_skip();
static void accept_handler(struct ev_loop *loop, ev_io *w, int revents);
static void client_handler(struct ev_loop *loop, ev_io *w, int revents);
static void logger(struct ev_loop *loop, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/******************************************************************************
 * PRIVATE FUNCTIONS
 ******************************************************************************/

/**
 * @brief Allocate memory for client watcher
 *
 * @return ev_io* Allocated client watcher
 */
static ev_io *alloc_client() {
    ev_io *client_watcher = (ev_io *)malloc(sizeof(ev_io));
    if (!client_watcher) {
        return NULL;
    }

    client_watcher->data = (buff_t *)malloc(sizeof(buff_t));
    if (!client_watcher->data) {
        free(client_watcher);
        return NULL;
    }

    memset(client_watcher->data, 0, sizeof(buff_t));
    return client_watcher;
}

/**
 * @brief Deallocate client watcher
 *
 * @param client_watcher Client watcher to deallocate
 */
static void dealloc_client(ev_io *client_watcher) {
    free(client_watcher->data);
    free(client_watcher);
}

/**
 * @brief Deinitialize and deallocate client watcher from event loop
 *
 * @param loop Event loop
 * @param client_watcher Watcher to deinitialize
 */
static inline void destroy_client_watcher(struct ev_loop *loop,
                                          ev_io *client_watcher) {
    ev_io_stop(loop, client_watcher);
    close(client_watcher->fd);
    dealloc_client(client_watcher);
}

/**
 * @brief Set up non-blocking socket in listen() mode
 *
 * @return int Socket fd
 */
static int setup_listener_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return sockfd;
    }

    if (socket_set_nonblocking(sockfd) < 0) {
        close(sockfd);
        return -1;
    }

    if (socket_set_reusable(sockfd) < 0) {
        close(sockfd);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(LISTEN_PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
        0) {
        exit(EXIT_FAILURE);
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/**
 * @brief Set socket to non-blocking mode
 *
 * @param sockfd Socket fd
 * @return int fcntl return value
 */
static int socket_set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);

    if (flags < 0) {
        return flags;
    }

    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief Make socket address reusable on exit
 *
 * @param sockfd Socket fd
 * @return int setsockopt return value
 */
static int socket_set_reusable(int sockfd) {
    int status =
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    if (status < 0) {
        return status;
    }

    struct linger linger = {.l_onoff = 1, .l_linger = 0};
    status = setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
    return status;
}

/**
 * @brief Convert sockaddr_in structure to string value
 * (uses static buffer, not thread-safe)
 *
 * @param addr sockaddr_in structure pointer
 * @return char* Pointer to static string buffer
 */
static char *addr2str(struct sockaddr_in *addr) {
    // NOTE: not thread-safe, but does not require deinitialization
    // (Fine for asynchronous event loop)
    static char buf[64] = {0};
    memset(buf, 0, sizeof(buf));

    char ipaddr[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &(addr->sin_addr), ipaddr, INET_ADDRSTRLEN);
    uint16_t port = ntohs(addr->sin_port);

    snprintf(buf, sizeof(buf), "%s:%u", ipaddr, port);
    return buf;
}

/**
 * @brief Check if error is temporal and does not require abort
 *
 * @return int 1 if error temporal, 0 otherwise
 */
static inline int is_async_skip() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}

/**
 * @brief Handler for listener socket to accept new client connections
 *
 * @param loop Pointer to loop instance
 * @param w Pointer to watcher instance
 * @param revents Events triggered
 */
static void accept_handler(struct ev_loop *loop, ev_io *w, int revents) {
    struct sockaddr_in client_addr = {0};
    socklen_t client_len = sizeof(client_addr);
    int client = accept(w->fd, (struct sockaddr *)&client_addr, &client_len);

    if (client < 0) {
        if (!is_async_skip()) {
            perror("accept()");
        }
        return;
    }

    if (socket_set_nonblocking(client) < 0) {
        perror("accept_handler::accept()");
        return;
    }

    logger(loop, "New client %s\n", addr2str(&client_addr));

    ev_io *client_watcher = alloc_client();
    ev_io_init(client_watcher, client_handler, client, EV_READ);
    ev_io_start(loop, client_watcher);
}

/**
 * @brief Handler for client connections (provides echo responses)
 *
 * @param loop Pointer to loop instance
 * @param w Pointer to watcher instance
 * @param revents Events triggered
 */
static void client_handler(struct ev_loop *loop, ev_io *w, int revents) {
    struct sockaddr_in client_addr = {0};
    socklen_t addr_len = sizeof(client_addr);
    getpeername(w->fd, (struct sockaddr *)(&client_addr), &addr_len);
    const char *client_name = addr2str(&client_addr);

    buff_t *buf = (buff_t *)w->data;
    ssize_t bytes_n = 0;  // Number of bytes processed at current state
    int next_event = 0;   // Next event to wait (either EV_READ or EV_WRITE)

    if (revents & EV_READ) {
        // If the client connection is readable, read the data
        // into intermediate buffer and switch watcher to wait for
        // writeable state, so we can write the buffer data back to it
        bytes_n = read(w->fd, buf->buf, sizeof(buf->buf));
        buf->len = bytes_n;

        if (bytes_n > 0) {
            logger(loop, "Received %lu bytes of data from %s\n", bytes_n,
                   client_name);
        }

        next_event = EV_WRITE;

    } else if (revents & EV_WRITE) {
        // Writeable state is following readable state.
        // It is guaranteed that:
        //   1. There is buffer with some ready data to be sent to client
        //   2. Client socket is read to be written
        //      (unless error happened on it)

        // So we can try to write buffer to socket and then switch back to
        // waiting for readable state

        bytes_n = write(w->fd, buf->buf, buf->len);
        next_event = EV_READ;
    } else {
        // Unexpected state occured
        logger(loop, "Unexpected state occured with %s. Terminating connection",
               addr2str(&client_addr));
        destroy_client_watcher(loop, w);
        return;
    }

    if (bytes_n < 0) {
        // Some error occured - need to abort connection
        // (unless it is temporal error caused by non-blocking mode)
        if (!is_async_skip()) {
            logger(loop, "Connection with %s failed (%s)", client_name,
                   strerror(errno));
            destroy_client_watcher(loop, w);
        }

        // In case if the error is temporal - make another attempt
        return;
    } else if (bytes_n == 0) {
        // Connection has been terminated - just cleanup
        logger(loop, "Connection with %s has been terminated\n", client_name);
        destroy_client_watcher(loop, w);
        return;
    }

    // Update watcher event type to opposite
    ev_io_stop(loop, w);
    ev_io_set(w, w->fd, next_event);
    ev_io_start(loop, w);
}

/**
 * @brief printf-like logger function, which adds timestamp to messages
 *
 * @param loop Loop instance
 * @param format Format string
 * @param ... Arguments for formatting
 */
static void logger(struct ev_loop *loop, const char *format, ...) {
    va_list args;
    va_start(args, format);

    time_t ts = (time_t)ev_now(loop);
    struct tm *time_info = localtime(&ts);

    char timebuf[80];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", time_info);

    printf("[%s] ", timebuf);
    vprintf(format, args);
    va_end(args);
}

int main() {
    struct ev_loop *loop = EV_DEFAULT;

    // setup connection listener socket
    int sockfd = setup_listener_socket();
    if (sockfd < 0) {
        perror("setup_listener_socket()");
        exit(EXIT_FAILURE);
    }

    // setup watcher
    ev_io accept_watcher = {0};
    ev_io_init(&accept_watcher, accept_handler, sockfd, EV_READ);
    ev_io_start(loop, &accept_watcher);

    // start event loop
    ev_run(loop, 0);
    ev_loop_destroy(loop);
}