#include "libhttp.h"

#include <arpa/inet.h>
#include <asm-generic/errno-base.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

ssize_t writen(int fd, const void* buffer, size_t n) {
  errno = 0;
  ssize_t wrote;
  size_t total;
  const unsigned char* it;

  it = buffer;
  for (total = 0; total < n;) {
    if ((wrote = write(fd, it, n - total)) <= 0) {
      if (wrote == -1 && errno == EINTR) {
        continue;
      } else {
        return -1;
      }
    }
    total += wrote;
    it += wrote;
  }
  if (total != n) {
    printf("WRITEN ERROR! total != n\n");
  }
  return total;
}

ssize_t readn(int fd, void* buffer, size_t n) {
  errno = 0;
  ssize_t red;
  size_t total;
  unsigned char* it;

  it = buffer;
  for (total = 0; total < n;) {
    red = read(fd, it, n - total);
    if (red == 0) {
      break;
    }
    if (red == -1) {
      if (errno == EINTR) {
        continue;
      } else {
        return -1;
      }
    }
    total += red;
    it += red;
  }
  return total;
}

// Used in proxy mode to send large known-size messages
int relay_large_msg(char* buffer, size_t max_size, int from, int to,
                    size_t msg_size) {
  size_t total;
  ssize_t red;
  size_t read_size;

  if (msg_size < max_size) {
    read_size = msg_size;
  } else {
    read_size = max_size;
  }

  for (total = 0; total < msg_size;) {
    red = readn(from, buffer, read_size);
    if (red <= 0) {
      puts("Error relay_large_msg readn");
      return -1;
    }
    buffer[red] = '\0';
    total += red;
    size_t wrote;
    if ((wrote = writen(to, buffer, red)) <= 0) {
      printf("read %zu, wrote %zu\n", red, wrote);
      return -1;
    }
    // printf("read %zu, wrote %zu\n", red, wrote);

    if (msg_size - total < max_size) {
      read_size = msg_size - total;
    } else {
      read_size = max_size;
    }
  }
  // printf("BREEEEEEEEEAK! red (%zu) and wanted (%zu)\n", total, msg_size);
  return total == msg_size;
}

ssize_t get_header_len(int fd, char* target) {
  char buffer[LIBHTTP_REQUEST_MAX_SIZE + 1];
  ssize_t red;
  char* found;
  ssize_t header_len;

  for (size_t total = 0; total < LIBHTTP_REQUEST_MAX_SIZE;) {
    // Get header length. Don't forget the MSG_PEEK flag.
    if ((red = recv(fd, buffer, LIBHTTP_REQUEST_MAX_SIZE - total, MSG_PEEK)) <=
        0) {
      if (red == 0) return 0;
      perror("Error get_header_len recv");
      return -1;
    }
    total += red;
    buffer[total] = '\0';
    // Found delimiter. Send the header.
    if ((found = strstr(buffer, target)) != NULL) {
      found += strlen(target);
      header_len = found - buffer;
      return header_len;
    }
  }
  // Mustn't reach here if the request is well-formed.
  puts("Error get_header_len: malformed request?");
  return -1;
}

/* chunk_size is the chunk size value at the beginning of each  chunk plus
 * "\r\n\r\n" plus the num of digits of the chunk size number */
ssize_t http_get_next_chunk(int socket_fd) {
  char recv_str[255];
  ssize_t chunk_size;
  char* end_ptr = NULL;
  ssize_t red = 0;
  for (size_t i = 1; i < 255; i++) {
    if ((red = recv(socket_fd, recv_str, i, MSG_PEEK)) <= 0) {
      perror("Error get_next_chunk");
      return -1;
    }
    // Do NOT use strchr because encoding might be raw bytes (gzip)
    if (memchr(recv_str, '\r', i) != NULL) {
      break;
    }
  }
  chunk_size = strtol(recv_str, &end_ptr, 16);
  chunk_size += 4 + end_ptr - recv_str;
  return chunk_size;
}

ssize_t http_get_content_length(char* http_header) {
  // No need to call str_to_lower because it's already
  // been called in has_body
  char* found = strstr(http_header, "content-length: ");
  if (found == NULL) {
    return -1;
  }
  found += strlen("content-length: ");
  return strtol(found, NULL, 10);
}

enum body_enum has_body(char* http_header, size_t len) {
  str_to_lower(http_header, len);
  if (strstr(http_header, "content-length") != NULL) {
    return BODY_LENGTH;
  }
  if (strstr(http_header, "transfer-encoding: chunked") != NULL) {
    return BODY_CHUNKED;
  }
  return BODY_NONE;
}

