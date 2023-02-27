#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>

#include "libhttp.h"
#include "wq.h"

#define BUFFER 2048

/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
wq_t work_queue;  // Only used by poolserver
int num_threads;  // Only used by poolserver
int server_port;  // Default value: 8000
int backlog;
char* server_files_directory;
char* server_proxy_hostname;
int server_proxy_port;
pthread_cond_t cond;
pthread_mutex_t mutex;
bool isDone = false;
bool isMade = false;

typedef struct {
  int client_fd;
  int* target_fd_adr;
} ARGS;

void* thread_func(void* args_) {
  ARGS* args = (ARGS*)args_;
  int client_fd = args->client_fd;
  int* target_fd = args->target_fd_adr;
  // Create an IPv4 TCP socket to communicate with the proxy target.
  *target_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (*target_fd == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno,
            strerror(errno));
    close(client_fd);
    exit(errno);
  }

  // So that reading more than the server sent doesn't cause
  // the read() to block for long
  // struct timeval timeout = {.tv_sec = 3, .tv_usec = 000000};
  // if (setsockopt(*target_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
  // sizeof(timeout)) < 0) {
  // perror("setsockopt failed");
  // }

  /*
   * The code below does a DNS lookup of server_proxy_hostname and
   * opens a connection to it. Please do not modify.
   */
  struct sockaddr_in target_address;
  memset(&target_address, 0, sizeof(target_address));
  target_address.sin_family = AF_INET;
  target_address.sin_port = htons(server_proxy_port);

  // Use DNS to resolve the proxy target's IP address
  struct hostent* target_dns_entry =
      gethostbyname2(server_proxy_hostname, AF_INET);

  if (target_dns_entry == NULL) {
    fprintf(stderr, "Cannot find host: %s\n", server_proxy_hostname);
    close(*target_fd);
    close(client_fd);
    exit(ENXIO);
  }

  char* dns_address = target_dns_entry->h_addr_list[0];

  // Connect to the proxy target.
  memcpy(&target_address.sin_addr, dns_address,
         sizeof(target_address.sin_addr));
  int connection_status = connect(*target_fd, (struct sockaddr*)&target_address,
                                  sizeof(target_address));

  if (connection_status < 0) {
    /* Dummy request parsing, just to be compliant. */
    struct http_request* req = http_request_parse(client_fd);

    http_start_response(client_fd, 502);
    http_send_header(client_fd, "Content-Type", "text/html");
    http_end_headers(client_fd);
    free(req);
    close(*target_fd);
    close(client_fd);
    pthread_exit(NULL);
  }
  pthread_mutex_lock(&mutex);
  isMade = true;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);

  char* buffer = malloc(LIBHTTP_REQUEST_MAX_SIZE + 1);
  char* post_header = NULL;
  size_t header_len;
  ssize_t red;
  enum body_enum body;
  size_t total = 0;
  size_t overflow = 0;

  // Read the HTTP header
  while (total < LIBHTTP_REQUEST_MAX_SIZE) {
    // Keep reading until "\r\n\r\n" is found
    if ((red = read(*target_fd, buffer + total,
                    LIBHTTP_REQUEST_MAX_SIZE - total)) <= 0) {
      writen(STDOUT_FILENO, buffer, red);
      perror("header read failed");
      goto done;
    }
    total += red;
    buffer[total] = '\0';
    if ((post_header = strstr(buffer, "\r\n\r\n")) != NULL) {
      // Calculate how many characters were read past the header
      post_header += 4;
      overflow = &buffer[total] - post_header;
      printf("TOTAL RED = %zd\n", total);
      // printf("%s", buffer);
      printf("OVEEERFLOWW   ========= %zu\n", overflow);
      header_len = post_header - buffer;
      printf("HEADER LENGTH ============== %zd\n", header_len);

      // Send the HTTP header
      if (writen(client_fd, buffer, total) == -1) {
        perror("thread: header writen failed");
        goto done;
      }

      break;
    }
  }

  body = has_body(buffer, header_len);

  ssize_t chunk_size;

  /* Overflow might be bigger than one chunk
     Keep looking for last chunk size value read */
  if (body == BODY_CHUNKED && overflow > 0) {
    char* end_ptr;
    char* position = post_header;
    for (size_t size = 0; size < overflow; size += chunk_size) {
      chunk_size = strtol(position, &end_ptr, 16);
      chunk_size += 4 + end_ptr - position;
      position += chunk_size;
    }
    if (chunk_size == 5 && strcmp(position - 5, "0\r\n\r\n") == 0) {
      goto done;
    }
    chunk_size -= overflow;
  } else if (body == BODY_CHUNKED) {
    if ((chunk_size = http_get_next_chunk(*target_fd)) == -1) {
      goto done;
    }
  }

  if (body == BODY_LENGTH) {
    chunk_size = http_get_content_length(buffer);
    chunk_size -= overflow;
    printf("CHUUUUNK ========= %zd\n", chunk_size);
    if (relay_large_msg(buffer, LIBHTTP_REQUEST_MAX_SIZE, *target_fd, client_fd,
                        chunk_size) <= 0) {
      perror("Error sending Content-Length message");
      goto done;
    }
  }

  while (body == BODY_CHUNKED) {
    if (relay_large_msg(buffer, LIBHTTP_REQUEST_MAX_SIZE, *target_fd, client_fd,
                        chunk_size) <= 0) {
      perror("Error sending chunk");
      goto done;
    }
    // Don't get more chunks if we sent the last one.
    if (chunk_size == 5 && memcmp(buffer, "0\r\n\r\n", 5) == 0) {
      goto done;
    }
    chunk_size = http_get_next_chunk(*target_fd);
  }

