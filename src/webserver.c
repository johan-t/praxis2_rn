#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "data.h"
#include "dht.h"
#include "http.h"
#include "util.h"

#define MAX_RESOURCES 100

struct tuple resources[MAX_RESOURCES] = {
    {"/static/foo", "Foo", sizeof "Foo" - 1},
    {"/static/bar", "Bar", sizeof "Bar" - 1},
    {"/static/baz", "Baz", sizeof "Baz" - 1}
};

static struct dht_state dht = {0};

static void send_http_response(int conn, const char *response, size_t length) {
    if (send(conn, response, length, 0) == -1) {
        perror("send");
        close(conn);
    }
}

static void send_redirect(int conn, const char *ip, const char *port, const char *uri) {
    char buffer[HTTP_MAX_SIZE];
    int len = snprintf(buffer, sizeof(buffer),
                      "HTTP/1.1 303 See Other\r\n"
                      "Location: http://%s:%s%s\r\n"
                      "Content-Length: 0\r\n\r\n",
                      ip, port, uri);
    send_http_response(conn, buffer, len);
}

static void send_service_unavailable(int conn) {
    const char *response = "HTTP/1.1 503 Service Unavailable\r\n"
                          "Retry-After: 1\r\n"
                          "Content-Length: 0\r\n\r\n";
    send_http_response(conn, response, strlen(response));
}

static void handle_get_request(int conn, const char *uri, size_t *offset, char *reply) {
    size_t resource_length;
    const char *resource = get(uri, resources, MAX_RESOURCES, &resource_length);

    if (resource) {
        size_t payload_offset = sprintf(reply, 
            "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", resource_length);
        memcpy(reply + payload_offset, resource, resource_length);
        *offset = payload_offset + resource_length;
    } else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        strcpy(reply, not_found);
        *offset = strlen(not_found);
    }
}

static void handle_put_request(int conn, const char *uri, const char *payload, 
                             size_t payload_length, size_t *offset, char *reply) {
    fprintf(stderr, "PUT request for URI: %s, payload length: %zu\n", uri, payload_length);
    fprintf(stderr, "Payload content: %.*s\n", (int)payload_length, payload);

    bool updated = set(uri, (char *)payload, payload_length, resources, MAX_RESOURCES);
    const char *response;
    if (updated) {
        response = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    } else {
        response = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
    }
    strcpy(reply, response);
    *offset = strlen(response);

    fprintf(stderr, "PUT request completed. Updated: %d\n", updated);
}

static void handle_delete_request(int conn, const char *uri, size_t *offset, char *reply) {
    bool deleted = remove_tuple(uri, resources, MAX_RESOURCES);
    const char *response = deleted ?
        "HTTP/1.1 204 No Content\r\n\r\n" :
        "HTTP/1.1 404 Not Found\r\n\r\n";
    strcpy(reply, response);
    *offset = strlen(response);
}

void send_reply(int conn, struct request *request, int udp_socket) {
    char buffer[HTTP_MAX_SIZE];
    char *reply = buffer;
    size_t offset = 0;

    fprintf(stderr, "Handling %s request for %s (%lu byte payload)\n",
            request->method, request->uri, request->payload_length);

    uint16_t uri_hash = pseudo_hash((unsigned char *)request->uri, strlen(request->uri));

    // DHT PART

    if (!is_responsible(uri_hash, dht.self_id, dht.pred_id)) {
        fprintf(stderr, "Not responsible for hash 0x%04x, forwarding to successor\n", uri_hash);
        send_dht_lookup(udp_socket, &dht, uri_hash);
        send_service_unavailable(conn);
        return;
    }

    // WEBSERVER PART

    fprintf(stderr, "Responsible for hash 0x%04x\n", uri_hash);

    if (strcmp(request->method, "GET") == 0) {
        handle_get_request(conn, request->uri, &offset, reply);
    } else if (strcmp(request->method, "PUT") == 0) {
        handle_put_request(conn, request->uri, request->payload, 
                          request->payload_length, &offset, reply);
    } else if (strcmp(request->method, "DELETE") == 0) {
        handle_delete_request(conn, request->uri, &offset, reply);
    } else {
        reply = "HTTP/1.1 501 Method Not Supported\r\n\r\n";
        offset = strlen(reply);
    }

    send_http_response(conn, reply, offset);

    fprintf(stderr, "URI hash: 0x%04x, self_id: 0x%04x, pred_id: 0x%04x\n",
            uri_hash, dht.self_id, dht.pred_id);
    fprintf(stderr, "Is responsible: %d\n",
            is_responsible(uri_hash, dht.self_id, dht.pred_id));
}

static bool should_close_connection(struct request *request) {
    const string connection_header = get_header(request, "Connection");
    return connection_header && strcmp(connection_header, "close") == 0;
}