void str_to_lower(char* str, size_t len) {
  for (size_t i = 0; i < len; i++) {
    str[i] = tolower(str[i]);
  }
}

void http_fatal_error(char* message) {
  fprintf(stderr, "%s\n", message);
  exit(ENOBUFS);
}

struct http_request* http_request_parse(int fd) {
  struct http_request* request = malloc(sizeof(struct http_request));
  if (!request) http_fatal_error("Malloc failed");

  char* read_buffer = malloc(LIBHTTP_REQUEST_MAX_SIZE + 1);
  if (!read_buffer) http_fatal_error("Malloc failed");

  int bytes_read = read(fd, read_buffer, LIBHTTP_REQUEST_MAX_SIZE);
  read_buffer[bytes_read] = '\0'; /* Always null-terminate. */
  printf("%s", read_buffer);

  char *read_start, *read_end;
  size_t read_size;

  do {
    /* Read in the HTTP method: "[A-Z]*" */
    read_start = read_end = read_buffer;
    while (*read_end >= 'A' && *read_end <= 'Z') read_end++;
    read_size = read_end - read_start;
    if (read_size == 0) break;
    request->method = malloc(read_size + 1);
    memcpy(request->method, read_start, read_size);
    request->method[read_size] = '\0';

    /* Read in a space character. */
    read_start = read_end;
    if (*read_end != ' ') break;
    read_end++;

    /* Read in the path: "[^ \n]*" */
    read_start = read_end;
    while (*read_end != '\0' && *read_end != ' ' && *read_end != '\n')
      read_end++;
    read_size = read_end - read_start;
    if (read_size == 0) break;
    request->path = malloc(read_size + 1);
    memcpy(request->path, read_start, read_size);
    request->path[read_size] = '\0';

    /* Read in HTTP version and rest of request line: ".*" */
    read_start = read_end;
    while (*read_end != '\0' && *read_end != '\n') read_end++;
    if (*read_end != '\n') break;
    read_end++;

    free(read_buffer);
    return request;
  } while (0);

  /* An error occurred. */
  free(request);
  free(read_buffer);
  return NULL;
}

char* http_get_response_message(int status_code) {
  switch (status_code) {
    case 100:
      return "Continue";
    case 200:
      return "OK";
    case 301:
      return "Moved Permanently";
    case 302:
      return "Found";
    case 304:
      return "Not Modified";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    default:
      return "Internal Server Error";
  }
}

void http_reject_response(int fd, int status_code) {
  http_start_response(fd, status_code);
  http_send_header(fd, "Content-Type", "text/html");
  http_end_headers(fd);
}

void http_start_response(int fd, int status_code) {
  dprintf(fd, "HTTP/1.0 %d %s\r\n", status_code,
          http_get_response_message(status_code));
}

void http_send_header(int fd, char* key, char* value) {
  dprintf(fd, "%s: %s\r\n", key, value);
}

void http_end_headers(int fd) { dprintf(fd, "\r\n"); }

char* http_get_mime_type(char* file_name) {
  char* file_extension = strrchr(file_name, '.');
  if (file_extension == NULL) {
    return "text/plain";
  }

  if (strcmp(file_extension, ".html") == 0 ||
      strcmp(file_extension, ".htm") == 0) {
    return "text/html";
  } else if (strcmp(file_extension, ".jpg") == 0 ||
             strcmp(file_extension, ".jpeg") == 0) {
    return "image/jpeg";
  } else if (strcmp(file_extension, ".png") == 0) {
    return "image/png";
  } else if (strcmp(file_extension, ".css") == 0) {
    return "text/css";
  } else if (strcmp(file_extension, ".js") == 0) {
    return "application/javascript";
  } else if (strcmp(file_extension, ".pdf") == 0) {
    return "application/pdf";
  } else {
    return "text/plain";
  }
}

/*
 * Puts `<a href="/path/filename">filename</a><br/>` into the provided buffer.
 * The resulting string in the buffer is null-terminated. It is the caller's
 * responsibility to ensure that the buffer has enough space for the resulting
 * string.
 */
void http_format_href(char* buffer, char* path, char* filename) {
  int length = strlen("<a href=\"//\"></a><br/>") + strlen(path) +
               strlen(filename) * 2 + 1;
  snprintf(buffer, length, "<a href=\"/%s/%s\">%s</a><br/>", path, filename,
           filename);
}

/*
 * Puts `path/index.html` into the provided buffer.
 * The resulting string in the buffer is null-terminated.
 * It is the caller's responsibility to ensure that the
 * buffer has enough space for the resulting string.
 */
void http_format_index(char* buffer, char* path) {
  int length = strlen(path) + strlen("/index.html") + 1;
  snprintf(buffer, length, "%s/index.html", path);
}
