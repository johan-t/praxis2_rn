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
#include "http_response.h"
#include "socket_handler.h"
#include "dht_handler.h"

struct dht_state dht = {0};
struct tuple resources[MAX_RESOURCES] = {
    {"/static/foo", "Foo", sizeof "Foo" - 1},
    {"/static/bar", "Bar", sizeof "Bar" - 1},
    {"/static/baz", "Baz", sizeof "Baz" - 1}
};

int main(int argc, char **argv) {
    if (argc < 3) return EXIT_FAILURE;

    init_dht_state(&dht, argc, argv);

    struct sockaddr_in addr = derive_sockaddr(argv[1], argv[2]);

    int server_socket = setup_server_socket(addr);
    int udp_socket = setup_udp_socket(addr);

    print_dht_info(&dht);

    struct pollfd sockets[3] = {
        {.fd = server_socket, .events = POLLIN},  // index 0: tcp server socket
        {.fd = -1, .events = 0},              // index 1: future client socket
        {.fd = udp_socket, .events = POLLIN}  // index 2: udp socket for DHT
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
            } else if (s == udp_socket) {
                struct sockaddr_in sender;
                socklen_t sender_len = sizeof(sender);
                struct dht_message msg;
                ssize_t bytes_read = recvfrom(udp_socket, &msg, sizeof(msg), 0,
                                          (struct sockaddr *)&sender, &sender_len);
                if (bytes_read > 0) {
                    handle_dht_message(udp_socket, &msg, &sender, &dht);
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
