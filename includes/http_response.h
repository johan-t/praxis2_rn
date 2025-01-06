#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <stdbool.h>
#include "data.h"

#define MAX_RESOURCES 100

extern struct tuple resources[MAX_RESOURCES];

void send_http_response(int conn, const char *response, size_t length);
void send_redirect(int conn, const char *ip, const char *port, const char *uri);
void send_service_unavailable(int conn);
void handle_get_request(int conn, const char *uri, size_t *offset, char *reply);
void handle_put_request(int conn, const char *uri, const char *payload,
                       size_t payload_length, size_t *offset, char *reply);
void handle_delete_request(int conn, const char *uri, size_t *offset, char *reply);

#endif // HTTP_RESPONSE_H 