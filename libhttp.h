/*
 * A simple HTTP library.
 *
 * Usage example:
 *
 *     // Returns NULL if an error was encountered.
 *     struct http_request *request = http_request_parse(fd);
 *
 *     ...
 *
 *     http_start_response(fd, 200);
 *     http_send_header(fd, "Content-type", http_get_mime_type("index.html"));
 *     http_send_header(fd, "Server", "httpserver/1.0");
 *     http_end_headers(fd);
 *     http_send_string(fd, "<html><body><a href='/'>Home</a></body></html>");
 *
 *     close(fd);
 */

#ifndef LIBHTTP_H
#define LIBHTTP_H

#include <stddef.h>
#include <sys/types.h>
#define LIBHTTP_REQUEST_MAX_SIZE 8192

enum body_enum { BODY_NONE, BODY_LENGTH, BODY_CHUNKED };
struct http_request {
  char* method;
  char* path;
};

/*
 * Functions for parsing an HTTP request/response.
 */
struct http_request* http_request_parse(int fd);
ssize_t http_get_next_chunk(int socket_fd);
ssize_t http_get_content_length(char* http_header);

/*
 * Functions for sending an HTTP response.
 */
void http_start_response(int fd, int status_code);
void http_reject_response(int fd, int status_code);
void http_send_header(int fd, char* key, char* value);
void http_end_headers(int fd);
void http_format_href(char* buffer, char* path, char* filename);
void http_format_index(char* buffer, char* path);
int relay_large_msg(char* buffer, size_t max_size, int sock_from, int sock_to,
                    size_t msg_size);
ssize_t get_header_len(int socket_fd, char* target);

/* Gets the Content-Type based on a file name */
char* http_get_mime_type(char* file_name);

/* Reliable read & write */
ssize_t writen(int fd, const void* buffer, size_t n);
ssize_t readn(int fd, void* buffer, size_t n);

/* Util functions */
void str_to_lower(char* str, size_t len);
enum body_enum has_body(char* http_header, size_t len);
char* parse_host_name(int sock_fd);
int connect_target(int client_fd, char* server_hostname);

#endif
