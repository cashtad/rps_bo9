// server.c
// Minimal TCP server skeleton for RPS bo9 project.
// - accept connections
// - parse simple line-based protocol (CRLF terminated)
// - implement HELLO, LIST, CREATE, JOIN (basic)
// - thread-per-client model, global mutex for rooms/clients

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <stdarg.h>

#define LISTEN_BACKLOG 16
#define LINE_BUF 512
#define MAX_CLIENTS 128
#define MAX_ROOMS 64
#define NICK_MAX 32
#define ROOM_NAME_MAX 64

typedef enum { ST_CONNECTED, ST_AUTH, ST_IN_LOBBY, ST_IN_ROOM } client_state_t;

typedef struct {
    int id;
    char name[ROOM_NAME_MAX+1];
    int players[2]; // client_fds (or -1)
    int player_count;
} room_t;

typedef struct {
    int fd;
    char nick[NICK_MAX+1];
    char token[64];
    client_state_t state;
    int room_id; // -1 if none
    time_t last_seen;
    pthread_t thread;
} client_t;

static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static client_t *clients[MAX_CLIENTS];
static room_t rooms[MAX_ROOMS];
static int next_room_id = 1;

/* utility: trim CRLF */
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n>0 && (s[n-1] == '\r' || s[n-1] == '\n')) { s[n-1] = '\0'; n--; }
}

/* send a line (adds CRLF) */
static int send_line(int fd, const char *fmt, ...) {
    char buf[LINE_BUF];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    strncat(buf, "\r\n", sizeof(buf)-strlen(buf)-1);
    size_t to_write = strlen(buf);
    ssize_t written = send(fd, buf, to_write, 0);
    return (written == (ssize_t)to_write) ? 0 : -1;
}

/* generate simple token */
static void gen_token(char *out, size_t outlen) {
    const char *hex = "0123456789abcdef";
    srand((unsigned)time(NULL) ^ (uintptr_t)pthread_self());
    for (size_t i=0;i<30 && i+1<outlen;i++) out[i] = hex[rand() % 16];
    out[30 < outlen ? 30 : outlen-1] = '\0';
}

