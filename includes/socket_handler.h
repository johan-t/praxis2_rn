#ifndef SOCKET_HANDLER_H
#define SOCKET_HANDLER_H

#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include "http.h"

int setup_socket_common(int sock);
int setup_server_socket(struct sockaddr_in addr);
int setup_udp_socket(struct sockaddr_in addr);
void connection_setup(struct connection_state *state, int sock);
char *buffer_discard(char *buffer, size_t discard, size_t keep);
void handle_server_socket(int server_socket, struct pollfd *sockets,
                         struct connection_state *state);
void handle_client_socket(struct connection_state *state,
                         struct pollfd *sockets, int udp_socket);
bool handle_connection(struct connection_state *state, int udp_socket);
bool handle_incoming_data(struct connection_state *state, int udp_socket);
size_t process_packet(int conn, char *buffer, size_t n, int udp_socket);
void send_reply(int conn, struct request *request, int udp_socket);

#endif // SOCKET_HANDLER_H 