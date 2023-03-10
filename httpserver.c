#include <arpa/inet.h>
#include <asm-generic/errno.h>
#include <asm-generic/socket.h>
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
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

#include "libhttp.h"
#include "wq.h"

#define BUFFER 2048

#define RED "\x1B[31m"
#define GRN "\x1B[32m"
#define YEL "\x1B[33m"
#define BLU "\x1B[34m"
#define MAG "\x1B[35m"
#define CYN "\x1B[36m"
#define WHT "\x1B[37m"
#define RESET "\x1B[0m"

/*
 * Global configuration variables.
 * Values are set up in main() using the
 * command line arguments.
 */
wq_t work_queue;  // Only used by poolserver
int num_threads;  // Only used by poolserver
int server_port;  // Default value: 8000
int backlog;
char* server_files_directory;
bool is_working;

typedef struct {
  int client_fd;
  int target_fd;
  bool* is_done_client;
  bool* is_done_target;
  pthread_cond_t* cond;
  pthread_mutex_t* mutex;
  char* server_proxy_hostname;
} ARGS;

char* parse_host_name(int fd) {
  char buffer[LIBHTTP_REQUEST_MAX_SIZE + 1];
  ssize_t red;
  char* begin;
  char* end;
  char* host;

  for (size_t total = 0; total < LIBHTTP_REQUEST_MAX_SIZE;) {
    // Don't forget the MSG_PEEK flag.
    if ((red = recv(fd, buffer, LIBHTTP_REQUEST_MAX_SIZE - total, MSG_PEEK)) <=
        0) {
      if (red == 0) return 0;
      // perror("Error parse_host_name recv");
      return NULL;
    }
    total += red;
    buffer[total] = '\0';

    // Change to lowercase because some servers don't capitalize the H in Host
    str_to_lower(buffer, red);

    if ((begin = strstr(buffer, "host: ")) != NULL) {
      /*
       * Host field has the format "Host: mywebsite.com:80\r\n"
       * Port number is optional
       */
      begin += strlen("host: ");
      if ((end = strchr(begin, '\r')) != NULL) {
        void* colon = NULL;
        // Must use strchr or it will search all of the header
        if ((colon = memchr(begin, ':', end - begin)) != NULL) {
          end = colon;
        }
        host = malloc(end - begin + 1);
        strncpy(host, begin, end - begin);
        host[end - begin] = '\0';
        return host;
      }
    }
  }
  return NULL;
}

int connect_target(int client_fd, char* server_proxy_hostname) {
  /*
   * The code below does a DNS lookup of server_proxy_hostname and
   * opens a connection to it. Please do not modify.
   */
  int target_fd;
  int server_proxy_port = 80;
  struct sockaddr_in target_address;

  if (server_proxy_hostname == NULL) {
    puts("Hostname is null..");
    return -1;
  }

  memset(&target_address, 0, sizeof(target_address));
  target_address.sin_family = AF_INET;
  target_address.sin_port = htons(server_proxy_port);

  // Use DNS to resolve the proxy target's IP address
  struct hostent* target_dns_entry =
      gethostbyname2(server_proxy_hostname, AF_INET);

  // Create an IPv4 TCP socket to communicate with the proxy target.
  target_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (target_fd == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno,
            strerror(errno));
    close(client_fd);
    exit(errno);
  }

  if (target_dns_entry == NULL) {
    fprintf(stderr, "Cannot find host: %s\n", server_proxy_hostname);
    close(target_fd);
    close(client_fd);
    exit(ENXIO);
  }

  char* dns_address = target_dns_entry->h_addr_list[0];

  // Connect to the proxy target.
  memcpy(&target_address.sin_addr, dns_address,
         sizeof(target_address.sin_addr));
  int connection_status = connect(target_fd, (struct sockaddr*)&target_address,
                                  sizeof(target_address));

  if (connection_status < 0) {
    /* Dummy request parsing, just to be compliant. */
    struct http_request* req = http_request_parse(client_fd);
    free(req);
    return -1;
  }

  return target_fd;
}