done:
  close(*target_fd);
  free(buffer);
  pthread_mutex_lock(&mutex);
  isDone = true;
  puts("Sending finish signal");
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);
  pthread_exit(NULL);
}

/*  Serves the contents the file stored at `path` to the client socket `fd`. */
void serve_file(int sock_fd, char* path) {
  char buffer[BUFFER];
  int red;
  int file_fd;

  file_fd = open(path, O_RDONLY);
  if (file_fd < 0) {
    http_reject_response(sock_fd, 404);
    perror("open error");
    close(sock_fd);
    return;
  }
  off_t file_size = lseek(file_fd, 0, SEEK_END);

  http_start_response(sock_fd, 200);
  http_send_header(sock_fd, "Content-Type", http_get_mime_type(path));
  dprintf(sock_fd, "%s: %zu\r\n", "Content-Length", file_size);
  http_end_headers(sock_fd);

  // Rewind after checking file size
  lseek(file_fd, 0, SEEK_SET);

  while ((red = read(file_fd, buffer, BUFFER)) > 0) {
    if (writen(sock_fd, buffer, red) == -1) {
      perror("serve_file: Writen failed");
    }
  }

  close(file_fd);
}

void serve_directory(int fd, char* path) {
  DIR* dirp = opendir(path);
  if (dirp == NULL) {
    perror("opendir failed");
    http_reject_response(fd, 500);
    closedir(dirp);
    return;
  }

  // Check if dir has an index.html
  errno = 0;
  struct dirent* dirent;
  while ((dirent = readdir(dirp)) != NULL) {
    if (strcmp(dirent->d_name, "index.html") == 0) {
      int length = strlen(path) + strlen("/index.html") + 1;
      char buffer[length];
      http_format_index(buffer, path);
      closedir(dirp);
      serve_file(fd, buffer);
      return;
    }
  }
  if (errno) {
    perror("readdir failed");
    http_reject_response(fd, 500);
    closedir(dirp);
    return;
  }
  closedir(dirp);

  http_start_response(fd, 200);
  http_send_header(fd, "Content-Type", "text/html");
  http_end_headers(fd);

  // No index.html. List entries.
  struct dirent** namelist;
  int files_num;
  files_num = scandir(path, &namelist, NULL, alphasort);
  if (files_num < 0) {
    perror("scandir");
    http_reject_response(fd, 500);
    return;
  }
  for (int i = 0; i < files_num; i++) {
    if (strcmp(namelist[i]->d_name, ".") == 0) {
      continue;
    }

    int length = strlen("<a href=\"//\"></a><br/>") + strlen(path) +
                 strlen(namelist[i]->d_name) * 2 + 1;
    char buffer[length];
    http_format_href(buffer, path, namelist[i]->d_name);
    dprintf(fd, "%s", buffer);
    free(namelist[i]);
  }
  free(namelist);
}

/*
 * Reads an HTTP request from client socket (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 *
 *   Closes the client socket (fd) when finished.
 */
