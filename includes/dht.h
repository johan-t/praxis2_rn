#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#define MESSAGE_TYPE_LOOKUP 0
#define MESSAGE_TYPE_REPLY 1

#define MESSAGE_FORMAT_SIZE 12


struct dht_message {
    uint8_t type;
    uint16_t hash;
    uint16_t node_id;
    uint32_t node_ip;
    uint16_t node_port;
} __attribute__((packed));


struct dht_state {
    uint16_t self_id;
    const char *self_ip;
    uint16_t self_port;

    uint16_t pred_id;
    const char *pred_ip;
    const char *pred_port;

    uint16_t succ_id;
    const char *succ_ip;
    const char *succ_port;
};


/**
 * Initialize DHT state from command line arguments and environment variables
 */
void init_dht_state(struct dht_state *dht, int argc, char **argv);

/**
 * Print DHT server information to stderr
 */
void print_dht_info(const struct dht_state *dht);

/**
 * Determines if this node is responsible for the given hash
 */
bool is_responsible(uint16_t hash, uint16_t self_id, uint16_t pred_id);

/**
 * Send a lookup message to the successor node
 */
void send_dht_lookup(int udp_socket, const struct dht_state *dht, uint16_t hash);

/**
 * Send a reply message to the successor node
 */
void send_dht_reply(int udp_socket, const struct dht_state *dht, uint16_t responsible_node_id, const char * responsible_node_ip, uint16_t responsible_node_port, uint16_t hash);

/**
 * Convert host and port to sockaddr_in structure
 */
struct sockaddr_in derive_sockaddr(const char *host, const char *port);
