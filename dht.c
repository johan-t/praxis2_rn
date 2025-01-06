#include "dht.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool is_responsible(uint16_t hash, uint16_t self_id, uint16_t pred_id) {
    if (pred_id < self_id) {
        return hash > pred_id && hash <= self_id;
    }
    return (hash > pred_id) || (hash <= self_id);
}

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


void init_dht_state(struct dht_state *dht, int argc, char **argv) {
    dht->self_id = (argc > 3) ? strtoul(argv[3], NULL, 10) : 0;

    const char *pred_id_str = getenv("PRED_ID");
    dht->pred_id = pred_id_str ? strtoul(pred_id_str, NULL, 10) : dht->self_id;

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