void* proxy_client(void* args_) {
  ARGS* args = (ARGS*)args_;
  int client_fd = args->client_fd;
  int target_fd = args->target_fd;
  bool* is_done_client = args->is_done_client;
  char* curr_host = args->server_proxy_hostname;

  char buffer[LIBHTTP_REQUEST_MAX_SIZE + 1];
  ssize_t red;
  ssize_t header_len;

  do {
    // If new host value appears, then close connection
    char* new_host = parse_host_name(client_fd);
    if (new_host == NULL || strcmp(new_host, curr_host) != 0) {
      break;
    }
    // puts("Target waiting get_header_len");
    header_len = get_header_len(client_fd, "\r\n\r\n");
    if (header_len <= 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        puts(YEL "Client socket timed-out!" RESET);
      } else if (header_len < 0) {
        perror("Error get_header_len");
      }
      break;
    }
    // puts("Red target header");
    if ((red = readn(client_fd, buffer, header_len)) < header_len) {
      perror("Error getting header readn");
      break;
    }
    buffer[red] = '\0';
    printf("%s", buffer);
    ssize_t wrote;
    if ((wrote = writen(target_fd, buffer, header_len)) < header_len) {
      perror("Error getting header writen");
      printf("wrote = %zu\n", wrote);
      break;
    }
    // printf("%s", buffer);
    enum body_enum body = has_body(buffer, header_len);
    ssize_t chunk_size;

    if (body == BODY_CHUNKED) {
      if ((chunk_size = http_get_next_chunk(client_fd)) == -1) {
        break;
      }
    } else if (body == BODY_LENGTH) {
      chunk_size = http_get_content_length(buffer);
      // printf("CHUUUUNK ========= %zd\n", chunk_size);
      if (relay_large_msg(buffer, LIBHTTP_REQUEST_MAX_SIZE, client_fd,
                          target_fd, chunk_size) <= 0) {
        perror("Error sending Content-Length message");
        break;
      }
    }

    while (body == BODY_CHUNKED) {
      if (relay_large_msg(buffer, LIBHTTP_REQUEST_MAX_SIZE, client_fd,
                          target_fd, chunk_size) <= 0) {
        perror("Error sending chunk");
        break;
      }
      // Don't get more chunks if we sent the last one.
      if (chunk_size == 5 && memcmp(buffer, "0\r\n\r\n", 5) == 0) {
        break;
      }
      chunk_size = http_get_next_chunk(client_fd);
    }
  } while (1);
  pthread_mutex_lock(args->mutex);
  *is_done_client = true;
  pthread_cond_signal(args->cond);
  pthread_mutex_unlock(args->mutex);
  // puts("Client Finished!");
  pthread_exit(NULL);
}

