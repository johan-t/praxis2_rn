/**
 * This file provides functions to parse and extract information from an HTTP
 * request, such as the request line, headers, and payload.
 */

#include "http.h"
#include "util.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Non null-terminated string
 */
struct non_string {
    char *start;
    size_t n;
};

/**
 * Parse the request line of an HTTP request
 *
 * Returns whether a valid request line (excluding line separator) is parsed.
 * On valid request lines, `method` and `uri` are populated with the
 * corresponding values, reusing `buffer`'s memory.
 */
static bool find_space_delimiter(char *buffer, const char *end, struct non_string *part) {
    char *pos = memchr(buffer, ' ', end - buffer);
    if (!pos) return false;
    
    part->start = buffer;
    part->n = pos - buffer;
    return true;
}

static bool parse_request_line(char *buffer, size_t n,
                             struct non_string *method,
                             struct non_string *uri) {
    const char *end = buffer + n;
    char *pos = buffer;

    // Parse method
    if (!find_space_delimiter(buffer, end, method)) return false;
    pos = buffer + method->n + 1;

    // Parse URI
    uri->start = pos;
    char *uri_end = memchr(pos, ' ', end - pos);
    if (!uri_end) return false;
    uri->n = uri_end - pos;

    return true;
}

/**
 * Parse HTTP header
 *
 * Returns whether a valid header (excluding line separator) is parsed. On
 * a valid header, `key` and `value` are populated with the corresponding
 * values, reusing `buffer`'s memory.
 */
static bool parse_header(char *buffer, size_t n, struct non_string *key,
                        struct non_string *value) {
    char *field_name_end = memchr(buffer, ':', n);
    if (!field_name_end) return false;

    key->start = buffer;
    key->n = field_name_end - buffer;
    
    // Skip the colon and any following whitespace
    char *value_start = field_name_end + 1;
    while (value_start < buffer + n && (*value_start == ' ' || *value_start == '\t')) {
        value_start++;
    }
    
    value->start = value_start;
    value->n = (buffer + n) - value_start;

    return true;
}

static bool parse_headers(char *pos, const char *end, const char *line_separator,
                         struct request *request, size_t *header_count,
                         char **next_pos) {
    struct non_string headers_array[HTTP_MAX_HEADERS][2] = {0}; // Array for key-value pairs

    char *line_end;
    while ((line_end = memstr(pos, end - pos, line_separator)) != pos) {
        if (*header_count >= HTTP_MAX_HEADERS) {
            fprintf(stderr, "Exceeded max header count.\n");
            return false;
        }
        if (!line_end) return false; // Header not fully received

        if (!parse_header(pos, line_end - pos, 
                         &headers_array[*header_count][0],  // key
                         &headers_array[*header_count][1])) // value
            return false;

        pos = line_end + strlen(line_separator);
        (*header_count)++;
    }

    // Convert non_strings to null-terminated strings
    for (size_t i = 0; i < *header_count; i++) {
        headers_array[i][0].start[headers_array[i][0].n] = '\0';
        headers_array[i][1].start[headers_array[i][1].n] = '\0';
        request->headers[i].key = headers_array[i][0].start;
        request->headers[i].value = headers_array[i][1].start;
    }

    *next_pos = pos + strlen(line_separator);
    return true;
}

static ssize_t parse_content_length(struct request *request, size_t header_count) {
    for (size_t i = 0; i < header_count; i++) {
        if (strcmp(request->headers[i].key, "Content-Length") == 0) {
            ssize_t length = strtoul(request->headers[i].value, NULL, 10);
            fprintf(stderr, "Found Content-Length header: %zd\n", length);
            return length;
        }
    }
    fprintf(stderr, "No Content-Length header found\n");
    return 0;
}

ssize_t parse_request(char *buffer, size_t n, struct request *request) {
    fprintf(stderr, "Parsing request of size %zu\n", n);
    fprintf(stderr, "Request content: %.*s\n", (int)n, buffer);

    const char *line_separator = "\r\n";
    const char *end = buffer + n;
    char *pos = buffer;

    // Parse request line
    char *line_end = memstr(pos, end - pos, line_separator);
    if (!line_end) {
        fprintf(stderr, "Request line not complete\n");
        return 0; // Request line not received yet
    }

    struct non_string method = {0};
    struct non_string uri = {0};
    if (!parse_request_line(pos, line_end - pos, &method, &uri)) {
        fprintf(stderr, "Failed to parse request line\n");
        return -1;
    }

    fprintf(stderr, "Method: %.*s, URI: %.*s\n", (int)method.n, method.start, (int)uri.n, uri.start);
    pos = line_end + strlen(line_separator);

    // Parse headers
    size_t header_count = 0;
    if (!parse_headers(pos, end, line_separator, request, &header_count, &pos)) {
        fprintf(stderr, "Failed to parse headers\n");
        return -1;
    }

    fprintf(stderr, "Parsed %zu headers\n", header_count);

    // Parse payload length
    request->payload_length = parse_content_length(request, header_count);
    if (request->payload_length < 0) {
        if (method.start && strncmp(method.start, "PUT", strlen("PUT")) == 0) {
            fprintf(stderr, "Content-Length required for PUT request\n");
            return -1; // Content-Length required for PUT requests
        }
        request->payload_length = 0;
    }

    // Verify payload is complete
    request->payload = pos;
    if (pos + request->payload_length > end) {
        fprintf(stderr, "Payload not complete (have %zd bytes, need %zd)\n", 
                end - pos, request->payload_length);
        return 0; // Payload not yet received completely
    }

    // Finalize request by null-terminating strings
    method.start[method.n] = '\0';
    request->method = method.start;
    uri.start[uri.n] = '\0';
    request->uri = uri.start;

    fprintf(stderr, "Successfully parsed request: %s %s (payload: %zd bytes)\n",
            request->method, request->uri, request->payload_length);

    return pos + request->payload_length - buffer;
}

string get_header(const struct request *request, const string name) {
    for (size_t i = 0; i < HTTP_MAX_HEADERS; i++) {
        if (!request->headers[i].key) break;
        if (strcmp(request->headers[i].key, name) == 0)
            return request->headers[i].value;
    }
    return NULL;
}

