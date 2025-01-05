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
#include "http.h"
#include "util.h"

#define MAX_RESOURCES 100
#define MESSAGE_TYPE_LOOKUP 0
#define MESSAGE_FORMAT_SIZE                                                    \
  12 // Size of message format (type + hash + node info)

struct tuple resources[MAX_RESOURCES] = {
    {"/static/foo", "Foo", sizeof "Foo" - 1},
    {"/static/bar", "Bar", sizeof "Bar" - 1},
    {"/static/baz", "Baz", sizeof "Baz" - 1}};

struct dht_message {
  uint8_t type;
  uint16_t hash;
  uint16_t node_id;
  uint32_t node_ip; // IPv4 address
  uint16_t node_port;
} __attribute__((packed));

struct dht_state {
  uint16_t self_id;
  uint16_t pred_id;
  uint16_t succ_id;
  const char *self_ip;
  uint16_t self_port;
  const char *succ_ip;
  const char *succ_port;
} dht = {0};

static void send_lookup(int udp_socket, uint16_t hash, const char *succ_ip,
                        const char *succ_port);
static void send_service_unavailable(int conn);

/**
 * Determines if this node is responsible for the given hash
 *
 * @param hash The hash to check
 * @param self_id The ID of this node
 * @param pred_id The ID of the predecessor node
 * @return true if this node is responsible, false otherwise
 */
static bool is_responsible(uint16_t hash, uint16_t self_id, uint16_t pred_id) {
  if (pred_id < self_id) {
    return hash > pred_id && hash <= self_id;
  } else {
    return (hash > pred_id) || (hash <= self_id);
  }
}

/**
 * Sends a redirect response to the client
 *
 * @param conn The connection socket
 * @param ip The IP to redirect to
 * @param port The port to redirect to
 * @param uri The URI of the resource
 */
static void send_redirect(int conn, const char *ip, const char *port,
                          const char *uri) {
  char buffer[HTTP_MAX_SIZE];
  int len = snprintf(buffer, sizeof(buffer),
                     "HTTP/1.1 303 See Other\r\n"
                     "Location: http://%s:%s%s\r\n"
                     "Content-Length: 0\r\n\r\n",
                     ip, port, uri);

  if (send(conn, buffer, len, 0) == -1) {
    perror("send");
    close(conn);
  }
}

/**
 * Sends an HTTP reply to the client based on the received request.
 *
 * @param conn      The file descriptor of the client connection socket.
 * @param request   A pointer to the struct containing the parsed request
 * information.
 */
void send_reply(int conn, struct request *request, int udp_socket) {
  char buffer[HTTP_MAX_SIZE];
  char *reply = buffer;
  size_t offset = 0;

  fprintf(stderr, "Handling %s request for %s (%lu byte payload)\n",
          request->method, request->uri, request->payload_length);

  // Calculate hash of the URI
  uint16_t uri_hash =
      pseudo_hash((unsigned char *)request->uri, strlen(request->uri));

  // Check if we're responsible for this hash
  if (is_responsible(uri_hash, dht.self_id, dht.pred_id)) {
    // Handle the request locally as before
    if (strcmp(request->method, "GET") == 0) {
      // Find the resource with the given URI in the 'resources' array.
      size_t resource_length;
      const char *resource =
          get(request->uri, resources, MAX_RESOURCES, &resource_length);

      if (resource) {
        size_t payload_offset =
            sprintf(reply, "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n",
                    resource_length);
        memcpy(reply + payload_offset, resource, resource_length);
        offset = payload_offset + resource_length;
      } else {
        reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        offset = strlen(reply);
      }
    } else if (strcmp(request->method, "PUT") == 0) {
      // Try to set the requested resource with the given payload in the
      // 'resources' array.
      if (set(request->uri, request->payload, request->payload_length,
              resources, MAX_RESOURCES)) {
        reply = "HTTP/1.1 204 No Content\r\n\r\n";
      } else {
        reply = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
      }
      offset = strlen(reply);
    } else if (strcmp(request->method, "DELETE") == 0) {
      // Try to delete the requested resource from the 'resources' array
      if (delete (request->uri, resources, MAX_RESOURCES)) {
        reply = "HTTP/1.1 204 No Content\r\n\r\n";
      } else {
        reply = "HTTP/1.1 404 Not Found\r\n\r\n";
      }
      offset = strlen(reply);
    } else {
      reply = "HTTP/1.1 501 Method Not Supported\r\n\r\n";
      offset = strlen(reply);
    }
  } else {
    // Send lookup to successor
    send_lookup(udp_socket, uri_hash, dht.succ_ip, dht.succ_port);
    // Send 503 to client
    send_service_unavailable(conn);
    return;
  }

  // Send the reply back to the client
  if (send(conn, reply, offset, 0) == -1) {
    perror("send");
    close(conn);
  }

  fprintf(stderr, "URI hash: 0x%04x, self_id: 0x%04x, pred_id: 0x%04x\n",
          uri_hash, dht.self_id, dht.pred_id);
  fprintf(stderr, "Is responsible: %d\n",
          is_responsible(uri_hash, dht.self_id, dht.pred_id));
}

