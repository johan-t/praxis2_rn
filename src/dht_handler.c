#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include "dht.h"
#include "dht_handler.h"

static struct {
    bool received;
    uint16_t responsible_id;
    const char *responsible_ip;
    uint16_t responsible_port;
} last_dht_reply = {0};

void handle_dht_message(int udp_socket, const struct dht_message *msg, 
                       const struct sockaddr_in *sender, struct dht_state *dht) {
    uint16_t hash = ntohs(msg->hash);
    uint16_t node_id = ntohs(msg->node_id);
    uint16_t sender_port = ntohs(sender->sin_port);

    char sender_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(sender->sin_addr), sender_ip, INET_ADDRSTRLEN);

    if (msg->type == MESSAGE_TYPE_LOOKUP) {
        fprintf(stderr, "(%s:%d) Received lookup for hash 0x%04x from %s:%d\n", 
                dht->self_ip, dht->self_port, hash, sender_ip, sender_port);
        
        // Check if our successor is responsible for the hash
        if (is_responsible(hash, dht->succ_id, dht->self_id)) {
            fprintf(stderr, "(%s:%d) Our successor is responsible for hash 0x%04x\n", 
                    dht->self_ip, dht->self_port, hash);
            send_dht_reply(udp_socket, dht, dht->succ_id, sender_ip, sender_port, dht->self_id);
        }
        // Check if we are responsible for the hash
        else if (is_responsible(hash, dht->self_id, dht->pred_id)) {
            fprintf(stderr, "(%s:%d) We are responsible for hash 0x%04x\n", 
                    dht->self_ip, dht->self_port, hash);
            send_dht_reply(udp_socket, dht, dht->self_id, sender_ip, sender_port, dht->pred_id);
        }
        // Neither we nor our successor is responsible
        else {
            fprintf(stderr, "(%s:%d) Forwarding lookup for hash 0x%04x to successor: %s:%s\n", 
                    dht->self_ip, dht->self_port, hash, dht->succ_ip, dht->succ_port);
            struct sockaddr_in succ_addr;
            succ_addr.sin_family = AF_INET;
            succ_addr.sin_port = htons(atoi(dht->succ_port));
            succ_addr.sin_addr.s_addr = inet_addr(dht->succ_ip);
            
            if (sendto(udp_socket, msg, sizeof(*msg), 0, 
                      (struct sockaddr *)&succ_addr, sizeof(succ_addr)) == -1) {
                perror("sendto");
            }
        }
    } else if (msg->type == MESSAGE_TYPE_REPLY) {
        fprintf(stderr, "(%s:%d) Received DHT reply from %s:%d: responsible=%04x, predecessor=%04x\n",
                dht->self_ip, dht->self_port, sender_ip, sender_port, node_id, hash);
        last_dht_reply.received = true;
        last_dht_reply.responsible_id = node_id;
        last_dht_reply.responsible_ip = inet_ntoa(*(struct in_addr *)&msg->node_ip);
        last_dht_reply.responsible_port = ntohs(msg->node_port);
    }
}

bool get_last_dht_reply(uint16_t *id, const char **ip, uint16_t *port) {
    if (!last_dht_reply.received) {
        return false;
    }
    
    *id = last_dht_reply.responsible_id;
    *ip = last_dht_reply.responsible_ip;
    *port = last_dht_reply.responsible_port;
    
    last_dht_reply.received = false;  // Clear the reply
    return true;
} 