void handle_files_request(int fd) {
  struct http_request* request = http_request_parse(fd);
  if (request == NULL || request->path[0] != '/') {
    http_reject_response(fd, 400);
    free(request);
    close(fd);
    return;
  }

  if (strstr(request->path, "..") != NULL) {
    http_reject_response(fd, 403);
    free(request);
    close(fd);
    return;
  }

  /* Convert beginning '/' to './' */
  char* path = malloc(1 + strlen(request->path) + 1);
  path[0] = '.';
  memcpy(path + 1, request->path, strlen(request->path) + 1);

  // Check if path exists
  struct stat file_stat;
  if (stat(path, &file_stat) == -1) {
    http_reject_response(fd, 404);
    free(request);
    free(path);
    close(fd);
    return;
  }

  // Get rid of any extra slashes
  // Must free() string returned by realpath
  char* real_path = realpath(path, NULL);
  char* relative_path = strstr(real_path, server_files_directory);

  // Skip public folder's name
  relative_path += strlen(server_files_directory);
  // If not top level directory, skip slash.
  relative_path = *relative_path != 0 ? relative_path + 1 : "./";

  if (S_ISREG(file_stat.st_mode))
    serve_file(fd, relative_path);
  else
    serve_directory(fd, relative_path);

  free(real_path);
  free(request);
  free(path);
  close(fd);
  return;
}

/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname and
 * port=server_proxy_port) and relays traffic to/from the stream client_fd and
 * the proxy target_fd. HTTP requests from the client (client_fd) should be sent
 * to the proxy target (target_fd), and HTTP responses from the proxy target
 * (target_fd) should be sent to the client (client_fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 *
 *   Closes client socket (client_fd) and proxy target client_fd (target_fd)
 * when finished.
 */
void handle_proxy_request(int client_fd) {
  // Create a second thread to RECEIVE data from target server
  pthread_t receive_thread;
  int target_fd = -99;
  ARGS args = {.target_fd_adr = &target_fd, .client_fd = client_fd};
  pthread_cond_init(&cond, NULL);
  pthread_mutex_init(&mutex, NULL);

  // Let main thread handle data SENDING to target server
  char buffer[LIBHTTP_REQUEST_MAX_SIZE];
  ssize_t red = 0;
  while (1) {
    red = read(client_fd, buffer, LIBHTTP_REQUEST_MAX_SIZE);
    if (red <= 0) {
      perror("Error reading request from client");
      break;
    }
    pthread_create(&receive_thread, NULL, &thread_func, &args);
    pthread_mutex_lock(&mutex);
    while (!isMade) {
      pthread_cond_wait(&cond, &mutex);
    }
    isMade = false;
    pthread_mutex_unlock(&mutex);

    // Replace Host header with the target server's hostname
    char* host_field = strstr(buffer, "Host:");
    char* post_host_field = buffer;
    buffer[red] = '\0';

    if (host_field) {
      // Plus 2 to skip \r\n
      post_host_field = strstr(host_field, "\r\n") + 2;
      if (writen(target_fd, buffer, host_field - buffer) == -1) {
        perror("send reqline: writen failed");
      }
      printf("%.*s", (int)(host_field - buffer), buffer);
      http_send_header(target_fd, "Host", server_proxy_hostname);
    }
    if (writen(target_fd, post_host_field, strlen(post_host_field)) == -1) {
      perror("writen failed");
    }
    pthread_mutex_lock(&mutex);
    while (!isDone) {
      pthread_cond_wait(&cond, &mutex);
    }
    isDone = false;
    pthread_mutex_unlock(&mutex);
    char* check = buffer + red - 4;
    // TODO: Change this because what if message has body (POST method)
    if (strncmp(check, "\r\n\r\n", 4) == 0) {
      break;
    }
  }
  close(client_fd);
  printf("CLOOOOOOOOOOOOOOOOSING %d\n", client_fd);
}

#ifdef POOLSERVER
/*
 * All worker threads will run this function until the server shutsdown.
 * Each thread should block until a new request has been received.
 * When the server accepts a new connection, a thread should be dispatched
 * to send a response to the client.
 */
void* handle_clients(void* void_request_handler) {
  void (*request_handler)(int) = (void (*)(int))void_request_handler;
  /* (Valgrind) Detach so thread frees its memory on completion, since we
   * won't be joining on it. */
  pthread_detach(pthread_self());

  /* TODO: PART 7 */
  /* PART 7 BEGIN */

  /* PART 7 END */
}

/*
 * Creates `num_threads` amount of threads. Initializes the work queue.
 */
void init_thread_pool(int num_threads, void (*request_handler)(int)) {
  /* TODO: PART 7 */
  /* PART 7 BEGIN */

  /* PART 7 END */
}
#endif

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int* socket_number, void (*request_handler)(int)) {
  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  // Creates a socket for IPv4 and TCP.
  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    perror("Failed to create a new socket");
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                 sizeof(socket_option)) == -1) {
    perror("Failed to set socket options");
    exit(errno);
  }

  // Setup arguments for bind()
  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  /*
   * Given the socket created above, call bind() to give it
   * an address and a port. Then, call listen() with the socket.
   * An appropriate size of the backlog is 1024, though you may
   * play around with this value during performance testing.
   */

  if (bind(*socket_number, (struct sockaddr*)&server_address,
           sizeof(server_address)) == -1) {
    perror("Bind failed");
    exit(errno);
  }

  if (listen(*socket_number, backlog) == -1) {
    perror("Listen failed");
    exit(errno);
  }

  printf("Listening on port %d...\n", server_port);