size_t process_packet(int conn, char *buffer, size_t n, int udp_socket) {
    struct request request = {0};
    ssize_t bytes_processed = parse_request(buffer, n, &request);

    if (bytes_processed > 0) {
        send_reply(conn, &request, udp_socket);
        return should_close_connection(&request) ? -1 : bytes_processed;
    }

    if (bytes_processed == -1) {
        const string bad_request = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send_http_response(conn, bad_request, strlen(bad_request));
        printf("Received malformed request, terminating connection.\n");
        close(conn);
        return -1;
    }

    return bytes_processed;
}

static void connection_setup(struct connection_state *state, int sock) {
    state->sock = sock;
    state->end = state->buffer;
    memset(state->buffer, 0, HTTP_MAX_SIZE);
}

char *buffer_discard(char *buffer, size_t discard, size_t keep) {
    memmove(buffer, buffer + discard, keep);
    memset(buffer + keep, 0, discard);
    return buffer + keep;
}

static bool handle_incoming_data(struct connection_state *state, int udp_socket) {
    const char *buffer_end = state->buffer + HTTP_MAX_SIZE;
    ssize_t bytes_read = recv(state->sock, state->end, buffer_end - state->end, 0);

    if (bytes_read == -1) {
        perror("recv");
        close(state->sock);
        exit(EXIT_FAILURE);
    }
    if (bytes_read == 0) return false;

    char *window_start = state->buffer;
    char *window_end = state->end + bytes_read;

    ssize_t bytes_processed;
    while ((bytes_processed = process_packet(state->sock, window_start,
                                           window_end - window_start, udp_socket)) > 0) {
        window_start += bytes_processed;
    }
    if (bytes_processed == -1) return false;

    state->end = buffer_discard(state->buffer, window_start - state->buffer,
                               window_end - window_start);
    return true;
}

bool handle_connection(struct connection_state *state, int udp_socket) {
    return handle_incoming_data(state, udp_socket);
}

static int setup_socket_common(int sock) {
    if (sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    const int enable = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    return sock;
}

static void bind_socket(int sock, struct sockaddr_in addr) {
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sock);
        exit(EXIT_FAILURE);
    }
}

static int setup_server_socket(struct sockaddr_in addr) {
    int sock = setup_socket_common(socket(AF_INET, SOCK_STREAM, 0));

    bind_socket(sock, addr);

    if (listen(sock, 1) == -1) {
        perror("listen");
        close(sock);
        exit(EXIT_FAILURE);
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        close(sock);
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Setting up TCP socket on port %d\n", ntohs(addr.sin_port));
    return sock;
}

static int setup_udp_socket(struct sockaddr_in addr) {
    int sock = setup_socket_common(socket(AF_INET, SOCK_DGRAM, 0));

    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

    bind_socket(sock, addr);

    fprintf(stderr, "Setting up UDP socket on port %d\n", ntohs(addr.sin_port));
    return sock;
}

static void handle_server_socket(int server_socket, struct pollfd *sockets,
                               struct connection_state *state) {
    int connection = accept(server_socket, NULL, NULL);
    if (connection == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
            close(server_socket);
            exit(EXIT_FAILURE);
        }
        return;
    }

    connection_setup(state, connection);
    sockets[0].events = POLLIN;
    sockets[1].fd = connection;
    sockets[1].events = POLLIN;
}

static void handle_client_socket(struct connection_state *state,
                               struct pollfd *sockets, int udp_socket) {
    bool cont = handle_connection(state, udp_socket);

    // if connection should end, 
    // 1. reset the server to listen for new connections
    // 2. mark the connection as inactive
    // 3. stop monitoring events
    if (!cont) {
        sockets[0].events = POLLIN;
        sockets[1].fd = -1;
        sockets[1].events = 0;
    }
}
// this is where everything starts
int main(int argc, char **argv) {
    if (argc < 3) return EXIT_FAILURE;

    init_dht_state(&dht, argc, argv);

    struct sockaddr_in addr = derive_sockaddr(argv[1], argv[2]);

    int server_socket = setup_server_socket(addr);
    int udp_socket = setup_udp_socket(addr);

    print_dht_info(&dht);

    struct pollfd sockets[3] = {
        {.fd = server_socket, .events = POLLIN}, // index 0: tcp server socket
        {.fd = -1, .events = 0}, // index 1: future client socket
        {.fd = udp_socket, .events = POLLIN} // index 2: udp socket for DHT
    };

    struct connection_state state = {0};

    while (true) {
        int ready = poll(sockets, sizeof(sockets) / sizeof(sockets[0]), -1);
        if (ready == -1) {
            perror("poll");
            exit(EXIT_FAILURE);
        }

        for (size_t i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
            if (sockets[i].revents != POLLIN) continue;

            int s = sockets[i].fd;
            if (s == server_socket) {
                handle_server_socket(server_socket, sockets, &state);
            } else if (s == state.sock) {
                handle_client_socket(&state, sockets, udp_socket);
            }
        }
    }

    return EXIT_SUCCESS;
}