/**
 * Processes an incoming packet from the client.
 *
 * @param conn The socket descriptor representing the connection to the client.
 * @param buffer A pointer to the incoming packet's buffer.
 * @param n The size of the incoming packet.
 *
 * @return Returns the number of bytes processed from the packet.
 *         If the packet is successfully processed and a reply is sent, the
 * return value indicates the number of bytes processed. If the packet is
 * malformed or an error occurs during processing, the return value is -1.
 *
 */
size_t process_packet(int conn, char *buffer, size_t n, int udp_socket) {
  struct request request = {
      .method = NULL, .uri = NULL, .payload = NULL, .payload_length = -1};
  ssize_t bytes_processed = parse_request(buffer, n, &request);

  if (bytes_processed > 0) {
    send_reply(conn, &request, udp_socket);

    // Check the "Connection" header in the request to determine if the
    // connection should be kept alive or closed.
    const string connection_header = get_header(&request, "Connection");
    if (connection_header && strcmp(connection_header, "close")) {
      return -1;
    }
  } else if (bytes_processed == -1) {
    // If the request is malformed or an error occurs during processing,
    // send a 400 Bad Request response to the client.
    const string bad_request = "HTTP/1.1 400 Bad Request\r\n\r\n";
    send(conn, bad_request, strlen(bad_request), 0);
    printf("Received malformed request, terminating connection.\n");
    close(conn);
    return -1;
  }

  return bytes_processed;
}

/**
 * Sets up the connection state for a new socket connection.
 *
 * @param state A pointer to the connection_state structure to be initialized.
 * @param sock The socket descriptor representing the new connection.
 *
 */
static void connection_setup(struct connection_state *state, int sock) {
  // Set the socket descriptor for the new connection in the connection_state
  // structure.
  state->sock = sock;

  // Set the 'end' pointer of the state to the beginning of the buffer.
  state->end = state->buffer;

  // Clear the buffer by filling it with zeros to avoid any stale data.
  memset(state->buffer, 0, HTTP_MAX_SIZE);
}

/**
 * Discards the front of a buffer
 *
 * @param buffer A pointer to the buffer to be modified.
 * @param discard The number of bytes to drop from the front of the buffer.
 * @param keep The number of bytes that should be kept after the discarded
 * bytes.
 *
 * @return Returns a pointer to the first unused byte in the buffer after the
 * discard.
 * @example buffer_discard(ABCDEF0000, 4, 2):
 *          ABCDEF0000 ->  EFCDEF0000 -> EF00000000, returns pointer to first 0.
 */
char *buffer_discard(char *buffer, size_t discard, size_t keep) {
  memmove(buffer, buffer + discard, keep);
  memset(buffer + keep, 0, discard); // invalidate buffer
  return buffer + keep;
}

/**
 * Handles incoming connections and processes data received over the socket.
 *
 * @param state A pointer to the connection_state structure containing the
 * connection state.
 * @return Returns true if the connection and data processing were successful,
 * false otherwise. If an error occurs while receiving data from the socket, the
 * function exits the program.
 */