void* proxy_target(void* args_) {
  ARGS* args = (ARGS*)args_;
  int client_fd = args->client_fd;
  int target_fd = args->target_fd;
  bool* is_done_target = args->is_done_target;

  char buffer[LIBHTTP_REQUEST_MAX_SIZE + 1];
  ssize_t header_len;
  ssize_t red;
  enum body_enum body;

  do {
    // Read the HTTP header
    header_len = get_header_len(target_fd, "\r\n\r\n");
    if (header_len <= 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        puts(YEL "Target socket timed-out!" RESET);
      } else if (header_len < 0) {
        perror("Error get_header_len");
      }
      break;
    }
    // puts("Red target header");
    if ((red = readn(target_fd, buffer, header_len)) < header_len) {
      perror("Error getting header readn");
      break;
    }
    buffer[red] = '\0';
    printf(GRN "%s" RESET, buffer);
    ssize_t wrote;
    if ((wrote = writen(client_fd, buffer, header_len)) < header_len) {
      perror("Error getting header writen");
      printf("wrote = %zu\n", wrote);
      break;
    }

    body = has_body(buffer, header_len);

    ssize_t chunk_size;

    if (body == BODY_LENGTH) {
      chunk_size = http_get_content_length(buffer);
      // printf("CHUUUUNK ========= %zd\n", chunk_size);
      if (relay_large_msg(buffer, LIBHTTP_REQUEST_MAX_SIZE, target_fd,
                          client_fd, chunk_size) <= 0) {
        perror("Error sending Content-Length message");
        break;
      }
    }

    while (body == BODY_CHUNKED) {
      chunk_size = http_get_next_chunk(target_fd);
      if (relay_large_msg(buffer, LIBHTTP_REQUEST_MAX_SIZE, target_fd,
                          client_fd, chunk_size) <= 0) {
        perror("Error sending chunk");
        break;
      }
      // Don't get more chunks if we sent the last one.
      if (chunk_size == 5 && memcmp(buffer, "0\r\n\r\n", 5) == 0) {
        break;
      }
    }
  } while (1);

  pthread_mutex_lock(args->mutex);
  *is_done_target = true;
  pthread_cond_signal(args->cond);
  pthread_mutex_unlock(args->mutex);
  // puts("Target Finished!");
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
  int target_fd;
  char* server_proxy_hostname = NULL;

  if ((server_proxy_hostname = parse_host_name(client_fd)) == NULL) {
    // puts("Couldn't parse hostname");
    // http_reject_response(client_fd, 400);
    goto done;
  }

  if ((target_fd = connect_target(client_fd, server_proxy_hostname)) <= 0) {
    puts("Couldn't connect to proxy target");
    http_reject_response(client_fd, 502);
    goto done;
  }

  struct timeval timeout = {.tv_sec = 20, .tv_usec = 000000};
  if (setsockopt(target_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) < 0) {
    perror("setsockopt failed");
  }

  if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) < 0) {
    perror("setsockopt failed");
  }

  pthread_t client_thread;
  pthread_t target_thread;
  pthread_cond_t cond;
  pthread_mutex_t mutex;
  bool is_done_client = false;
  bool is_done_target = false;

  // Pass hostname to client_thread so it can detect changes
  ARGS args_client = {.client_fd = client_fd,
                      .target_fd = target_fd,
                      .is_done_client = &is_done_client,
                      .cond = &cond,
                      .mutex = &mutex,
                      .server_proxy_hostname = server_proxy_hostname};
  ARGS args_target = {.client_fd = client_fd,
                      .target_fd = target_fd,
                      .is_done_target = &is_done_target,
                      .cond = &cond,
                      .mutex = &mutex};

  pthread_mutex_init(&mutex, NULL);
  pthread_cond_init(&cond, NULL);

  pthread_create(&client_thread, NULL, proxy_client, &args_client);
  pthread_create(&target_thread, NULL, proxy_target, &args_target);

  /* As soon as one side of the connection closes (erroneously or not)
   * then close the other side and re-open a new one
   */
  pthread_mutex_lock(&mutex);
  while (!is_done_client && !is_done_target) pthread_cond_wait(&cond, &mutex);
  pthread_mutex_unlock(&mutex);

  if (!is_done_target && pthread_cancel(target_thread) != 0) {
    perror("Error cancelling target_thread");
  }
  if (!is_done_client && pthread_cancel(client_thread) != 0) {
    perror("Error cancelling client_thread");
  }

  if (pthread_join(target_thread, NULL)) {
    perror("Error Joining target_thread");
  }
  if (pthread_join(client_thread, NULL)) {
    perror("Error Joining client_thread");
  }

  printf(CYN "CLOSING SOCKET #%d\n" RESET, client_fd);

done:
  close(client_fd);
  close(target_fd);
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

  while (1) {
    int fd = wq_pop(&work_queue);
    request_handler(fd);
  }

  // Won't happen
  puts("Exited worker thread?!");
  pthread_exit(NULL);
}

/*
 * Creates `num_threads` amount of threads. Initializes the work queue.
 */
void init_thread_pool(int num_threads, void (*request_handler)(int)) {
  wq_init(&work_queue);
  for (int i = 0; i < num_threads; i++) {
    pthread_t id;
    if (pthread_create(&id, NULL, handle_clients, (void*)request_handler) !=
        0) {
      perror("Failed to create a worker thread");
      exit(-1);
    }
    puts("Created worker thread");
  }
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

    printf(MAG "Accepted connection from %s on port %d. Socket #%d\n" RESET,
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
    } else {
      close(client_socket_number);
    }

#elif THREADSERVER
    /*
     * When a client connection has been accepted, a new
     * thread is created. This thread will send a response
     * to the client. The main thread should continue
     * listening and accepting connections. The main
     * thread will NOT be joining with the new thread.
     */

    pthread_t thread;
    pthread_create(&thread, NULL, (void*)request_handler,
                   (void*)((long)client_socket_number));

    // We won't join on it
    pthread_detach(thread);

#elif POOLSERVER
    /*
     * When a client connection has been accepted, add the
     * client's socket number to the work queue. A thread
     * in the thread pool will send a response to the client.
     */
    wq_push(&work_queue, client_socket_number);

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

  if (request_handler == NULL) {
    fprintf(stderr,
            "Please specify either \"--files [DIRECTORY]\" or \n"
            "                      \"--proxy\"\n");
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

