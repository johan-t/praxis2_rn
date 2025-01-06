#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "socket_handler.h"
#include "http.h"
#include "dht_handler.h"
#include "http_response.h"
#include "dht.h"
#include "util.h"

extern struct dht_state dht;

static bool should_close_connection(struct request *request) {
    const string connection_header = get_header(request, "Connection");
    return connection_header && strcmp(connection_header, "close") == 0;
}

void send_reply(int conn, struct request *request, int udp_socket) {
    char buffer[HTTP_MAX_SIZE];
    char *reply = buffer;
    size_t offset = 0;

    fprintf(stderr, "Handling %s request for %s (%lu byte payload)\n",
            request->method, request->uri, request->payload_length);

    uint16_t uri_hash =
        pseudo_hash((unsigned char *)request->uri, strlen(request->uri));

    // DHT PART
    // check if we are responsible
    if (is_responsible(uri_hash, dht.self_id, dht.pred_id)) {
        fprintf(stderr, "Responsible for hash 0x%04x\n", uri_hash);

        // If it's a GET request and the resource doesn't exist, return 404
        if (strcmp(request->method, "GET") == 0) {
            size_t resource_length;
            const char *resource = get(request->uri, resources, MAX_RESOURCES, &resource_length);
            if (!resource) {
                const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                send_http_response(conn, not_found, strlen(not_found));
                return;
            }
        }
    
    // check if our successor is responsible
    } else if (is_responsible(uri_hash, dht.succ_id, dht.self_id)) {
        // Our successor is responsible, redirect to it
        fprintf(stderr, "Successor is responsible for hash 0x%04x, redirecting\n", uri_hash);
        send_redirect(conn, dht.succ_ip, dht.succ_port, request->uri);
        return;
    } else {
        // Check if we have received a DHT reply
        uint16_t responsible_id;
        const char *responsible_ip;
        uint16_t responsible_port;
        
        if (get_last_dht_reply(&responsible_id, &responsible_ip, &responsible_port)) {
            // We have received a reply, redirect to the responsible node
            fprintf(stderr, "Received reply for hash 0x%04x, redirecting\n", uri_hash);
            char port_str[6];
            snprintf(port_str, sizeof(port_str), "%d", responsible_port);
            send_redirect(conn, responsible_ip, port_str, request->uri);
            return;
        } else {
            // Neither we nor our successor is responsible, forward lookup
            fprintf(stderr, "Not responsible for hash 0x%04x, forwarding to successor\n", uri_hash);
            send_dht_lookup(udp_socket, &dht, uri_hash);
            send_service_unavailable(conn);
            return;
        }
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

int setup_socket_common(int sock) {
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

int setup_server_socket(struct sockaddr_in addr) {
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

int setup_udp_socket(struct sockaddr_in addr) {
    int sock = setup_socket_common(socket(AF_INET, SOCK_DGRAM, 0));

    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        close(sock);
        exit(EXIT_FAILURE);
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sock);
        return -1;
    }

    fprintf(stderr, "Setting up UDP socket on port %d\n", ntohs(addr.sin_port));
    return sock;
}

void connection_setup(struct connection_state *state, int sock) {
    state->sock = sock;
    state->end = state->buffer;
    memset(state->buffer, 0, HTTP_MAX_SIZE);
}

char *buffer_discard(char *buffer, size_t discard, size_t keep) {
    memmove(buffer, buffer + discard, keep);
    memset(buffer + keep, 0, discard);
    return buffer + keep;
}

void handle_server_socket(int server_socket, struct pollfd *sockets,
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

void handle_client_socket(struct connection_state *state,
                         struct pollfd *sockets, int udp_socket) {
    bool cont = handle_connection(state, udp_socket);

    if (!cont) {
        sockets[0].events = POLLIN;
        sockets[1].fd = -1;
        sockets[1].events = 0;
    }
}

bool handle_connection(struct connection_state *state, int udp_socket) {
    return handle_incoming_data(state, udp_socket);
}

bool handle_incoming_data(struct connection_state *state, int udp_socket) {
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
