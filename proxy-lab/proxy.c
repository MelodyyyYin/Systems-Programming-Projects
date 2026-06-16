/*
 * Starter code for proxy lab.
 * Feel free to modify this code in whatever way you wish.
 */

/* Some useful includes to help you get started */

#include "csapp.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif

/*
 * Max cache and object sizes
 * You might want to move these to the file containing your cache implementation
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)
#define WORKER_THREADS 32
#define CONN_QUEUE_SIZE 64

/*
 * String to use for the User-Agent header.
 * Don't forget to terminate with \r\n
 */
static const char *header_user_agent = "Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20220411 Firefox/63.0.1";

#define CACHE_SLOTS 128

typedef struct {
    bool valid;
    char key[MAXLINE];
    char *data;
    size_t size;
    unsigned long last_use;
} cache_entry_t;

static cache_entry_t cache_entries[CACHE_SLOTS];
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long cache_clock = 0;
static size_t cache_bytes = 0;

static int conn_queue[CONN_QUEUE_SIZE];
static int conn_q_front = 0;
static int conn_q_back = 0;
static int conn_q_count = 0;
static pthread_mutex_t conn_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t conn_q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t conn_q_not_full = PTHREAD_COND_INITIALIZER;

static void cache_remove_entry(int idx) {
    if (!cache_entries[idx].valid) {
        return;
    }

    Free(cache_entries[idx].data);
    cache_bytes -= cache_entries[idx].size;
    cache_entries[idx].valid = false;
    cache_entries[idx].data = NULL;
    cache_entries[idx].size = 0;
    cache_entries[idx].last_use = 0;
    cache_entries[idx].key[0] = '\0';
}