bool handle_connection(struct connection_state *state, int udp_socket) {
  // Calculate the pointer to the end of the buffer to avoid buffer overflow
  const char *buffer_end = state->buffer + HTTP_MAX_SIZE;

  // Check if an error occurred while receiving data from the socket
  ssize_t bytes_read =
      recv(state->sock, state->end, buffer_end - state->end, 0);
  if (bytes_read == -1) {
    perror("recv");
    close(state->sock);
    exit(EXIT_FAILURE);
  } else if (bytes_read == 0) {
    return false;
  }

  char *window_start = state->buffer;
  char *window_end = state->end + bytes_read;

  ssize_t bytes_processed = 0;
  while ((bytes_processed =
              process_packet(state->sock, window_start,
                             window_end - window_start, udp_socket)) > 0) {
    window_start += bytes_processed;
  }
  if (bytes_processed == -1) {
    return false;
  }

  state->end = buffer_discard(state->buffer, window_start - state->buffer,
                              window_end - window_start);
  return true;
}

/**
 * Derives a sockaddr_in structure from the provided host and port information.
 *
 * @param host The host (IP address or hostname) to be resolved into a network
 * address.
 * @param port The port number to be converted into network byte order.
 *
 * @return A sockaddr_in structure representing the network address derived from
 * the host and port.
 */
static struct sockaddr_in derive_sockaddr(const char *host, const char *port) {
  struct addrinfo hints = {
      .ai_family = AF_INET,
  };
  struct addrinfo *result_info;

  // Resolve the host (IP address or hostname) into a list of possible
  // addresses.
  int returncode = getaddrinfo(host, port, &hints, &result_info);
  if (returncode) {
    fprintf(stderr, "Error parsing host/port");
    exit(EXIT_FAILURE);
  }

  // Copy the sockaddr_in structure from the first address in the list
  struct sockaddr_in result = *((struct sockaddr_in *)result_info->ai_addr);

  // Free the allocated memory for the result_info
  freeaddrinfo(result_info);
  return result;
}

/**
 * Sets up a TCP server socket and binds it to the provided sockaddr_in address.
 *
 * @param addr The sockaddr_in structure representing the IP address and port of
 * the server.
 *
 * @return The file descriptor of the created TCP server socket.
 */
static int setup_server_socket(struct sockaddr_in addr) {
  const int enable = 1;
  const int backlog = 1;

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) ==
      -1) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  // Bind first
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("bind");
    close(sock);
    exit(EXIT_FAILURE);
  }

  // Listen second
  if (listen(sock, backlog) == -1) {
    perror("listen");
    close(sock);
    exit(EXIT_FAILURE);
  }

  // Set non-blocking last
  if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
    perror("fcntl");
    close(sock);
    exit(EXIT_FAILURE);
  }

  fprintf(stderr, "Setting up TCP socket on port %d\n", ntohs(addr.sin_port));

  return sock;
}

/**
 * Sets up a UDP socket and binds it to the provided sockaddr_in address.
 *
 * @param addr The sockaddr_in structure representing the IP address and port of
 * the server.
 *
 * @return The file descriptor of the created UDP server socket.
 */
static int setup_udp_socket(struct sockaddr_in addr) {
  // Create UDP socket
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock == -1) {
    perror("UDP socket");
    exit(EXIT_FAILURE);
  }

  // Set socket to non-blocking mode
  if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
    perror("fcntl");
    exit(EXIT_FAILURE);
  }

  // Bind socket to the provided address
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("UDP bind");
    close(sock);
    exit(EXIT_FAILURE);
  }

  fprintf(stderr, "Setting up UDP socket on port %d\n", ntohs(addr.sin_port));

  return sock;
}

/**
 * Sends a lookup message
 *
 * @param udp_socket The UDP socket
 * @param hash The hash of the resource
 * @param succ_ip The IP of the successor
 * @param succ_port The port of the successor
 */
