#ifndef DHT_HANDLER_H
#define DHT_HANDLER_H

#include <netinet/in.h>
#include <stdbool.h>
#include "dht.h"

void handle_dht_message(int udp_socket, const struct dht_message *msg,
                       const struct sockaddr_in *sender, struct dht_state *dht);
bool get_last_dht_reply(uint16_t *id, const char **ip, uint16_t *port);

#endif // DHT_HANDLER_H 