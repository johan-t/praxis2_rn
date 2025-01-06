#include "dht.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool is_responsible(uint16_t hash, uint16_t self_id, uint16_t pred_id) {
    // Normal case:
    // ex. pred_id=100, self_id=200
    // Responsible for hashes in range (100, 200]
    if (pred_id < self_id) {
        return hash > pred_id && hash <= self_id;
    }
    // Wraparound case:
    // ex. pred_id=60000, self_id=1000
    // Responsible for hashes in range (60000, 65535] and [0, 1000]
    return (hash > pred_id) || (hash <= self_id);
}

// AUFGABE 1.3
void send_dht_lookup(int udp_socket, const struct dht_state *dht, uint16_t hash) {
    struct sockaddr_in addr = derive_sockaddr(dht->succ_ip, dht->succ_port);
    struct dht_message msg = {
        .type = MESSAGE_TYPE_LOOKUP,
        .hash = htons(hash),
        .node_id = htons(dht->self_id),
        .node_ip = inet_addr(dht->self_ip),
        .node_port = htons(dht->self_port)
    };

    if (sendto(udp_socket, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("sendto");
    }
} 

// AUFGABE 1.4
//  Message type 1 ( reply )
// hash id ist die ID des Vorgängers der verantwortlichen Node
// Node Id, IP und Port, Beschreibung der verantwortlichen Node
// wir haben hier die verantwortliche Node als parameter, da vielleicht der nachfolgende knoten der verantwortliche ist
void send_dht_reply(int udp_socket, const struct dht_state *dht, 
                    uint16_t responsible_node_id, 
                    const char *requesting_node_ip, 
                    uint16_t requesting_node_port, 
                    uint16_t predecessor_id) {
    // Send to the node that made the lookup request
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(requesting_node_port);
    addr.sin_addr.s_addr = inet_addr(requesting_node_ip);

    // If we're sending a reply about our successor being responsible,
    // use the successor's IP and port
    const char *resp_ip = (responsible_node_id == dht->succ_id) ? dht->succ_ip : dht->self_ip;
    const char *resp_port = (responsible_node_id == dht->succ_id) ? dht->succ_port : NULL;
    uint16_t resp_port_num = resp_port ? atoi(resp_port) : dht->self_port;

    struct dht_message msg = {
        .type = MESSAGE_TYPE_REPLY,
        .hash = htons(predecessor_id),  // ID of the predecessor of the responsible node
        .node_id = htons(responsible_node_id),  // ID of the responsible node
        .node_ip = inet_addr(resp_ip),  // IP of the responsible node
        .node_port = htons(resp_port_num)  // Port of the responsible node
    };

    fprintf(stderr, "Sending reply to %s:%d: responsible=%04x, predecessor=%04x\n",
            requesting_node_ip, requesting_node_port, responsible_node_id, predecessor_id);

    if (sendto(udp_socket, &msg, sizeof(msg), 0, 
               (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("sendto");
    }
}

struct sockaddr_in derive_sockaddr(const char *host, const char *port) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
    };
    struct addrinfo *result_info;

    int returncode = getaddrinfo(host, port, &hints, &result_info);
    if (returncode) {
        fprintf(stderr, "Error parsing host/port");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in result = *((struct sockaddr_in *)result_info->ai_addr);
    freeaddrinfo(result_info);
    return result;
}

void init_dht_state(struct dht_state *dht, int argc, char **argv) {
    dht->self_id = (argc > 3) ? strtoul(argv[3], NULL, 10) : 0;

    const char *pred_id_str = getenv("PRED_ID");
    dht->pred_id = pred_id_str ? strtoul(pred_id_str, NULL, 10) : dht->self_id;

    // dht->pred_ip = getenv("PRED_IP");
    // dht->pred_port = getenv("PRED_PORT");

    const char *succ_id_str = getenv("SUCC_ID");
    dht->succ_ip = getenv("SUCC_IP");
    dht->succ_port = getenv("SUCC_PORT");
    dht->succ_id = succ_id_str ? strtoul(succ_id_str, NULL, 10) : dht->self_id;

    dht->self_ip = argv[1];
    dht->self_port = atoi(argv[2]);
}

void print_dht_info(const struct dht_state *dht) {
    fprintf(stderr, "Server starting with:\n");
    fprintf(stderr, "Self ID: 0x%04x, IP: %s, Port: %d\n",
            dht->self_id, dht->self_ip, dht->self_port);
    fprintf(stderr, "Pred ID: 0x%04x\n", dht->pred_id);
    fprintf(stderr, "Succ ID: 0x%04x, IP: %s, Port: %s\n",
            dht->succ_id, dht->succ_ip, dht->succ_port);
}