/* find free client slot */
static int register_client(client_t *c) {
    pthread_mutex_lock(&global_lock);
    for (int i=0;i<MAX_CLIENTS;i++) {
        if (clients[i] == NULL) {
            clients[i] = c;
            pthread_mutex_unlock(&global_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&global_lock);
    return -1;
}

static void unregister_client(client_t *c) {
    pthread_mutex_lock(&global_lock);
    for (int i=0;i<MAX_CLIENTS;i++) {
        if (clients[i] == c) {
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&global_lock);
}

/* find room by id */
static room_t* find_room_by_id(int id) {
    for (int i=0;i<MAX_ROOMS;i++) {
        if (rooms[i].id == id) return &rooms[i];
    }
    return NULL;
}

/* create room */
static int create_room(const char *name) {
    pthread_mutex_lock(&global_lock);
    for (int i=0;i<MAX_ROOMS;i++) {
        if (rooms[i].id == 0) {
            rooms[i].id = next_room_id++;
            strncpy(rooms[i].name, name, ROOM_NAME_MAX);
            rooms[i].name[ROOM_NAME_MAX] = '\0';
            rooms[i].players[0] = rooms[i].players[1] = -1;
            rooms[i].player_count = 0;
            int id = rooms[i].id;
            pthread_mutex_unlock(&global_lock);
            return id;
        }
    }
    pthread_mutex_unlock(&global_lock);
    return -1;
}

/* list rooms: caller must hold no locks */
static int send_room_list(int fd) {
    pthread_mutex_lock(&global_lock);
    int count = 0;
    for (int i=0;i<MAX_ROOMS;i++) if (rooms[i].id != 0) count++;
    send_line(fd, "ROOM_LIST %d", count);
    for (int i=0;i<MAX_ROOMS;i++) {
        if (rooms[i].id == 0) continue;
        send_line(fd, "ROOM %d %s %d/2 %s", rooms[i].id, rooms[i].name, rooms[i].player_count,
                  (rooms[i].player_count==2) ? "PLAYING" : "OPEN");
    }
    pthread_mutex_unlock(&global_lock);
    return 0;
}

/* parse a single line and handle */
static void handle_line(client_t *c, char *line) {
    trim_crlf(line);
    if (strlen(line) == 0) return;
    // tokenize
    char *cmd = strtok(line, " ");
    if (!cmd) return;
    if (strcmp(cmd, "HELLO") == 0) {
        char *nick = strtok(NULL, " ");
        if (!nick) { send_line(c->fd, "ERR 100 BAD_FORMAT missing_nick"); return; }
        strncpy(c->nick, nick, NICK_MAX);
        c->nick[NICK_MAX] = '\0';
        gen_token(c->token, sizeof(c->token));
        c->state = ST_AUTH;
        send_line(c->fd, "WELCOME %s", c->token);
        return;
    } else if (strcmp(cmd, "LIST") == 0) {
        if (c->state < ST_AUTH) { send_line(c->fd, "ERR 101 INVALID_STATE not_auth"); return; }
        send_room_list(c->fd);
        return;
    } else if (strcmp(cmd, "CREATE") == 0) {
        if (c->state < ST_AUTH) { send_line(c->fd, "ERR 101 INVALID_STATE"); return; }
        char *rname = strtok(NULL, " ");
        if (!rname) { send_line(c->fd, "ERR 100 BAD_FORMAT missing_room_name"); return; }
        int rid = create_room(rname);
        if (rid < 0) { send_line(c->fd, "ERR 200 SERVER_FULL"); return; }
        send_line(c->fd, "ROOM_CREATED %d", rid);
        return;
    } else if (strcmp(cmd, "JOIN") == 0) {
        char *idstr = strtok(NULL, " ");
        if (!idstr) { send_line(c->fd, "ERR 100 BAD_FORMAT missing_room_id"); return; }
        int rid = atoi(idstr);
        pthread_mutex_lock(&global_lock);
        room_t *r = find_room_by_id(rid);
        if (!r) {
            pthread_mutex_unlock(&global_lock);
            send_line(c->fd, "ERR 104 UNKNOWN_ROOM");
            return;
        }
        if (r->player_count >= 2) {
            pthread_mutex_unlock(&global_lock);
            send_line(c->fd, "ERR 102 ROOM_FULL");
            return;
        }
        // add player
        for (int i=0;i<2;i++) if (r->players[i] == -1) { r->players[i] = c->fd; r->player_count++; break; }
        c->room_id = r->id;
        c->state = ST_IN_ROOM;
        pthread_mutex_unlock(&global_lock);
        send_line(c->fd, "ROOM_JOINED %d", r->id);
        return;
    } else if (strcmp(cmd, "QUIT") == 0) {
        send_line(c->fd, "OK bye");
        // close handled by caller
        return;
    } else if (strcmp(cmd, "PING") == 0) {
        send_line(c->fd, "PONG");
        return;
    } else {
        send_line(c->fd, "ERR 100 BAD_FORMAT unknown_command");
        return;
    }
}

/* client thread */
static void *client_worker(void *arg) {
    client_t *c = (client_t*)arg;
    char buf[LINE_BUF];
    ssize_t n;
    FILE *f = fdopen(c->fd, "r+");
    if (!f) {
        perror("fdopen");
        close(c->fd);
        unregister_client(c);
        free(c);
        return NULL;
    }
    setvbuf(f, NULL, _IOLBF, 0);
    while (fgets(buf, sizeof(buf), f) != NULL) {
        c->last_seen = time(NULL);
        handle_line(c, buf);
    }
    // cleanup on disconnect
    fprintf(stderr, "Client %s disconnected\n", c->nick);
    close(c->fd);
    unregister_client(c);
    free(c);
    return NULL;
}

int main(int argc, char **argv) {
    const char *host = "0.0.0.0";
    const char *port = "10000";
    if (argc >= 2) port = argv[1];
    int listen_fd;
    struct sockaddr_in servaddr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(1); }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(atoi(port));

    if (bind(listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) { perror("bind"); exit(1); }
    if (listen(listen_fd, LISTEN_BACKLOG) < 0) { perror("listen"); exit(1); }
    fprintf(stderr, "Server listening on 0.0.0.0:%s\n", port);

    /* init arrays */
    for (int i=0;i<MAX_CLIENTS;i++) clients[i] = NULL;
    for (int i=0;i<MAX_ROOMS;i++) rooms[i].id = 0;

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int connfd = accept(listen_fd, (struct sockaddr*)&cliaddr, &clilen);
        if (connfd < 0) {
            perror("accept");
            continue;
        }
        fprintf(stderr, "New connection fd=%d\n", connfd);
        client_t *c = calloc(1, sizeof(client_t));
        c->fd = connfd;
        c->state = ST_CONNECTED;
        c->room_id = -1;
        c->last_seen = time(NULL);
        gen_token(c->token, sizeof(c->token));
        if (register_client(c) != 0) {
            send_line(connfd, "ERR 200 SERVER_FULL");
            close(connfd);
            free(c);
            continue;
        }
        if (pthread_create(&c->thread, NULL, client_worker, c) != 0) {
            perror("pthread_create");
            unregister_client(c);
            close(connfd);
            free(c);
            continue;
        }
        pthread_detach(c->thread);
    }
}