static int cache_find_slot(const char *key) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (cache_entries[i].valid && strcmp(cache_entries[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int cache_find_empty_slot(void) {
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (!cache_entries[i].valid) {
            return i;
        }
    }
    return -1;
}

static int cache_find_lru_slot(void) {
    int slot = -1;
    unsigned long oldest = 0;

    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (!cache_entries[i].valid) {
            continue;
        }
        if (slot == -1 || cache_entries[i].last_use < oldest) {
            slot = i;
            oldest = cache_entries[i].last_use;
        }
    }
    return slot;
}

static bool cache_lookup(const char *key, char *buf, size_t *size) {
    bool hit = false;

    pthread_mutex_lock(&cache_mutex);
    int idx = cache_find_slot(key);
    if (idx >= 0) {
        *size = cache_entries[idx].size;
        memcpy(buf, cache_entries[idx].data, cache_entries[idx].size);
        cache_entries[idx].last_use = ++cache_clock;
        hit = true;
    }
    pthread_mutex_unlock(&cache_mutex);

    return hit;
}

static void cache_store(const char *key, const char *data, size_t size) {
    if (size == 0 || size > MAX_OBJECT_SIZE || size > MAX_CACHE_SIZE) {
        return;
    }

    pthread_mutex_lock(&cache_mutex);

    int idx = cache_find_slot(key);
    if (idx >= 0) {
        cache_remove_entry(idx);
    }

    while (cache_bytes + size > MAX_CACHE_SIZE) {
        int lru = cache_find_lru_slot();
        if (lru < 0) {
            break;
        }
        cache_remove_entry(lru);
    }

    idx = cache_find_empty_slot();
    if (idx < 0) {
        int lru = cache_find_lru_slot();
        if (lru >= 0) {
            cache_remove_entry(lru);
            idx = cache_find_empty_slot();
        }
    }

    if (idx >= 0 && cache_bytes + size <= MAX_CACHE_SIZE) {
        cache_entries[idx].data = Malloc(size);
        memcpy(cache_entries[idx].data, data, size);
        snprintf(cache_entries[idx].key, sizeof(cache_entries[idx].key), "%s",
                 key);
        cache_entries[idx].valid = true;
        cache_entries[idx].size = size;
        cache_entries[idx].last_use = ++cache_clock;
        cache_bytes += size;
    }

    pthread_mutex_unlock(&cache_mutex);
}

static void enqueue_connfd(int connfd) {
    pthread_mutex_lock(&conn_q_mutex);
    while (conn_q_count == CONN_QUEUE_SIZE) {
        pthread_cond_wait(&conn_q_not_full, &conn_q_mutex);
    }
    conn_queue[conn_q_back] = connfd;
    conn_q_back = (conn_q_back + 1) % CONN_QUEUE_SIZE;
    conn_q_count++;
    pthread_cond_signal(&conn_q_not_empty);
    pthread_mutex_unlock(&conn_q_mutex);
}

static int dequeue_connfd(void) {
    int connfd;

    pthread_mutex_lock(&conn_q_mutex);
    while (conn_q_count == 0) {
        pthread_cond_wait(&conn_q_not_empty, &conn_q_mutex);
    }
    connfd = conn_queue[conn_q_front];
    conn_q_front = (conn_q_front + 1) % CONN_QUEUE_SIZE;
    conn_q_count--;
    pthread_cond_signal(&conn_q_not_full);
    pthread_mutex_unlock(&conn_q_mutex);

    return connfd;
}

static void clienterror(int fd, const char *cause, const char *errnum,
                        const char *shortmsg, const char *longmsg) {
    char header[MAXLINE];
    char body[MAXLINE];
    size_t bodylen;
    ssize_t n;

    (void)cause;

    snprintf(body, sizeof(body),
             "<html><title>Proxy Error</title>"
             "<body bgcolor=\"ffffff\">\r\n"
             "%s: %s\r\n"
             "<p>%s\r\n"
             "<hr><em>CS:APP Proxy</em>\r\n"
             "</body></html>\r\n",
             errnum, shortmsg, longmsg);
    bodylen = strlen(body);

    n = snprintf(header, sizeof(header),
                 "HTTP/1.0 %s %s\r\n"
                 "Content-Type: text/html\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 errnum, shortmsg, bodylen);
    if (n < 0 || n >= (ssize_t)sizeof(header)) {
        return;
    }

    rio_writen(fd, header, (size_t)n);
    rio_writen(fd, body, bodylen);
}

static int parse_uri(const char *uri, char *hostname, char *port, char *path) {
    const char *hostbegin = strstr(uri, "//");
    const char *pathbegin;
    const char *portbegin;
    size_t hostlen;
    char hostport[MAXLINE];

    if (hostbegin == NULL) {
        return -1;
    }
    hostbegin += 2;
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        pathbegin = uri + strlen(uri);
        strcpy(path, "/");
    } else {
        snprintf(path, MAXLINE, "%s", pathbegin);
    }

    hostlen = (size_t)(pathbegin - hostbegin);
    if (hostlen == 0 || hostlen >= sizeof(hostport)) {
        return -1;
    }
    snprintf(hostport, sizeof(hostport), "%.*s", (int)hostlen, hostbegin);

    portbegin = strchr(hostport, ':');
    if (portbegin == NULL) {
        snprintf(hostname, MAXLINE, "%s", hostport);
        snprintf(port, MAXLINE, "%s", "80");
    } else {
        size_t n = (size_t)(portbegin - hostport);
        if (n == 0) {
            return -1;
        }
        snprintf(hostname, MAXLINE, "%.*s", (int)n, hostport);
        snprintf(port, MAXLINE, "%s", portbegin + 1);
        if (port[0] == '\0') {
            return -1;
        }
    }
    return 0;
}

static int build_and_send_request(int serverfd, const char *hostname,
                                  const char *port, const char *path,
                                  const char *extra_headers) {
    char buf[MAXLINE];
    char hostline[MAXLINE];
    ssize_t n;

    if (strcmp(port, "80") == 0) {
        snprintf(hostline, sizeof(hostline), "%s", hostname);
    } else {
        snprintf(hostline, sizeof(hostline), "%s:%s", hostname, port);
    }

    n = snprintf(buf, sizeof(buf), "GET %s HTTP/1.0\r\n", path);
    if (n < 0 || n >= (ssize_t)sizeof(buf)) {
        return -1;
    }
    if (rio_writen(serverfd, buf, (size_t)n) < 0) {
        return -1;
    }

    n = snprintf(buf, sizeof(buf), "Host: %s\r\n", hostline);
    if (n < 0 || n >= (ssize_t)sizeof(buf)) {
        return -1;
    }
    if (rio_writen(serverfd, buf, (size_t)n) < 0) {
        return -1;
    }

    n = snprintf(buf, sizeof(buf), "User-Agent: %s\r\n", header_user_agent);
    if (n < 0 || n >= (ssize_t)sizeof(buf)) {
        return -1;
    }
    if (rio_writen(serverfd, buf, (size_t)n) < 0) {
        return -1;
    }

    if (extra_headers != NULL && extra_headers[0] != '\0') {
        if (rio_writen(serverfd, extra_headers, strlen(extra_headers)) < 0) {
            return -1;
        }
    }

    if (rio_writen(serverfd, "Connection: close\r\n",
                   strlen("Connection: close\r\n")) < 0) {
        return -1;
    }
    if (rio_writen(serverfd, "Proxy-Connection: close\r\n",
                   strlen("Proxy-Connection: close\r\n")) < 0) {
        return -1;
    }
    if (rio_writen(serverfd, "\r\n", 2) < 0) {
        return -1;
    }
    return 0;
}

static void build_cache_key(const char *hostname, const char *port,
                            const char *path, char *key, size_t keylen) {
    snprintf(key, keylen, "%s:%s%s", hostname, port, path);
}

static int relay_response(int serverfd, int clientfd, const char *cache_key) {
    rio_t rio;
    char line[MAXLINE];
    char object[MAX_OBJECT_SIZE];
    ssize_t n;
    size_t object_size = 0;
    bool can_cache = true;
    rio_readinitb(&rio, serverfd);
    while ((n = rio_readlineb(&rio, line, sizeof(line))) > 0) {
        if (can_cache && object_size + (size_t)n <= MAX_OBJECT_SIZE) {
            memcpy(object + object_size, line, (size_t)n);
            object_size += (size_t)n;
        } else {
            can_cache = false;
        }

        if (rio_writen(clientfd, line, (size_t)n) < 0) {
            return -1;
        }

        if (!strcmp(line, "\r\n") || !strcmp(line, "\n")) {
            break;
        }
    }

    if (n < 0) {
        return -1;
    }

    while ((n = rio_readnb(&rio, line, sizeof(line))) > 0) {
        if (can_cache && object_size + (size_t)n <= MAX_OBJECT_SIZE) {
            memcpy(object + object_size, line, (size_t)n);
            object_size += (size_t)n;
        } else {
            can_cache = false;
        }

        if (rio_writen(clientfd, line, (size_t)n) < 0) {
            return -1;
        }
    }

    if (can_cache && object_size <= MAX_OBJECT_SIZE) {
        cache_store(cache_key, object, object_size);
    }

    return 0;
}

static void handle_client(int connfd) {
    rio_t rio;
    char buf[MAXLINE];
    char method[MAXLINE];
    char uri[MAXLINE];
    char version[MAXLINE];
    char hostname[MAXLINE];
    char port[MAXLINE];
    char path[MAXLINE];
    char extra_headers[MAXLINE];
    char cache_key[MAXLINE];
    char cached_object[MAX_OBJECT_SIZE];
    size_t cached_size = 0;
    int serverfd = -1;
    ssize_t n;

    rio_readinitb(&rio, connfd);
    extra_headers[0] = '\0';

    n = rio_readlineb(&rio, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }

    if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
        clienterror(connfd, buf, "400", "Bad Request",
                    "Malformed request line");
        return;
    }

    if (strcasecmp(method, "GET") != 0) {
        clienterror(connfd, method, "501", "Not Implemented",
                    "Proxy does not implement this method");
        return;
    }

    while ((n = rio_readlineb(&rio, buf, sizeof(buf))) > 0) {
        if (!strcmp(buf, "\r\n") || !strcmp(buf, "\n")) {
            break;
        }
        if (strncasecmp(buf, "Host:", 5) == 0 ||
            strncasecmp(buf, "Connection:", 11) == 0 ||
            strncasecmp(buf, "Proxy-Connection:", 17) == 0 ||
            strncasecmp(buf, "User-Agent:", 11) == 0) {
            continue;
        }
        if (strlen(extra_headers) + strlen(buf) < sizeof(extra_headers)) {
            strncat(extra_headers, buf,
                    sizeof(extra_headers) - strlen(extra_headers) - 1);
        }
    }

    if (parse_uri(uri, hostname, port, path) < 0) {
        clienterror(connfd, uri, "400", "Bad Request", "Invalid URI");
        return;
    }

    build_cache_key(hostname, port, path, cache_key, sizeof(cache_key));
    if (cache_lookup(cache_key, cached_object, &cached_size)) {
        rio_writen(connfd, cached_object, cached_size);
        return;
    }

    serverfd = open_clientfd(hostname, port);
    if (serverfd < 0) {
        clienterror(connfd, hostname, "502", "Bad Gateway",
                    "Could not connect to origin server");
        return;
    }

    if (build_and_send_request(serverfd, hostname, port, path, extra_headers) <
        0) {
        close(serverfd);
        clienterror(connfd, hostname, "500", "Internal Server Error",
                    "Could not forward request");
        return;
    }

    if (relay_response(serverfd, connfd, cache_key) < 0) {
        close(serverfd);
        return;
    }

    close(serverfd);
}

static void *worker_main(void *vargp) {
    (void)vargp;

    for (;;) {
        int connfd = dequeue_connfd();
        handle_client(connfd);
        close(connfd);
    }

    return NULL;
}

int main(int argc, char **argv) {
    int listenfd;
    int connfd;
    pthread_t tid;
    pthread_attr_t attr;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, SIG_IGN);
    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "Failed to open listening socket on port %s\n",
                argv[1]);
        exit(1);
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);

    for (int i = 0; i < WORKER_THREADS; i++) {
        if (pthread_create(&tid, &attr, worker_main, NULL) != 0) {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            exit(1);
        }
    }

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
        if (connfd < 0) {
            continue;
        }
        enqueue_connfd(connfd);
    }

    pthread_attr_destroy(&attr);
    return 0;
}