static void send_lookup(int udp_socket, uint16_t hash, const char *succ_ip,
                        const char *succ_port) {
  struct sockaddr_in addr = derive_sockaddr(succ_ip, succ_port);

  struct dht_message msg = {
      .type = MESSAGE_TYPE_LOOKUP,
      .hash = htons(hash),           // Convert to network byte order
      .node_id = htons(dht.self_id), // Convert to network byte order
      .node_ip = inet_addr(dht.self_ip),
      .node_port = htons(dht.self_port)};

  if (sendto(udp_socket, &msg, sizeof(msg), 0, (struct sockaddr *)&addr,
             sizeof(addr)) == -1) {
    perror("sendto");
  }
}

/**
 * Sends a 503 response
 *
 * @param conn The connection socket
 */
static void send_service_unavailable(int conn) {
  const char *response = "HTTP/1.1 503 Service Unavailable\r\n"
                         "Retry-After: 1\r\n"
                         "Content-Length: 0\r\n\r\n";

  if (send(conn, response, strlen(response), 0) == -1) {
    perror("send");
    close(conn);
  }
}

/**
 *  The program expects 3; otherwise, it returns EXIT_FAILURE.
 *
 *  Call as:
 *
 *  ./build/webserver self.ip self.port
 */
int main(int argc, char **argv) {
  if (argc < 3) {
    return EXIT_FAILURE;
  }

  // Get own ID (defaults to 0 if not provided)
  dht.self_id = (argc > 3) ? strtoul(argv[3], NULL, 10) : 0;

  // Get predecessor info from environment
  const char *pred_id_str = getenv("PRED_ID");
  const char *pred_ip = getenv("PRED_IP");
  const char *pred_port = getenv("PRED_PORT");
  dht.pred_id = pred_id_str ? strtoul(pred_id_str, NULL, 10) : dht.self_id;

  // Get successor info from environment
  const char *succ_id_str = getenv("SUCC_ID");
  dht.succ_ip = getenv("SUCC_IP");
  dht.succ_port = getenv("SUCC_PORT");
  dht.succ_id = succ_id_str ? strtoul(succ_id_str, NULL, 10) : dht.self_id;

  dht.self_ip = argv[1];
  dht.self_port = atoi(argv[2]);

  struct sockaddr_in addr = derive_sockaddr(argv[1], argv[2]);

  // Set up server sockets
  int server_socket = setup_server_socket(addr);
  int udp_socket = setup_udp_socket(addr);

  fprintf(stderr, "Server starting with:\n");
  fprintf(stderr, "Self ID: 0x%04x, IP: %s, Port: %d\n", dht.self_id,
          dht.self_ip, dht.self_port);
  fprintf(stderr, "Pred ID: 0x%04x\n", dht.pred_id);
  fprintf(stderr, "Succ ID: 0x%04x, IP: %s, Port: %s\n", dht.succ_id,
          dht.succ_ip, dht.succ_port);

  struct pollfd sockets[3] = {{.fd = server_socket, .events = POLLIN},
                              {.fd = -1, .events = 0},
                              {.fd = udp_socket, .events = POLLIN}};

  struct connection_state state = {0};
  while (true) {
    int ready = poll(sockets, sizeof(sockets) / sizeof(sockets[0]), -1);
    if (ready == -1) {
      perror("poll");
      exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i += 1) {
      if (sockets[i].revents != POLLIN) {
        continue;
      }
      int s = sockets[i].fd;

      if (s == server_socket) {
        int connection = accept(server_socket, NULL, NULL);
        if (connection == -1) {
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
            close(server_socket);
            exit(EXIT_FAILURE);
          }
        } else {
          connection_setup(&state, connection);
          sockets[0].events = POLLIN;
          sockets[1].fd = connection;
          sockets[1].events = POLLIN;
        }
      } else if (s == state.sock) {
        bool cont = handle_connection(&state, udp_socket);
        if (!cont) {
          sockets[0].events = POLLIN;
          sockets[1].fd = -1;
          sockets[1].events = 0;
        }
      }
    }
  }

  return EXIT_SUCCESS;
}
