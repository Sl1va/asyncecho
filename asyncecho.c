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
 * @brief Set up non-blocking socket in listen() state
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
 * @brief Set socket to non-blocking state
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
 * @brief Convert sockaddr_in structure to string value (uses static buffer, not
 * thread-safe)
 *
 * @param addr sockaddr_in structure pointer
 * @return char* Pointer to static string buffer
 */
static char *addr2str(struct sockaddr_in *addr) {
    // NOTE: not thread-safe, but does not require deinitialization
    // (Fine for asynchronous event loop)
    static char buf[BUFFER_SIZE] = {0};
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

    if (revents & EV_READ) {
        // If the client connection is readable, read the data
        // into intermediate buffer and switch watcher to wait for
        // writeable state, so we can write the buffer data back to it
        ssize_t read_n = read(w->fd, buf->buf, sizeof(buf->buf));
        buf->len = read_n;

        if (read_n < 0) {
            // Some error occured - need to abort connection (if it is not
            // temporal state)
            if (!is_async_skip()) {
                logger(loop, "Connection with %s failed (%s)", client_name,
                       strerror(errno));
                destroy_client_watcher(loop, w);
            }

            // In case if the error is temporal - make another attempt to read
            return;
        } else if (read_n == 0) {
            // Connection has been terminated
            logger(loop, "Connection with %s has been terminated\n",
                   client_name);
            destroy_client_watcher(loop, w);
            return;
        }

        logger(loop, "Received %lu bytes of data from %s\n", read_n,
               client_name);

        ev_io_stop(loop, w);
        ev_io_set(w, w->fd, EV_WRITE);
        ev_io_start(loop, w);
    } else if (revents & EV_WRITE) {
        // At this point we are guaranteed to have:
        //   1. Buffer with some data to send back to client
        //   2. Writeable socket (unless some error happened)
        // So we can try to write back to socket and then switch back to
        // listener state

        ssize_t write_n = write(w->fd, buf->buf, buf->len);

        if (write_n < 0) {
            if (!is_async_skip()) {
                logger(loop, "Connection with %s failed (%s)", client_name,
                       strerror(errno));
                destroy_client_watcher(loop, w);
            }

            return;
        } else if (write_n == 0) {
            logger(loop, "Connection with %s has been terminated\n",
                   client_name);
            destroy_client_watcher(loop, w);
            return;
        }

        buf->len = 0;
        ev_io_stop(loop, w);
        ev_io_set(w, w->fd, EV_READ);
        ev_io_start(loop, w);
    } else {
        // Unexpected state occured
        logger(loop, "Unexpected state occured with %s. Terminating connection",
               addr2str(&client_addr));
        destroy_client_watcher(loop, w);
        return;
    }
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