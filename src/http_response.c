#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "http.h"
#include "data.h"
#include "http_response.h"

void send_http_response(int conn, const char *response, size_t length) {
    if (send(conn, response, length, 0) == -1) {
        perror("send");
        close(conn);
    }
}

void send_redirect(int conn, const char *ip, const char *port, const char *uri) {
    char buffer[HTTP_MAX_SIZE];
    int len = snprintf(buffer, sizeof(buffer),
                      "HTTP/1.1 303 See Other\r\n"
                      "Location: http://%s:%s%s\r\n"
                      "Content-Length: 0\r\n\r\n",
                      ip, port, uri);
    send_http_response(conn, buffer, len);
}

void send_service_unavailable(int conn) {
    const char *response =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Retry-After: 1\r\n"
        "Content-Length: 0\r\n\r\n";
    send_http_response(conn, response, strlen(response));
}

void handle_get_request(int conn, const char *uri, size_t *offset, char *reply) {
    size_t resource_length;
    const char *resource = get(uri, resources, MAX_RESOURCES, &resource_length);

    if (resource) {
        size_t payload_offset =
            sprintf(reply, "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n",
                    resource_length);
        memcpy(reply + payload_offset, resource, resource_length);
        *offset = payload_offset + resource_length;
    } else {
        const char *not_found =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        strcpy(reply, not_found);
        *offset = strlen(not_found);
    }
}

void handle_put_request(int conn, const char *uri, const char *payload,
                       size_t payload_length, size_t *offset, char *reply) {
    fprintf(stderr, "PUT request for URI: %s, payload length: %zu\n", uri,
            payload_length);
    fprintf(stderr, "Payload content: %.*s\n", (int)payload_length, payload);

    bool updated =
        set(uri, (char *)payload, payload_length, resources, MAX_RESOURCES);
    const char *response;
    if (updated) {
        response = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    } else {
        response = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
    }
    strcpy(reply, response);
    *offset = strlen(response);

    fprintf(stderr, "PUT request completed. Updated: %d\n", updated);
}

void handle_delete_request(int conn, const char *uri, size_t *offset, char *reply) {
    bool deleted = remove_tuple(uri, resources, MAX_RESOURCES);
    const char *response = deleted ? "HTTP/1.1 204 No Content\r\n\r\n"
                                 : "HTTP/1.1 404 Not Found\r\n\r\n";
    strcpy(reply, response);
    *offset = strlen(response);
} 