#ifdef POOLSERVER
  /*
   * The thread pool is initialized *before* the server
   * begins accepting client connections.
   */
  init_thread_pool(num_threads, request_handler);
#endif

  while (1) {
    client_socket_number =
        accept(*socket_number, (struct sockaddr*)&client_address,
               (socklen_t*)&client_address_length);
    if (client_socket_number < 0) {
      perror("Error accepting socket");
      continue;
    }

    printf("Accepted connection from %s on port %d. SOCKEEEEEEEEET = %d\n",
           inet_ntoa(client_address.sin_addr), client_address.sin_port,
           client_socket_number);

#ifdef BASICSERVER
    /*
     * This is a single-process, single-threaded HTTP server.
     * When a client connection has been accepted, the main
     * process sends a response to the client. During this
     * time, the server does not listen and accept connections.
     * Only after a response has been sent to the client can
     * the server accept a new connection.
     */
    request_handler(client_socket_number);

#elif FORKSERVER
    /*
     * When a client connection has been accepted, a new
     * process is spawned. This child process will send
     * a response to the client. Afterwards, the child
     * process should exit. During this time, the parent
     * process should continue listening and accepting
     * connections.
     */
    pid_t child_pid;
    if ((child_pid = fork()) == -1) {
      perror("Erorr forking");
      exit(-1);
    }

    if (child_pid == 0) {
      request_handler(client_socket_number);
      exit(0);
    }

#elif THREADSERVER
    /*
     * TODO: PART 6
     *
     * When a client connection has been accepted, a new
     * thread is created. This thread will send a response
     * to the client. The main thread should continue
     * listening and accepting connections. The main
     * thread will NOT be joining with the new thread.
     */

    /* PART 6 BEGIN */

    /* PART 6 END */
#elif POOLSERVER
    /*
     * TODO: PART 7
     *
     * When a client connection has been accepted, add the
     * client's socket number to the work queue. A thread
     * in the thread pool will send a response to the client.
     */

    /* PART 7 BEGIN */

    /* PART 7 END */
#endif
  }

  shutdown(*socket_number, SHUT_RDWR);
  close(*socket_number);
}

int server_fd;
void signal_callback_handler(int signum) {
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
  exit(0);
}

char* USAGE =
    "Usage: ./httpserver --files some_directory/ [--port 8000 --num-threads "
    "5]\n"
    "       ./httpserver --proxy inst.eecs.berkeley.edu:80 [--port 8000 "
    "--num-threads 5]\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) {
  signal(SIGINT, signal_callback_handler);
  signal(SIGPIPE, SIG_IGN);

  /* Default settings */
  server_port = 8000;
  backlog = 1024;
  void (*request_handler)(int) = NULL;

  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char* proxy_target = argv[++i];
      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char* colon_pointer = strchr(proxy_target, ':');
      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {
        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char* server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--num-threads", argv[i]) == 0) {
      char* num_threads_str = argv[++i];
      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1) {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else if (strcmp("--backlog", argv[i]) == 0) {
      backlog = atoi(argv[i]);
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  if (server_files_directory == NULL && server_proxy_hostname == NULL) {
    fprintf(stderr,
            "Please specify either \"--files [DIRECTORY]\" or \n"
            "                      \"--proxy [HOSTNAME:PORT]\"\n");
    exit_with_usage();
  }

#ifdef POOLSERVER
  if (num_threads < 1) {
    fprintf(stderr, "Please specify \"--num-threads [N]\"\n");
    exit_with_usage();
  }
#endif

  chdir(server_files_directory);
  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}
