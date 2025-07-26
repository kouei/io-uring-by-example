#include <ctype.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_SERVER_PORT 8000
#define QUEUE_DEPTH 256
#define READ_SZ 8192

int server_socket;
struct sockaddr_in client_addr;
socklen_t client_addr_len = sizeof(client_addr);
struct io_uring ring;

enum event_type_t {
  EVENT_TYPE_ACCEPT,
  EVENT_TYPE_READ,
  EVENT_TYPE_WRITE,
};

struct request {
  enum event_type_t event_type;
  int iovec_count;
  int client_socket;
  struct iovec iov[0]; /* Flexible Array Member */
};

const char *unimplemented_content =
    "HTTP/1.0 400 Bad Request\r\n"
    "Content-type: text/html\r\n"
    "\r\n"
    "<html>"
    "<head>"
    "<title>ZeroHTTPd: Unimplemented</title>"
    "</head>"
    "<body>"
    "<h1>Bad Request (Unimplemented)</h1>"
    "<p>Your client sent a request ZeroHTTPd did not understand and it is "
    "probably not your fault.</p>"
    "</body>"
    "</html>";

const char *http_404_content = "HTTP/1.0 404 Not Found\r\n"
                               "Content-type: text/html\r\n"
                               "\r\n"
                               "<html>"
                               "<head>"
                               "<title>ZeroHTTPd: Not Found</title>"
                               "</head>"
                               "<body>"
                               "<h1>Not Found (404)</h1>"
                               "<p>Your client is asking for an object that "
                               "was not found on this server.</p>"
                               "</body>"
                               "</html>";

/*
 * Utility function to convert a string to lower case.
 * */

void strtolower(char *str) {
  for (; *str; ++str)
    *str = (char)tolower(*str);
}
/*
 One function that prints the system call and the error details
 and then exits with error code 1. Non-zero meaning things didn't go well.
 */
void fatal_error(const char *syscall) {
  perror(syscall);
  exit(1);
}

/*
 * This function is responsible for setting up the main listening socket used by
 * the web server.
 * */

int setup_listening_socket() {
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    fatal_error("socket()");
  }

  int enable = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
    fatal_error("setsockopt(SO_REUSEADDR)");
  }

  struct sockaddr_in srv_addr = {};
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(DEFAULT_SERVER_PORT);
  srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  /* We bind to a port and turn this socket into a listening
   * socket.
   * */
  int ret = bind(sock, (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (ret < 0) {
    fatal_error("bind()");
  }

  ret = listen(sock, 10);
  if (ret < 0) {
    fatal_error("listen()");
  }

  return sock;
}

void queue_accept_request() {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (sqe == NULL) {
    fprintf(stderr, "io_uring_get_sqe() failed.");
    exit(-1);
  }

  io_uring_prep_accept(sqe, server_socket, (struct sockaddr *)&client_addr,
                       &client_addr_len, 0);

  struct request *req = malloc(sizeof(*req));
  req->event_type = EVENT_TYPE_ACCEPT;

  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);
}

int queue_read_request(int client_socket) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (sqe == NULL) {
    fprintf(stderr, "io_uring_get_sqe() failed.");
    exit(-1);
  }

  struct request *req = malloc(sizeof(*req) + sizeof(req->iov[0]));
  req->event_type = EVENT_TYPE_READ;
  req->iov[0].iov_len = READ_SZ;
  req->iov[0].iov_base = malloc(req->iov[0].iov_len);
  req->client_socket = client_socket;
  memset(req->iov[0].iov_base, 0, req->iov[0].iov_len);

  /* Linux kernel 5.5 has support for readv, but not for recv() or read() */
  io_uring_prep_readv(sqe, client_socket, &req->iov[0], 1, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);
  return 0;
}

int queue_write_request(struct request *req) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (sqe == NULL) {
    fprintf(stderr, "io_uring_get_sqe() failed.");
    exit(-1);
  }

  req->event_type = EVENT_TYPE_WRITE;

  io_uring_prep_writev(sqe, req->client_socket, req->iov, req->iovec_count, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);
  return 0;
}

void send_static_string_content(const char *str, int client_socket) {
  struct request *req = malloc(sizeof(*req) + sizeof(req->iov[0]));

  req->iovec_count = 1;
  req->client_socket = client_socket;
  req->iov[0].iov_len = strlen(str);
  req->iov[0].iov_base = malloc(req->iov[0].iov_len);
  memcpy(req->iov[0].iov_base, str, req->iov[0].iov_len);

  queue_write_request(req);
}

/*
 * When ZeroHTTPd encounters any other HTTP method other than GET or POST, this
 * function is used to inform the client.
 * */

void handle_unimplemented_method(int client_socket) {
  send_static_string_content(unimplemented_content, client_socket);
}

/*
 * This function is used to send a "HTTP Not Found" code and message to the
 * client in case the file requested is not found.
 * */

void handle_http_404(int client_socket) {
  send_static_string_content(http_404_content, client_socket);
}

/*
 * Once a static file is identified to be served, this function is used to read
 * the file and write it over the client socket using Linux's sendfile() system
 * call. This saves us the hassle of transferring file buffers from kernel to
 * user space and back.
 * */

void copy_file_contents(char *file_path, off_t file_size, struct iovec *iov) {
  int fd = open(file_path, O_RDONLY);
  if (fd < 0) {
    fatal_error("read");
  }

  /* We should really check for short reads here */
  iov->iov_len = file_size;
  iov->iov_base = malloc(iov->iov_len);

  int offset = 0;
  int bytes_to_read = iov->iov_len;

  while (true) {
    int ret = read(fd, iov->iov_base + offset, bytes_to_read);
    if (ret < 0) {
      fprintf(stderr, "read() failed. error = %s\n", strerror(-ret));
      exit(1);
    }

    if (ret == bytes_to_read) {
      break;
    }

    if (ret < bytes_to_read) {
      offset += ret;
      bytes_to_read -= ret;
    }
  }

  close(fd);
}

/*
 * Simple function to get the file extension of the file that we are about to
 * serve.
 * */

const char *get_filename_ext(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot || dot == filename) {
    return "";
  }

  return dot + 1;
}

void set_iov(struct iovec *iov, const char *content) {
  iov->iov_len = strlen(content);
  iov->iov_base = malloc(iov->iov_len);
  memcpy(iov->iov_base, content, iov->iov_len);
}

/*
 * Sends the HTTP 200 OK header, the server string, for a few types of files, it
 * can also send the content type based on the file extension. It also sends the
 * content length header. Finally it send a '\r\n' in a line by itself
 * signalling the end of headers and the beginning of any content.
 * */

void prepare_headers(const char *path, off_t len, struct iovec *iov) {
  char small_case_path[1024];
  strcpy(small_case_path, path);
  strtolower(small_case_path);

  const char *str = "HTTP/1.0 200 OK\r\n";
  set_iov(&iov[0], str);

  str = "Server: zerohttpd/0.1\r\n";
  set_iov(&iov[1], str);

  /*
   * Check the file extension for certain common types of files
   * on web pages and send the appropriate content-type header.
   * Since extensions can be mixed case like JPG, jpg or Jpg,
   * we turn the extension into lower case before checking.
   * */
  char send_buffer[1024];
  const char *file_ext = get_filename_ext(small_case_path);
  if (strcmp("jpg", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: image/jpeg\r\n");
  } else if (strcmp("jpeg", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: image/jpeg\r\n");
  } else if (strcmp("png", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: image/png\r\n");
  } else if (strcmp("gif", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: image/gif\r\n");
  } else if (strcmp("htm", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: text/html\r\n");
  } else if (strcmp("html", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: text/html\r\n");
  } else if (strcmp("js", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: application/javascript\r\n");
  } else if (strcmp("css", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: text/css\r\n");
  } else if (strcmp("txt", file_ext) == 0) {
    strcpy(send_buffer, "Content-Type: text/plain\r\n");
  } else {
    fprintf(stderr, "Unexpected file extension = %s\n", file_ext);
    exit(-1);
  }

  set_iov(&iov[2], send_buffer);

  /* Send the content-length header, which is the file size in this case. */
  sprintf(send_buffer, "content-length: %ld\r\n", len);
  set_iov(&iov[3], send_buffer);

  /*
   * When the browser sees a '\r\n' sequence in a line on its own,
   * it understands there are no more headers. Content may follow.
   * */
  str = "\r\n";
  set_iov(&iov[4], str);
}

void handle_get_method(char *path, int client_socket) {
  char final_path[1024] = "http-home";
  strcat(final_path, path);

  /*
   If a path ends in a trailing slash, the client probably wants the index
   file inside of that directory.
   */
  if (path[strlen(path) - 1] == '/') {
    strcat(final_path, "index.html");
  }

  /* The stat() system call will give you information about the file
   * like type (regular file, directory, etc), size, etc. */
  struct stat path_stat;
  if (stat(final_path, &path_stat) == -1) {
    printf("Return 404: File Not Found: %s\n", final_path);
    handle_http_404(client_socket);
    return;
  }

  /* If this is not a regular file, return 404. */
  if (!S_ISREG(path_stat.st_mode)) {
    printf("Return 404: Not a Regular File: %s\n", final_path);
    handle_http_404(client_socket);
    return;
  }

  struct request *req = malloc(sizeof(*req) + (sizeof(req->iov[0]) * 6));
  req->iovec_count = 6;
  req->client_socket = client_socket;

  prepare_headers(final_path, path_stat.st_size, req->iov);
  copy_file_contents(final_path, path_stat.st_size, &req->iov[5]);
  queue_write_request(req);

  printf("200 %s %ld bytes\n", final_path, path_stat.st_size);
}

/*
 * This function looks at method used and calls the appropriate handler
 * function. Since we only implement GET and POST methods, it calls
 * handle_unimplemented_method() in case both these don't match. This sends an
 * error to the client.
 * */

void handle_http_verb(char *verb_buffer, int client_socket) {
  char *saveptr;

  char *http_verb = strtok_r(verb_buffer, " ", &saveptr);
  strtolower(http_verb);

  char *path = strtok_r(NULL, " ", &saveptr);

  if (strcmp(http_verb, "get") == 0) {
    handle_get_method(path, client_socket);
  } else {
    handle_unimplemented_method(client_socket);
  }
}

int get_line(const char *src, char *dest, int dest_sz) {
  for (int i = 0; i < dest_sz; i++) {
    if (src[i] == '\r' && src[i + 1] == '\n') {
      dest[i] = '\0';
      return 0;
    }

    dest[i] = src[i];
  }
  return 1;
}

int handle_read_request(struct request *req) {
  // An example of the content of "req->iov[0].iov_base" is like the following:
  //
  // GET /index.html HTTP/1.1\r\n
  // Host: 127.0.0.1:8000\r\n
  // User-Agent: curl/8.5.0\r\n
  // Accept: */*\r\n\r\n

  /* Get the first line, which will be the request */
  char first_line[1024];
  if (get_line(req->iov[0].iov_base, first_line, sizeof(first_line))) {
    fprintf(stderr, "Malformed request\n");
    exit(1);
  }

  // Now first_line == "GET /index.html HTTP/1.1"
  handle_http_verb(first_line, req->client_socket);
  return 0;
}

void server_loop() {
  queue_accept_request();

  while (true) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      fprintf(stderr, "io_uring_wait_cqe() failed. error = %s\n",
              strerror(-ret));
      exit(1);
    }

    struct request *req = (struct request *)cqe->user_data;
    if (cqe->res < 0) {
      fprintf(stderr, "Async request failed: %s for event: %d\n",
              strerror(-cqe->res), req->event_type);
      exit(1);
    }

    switch (req->event_type) {
    case EVENT_TYPE_ACCEPT:
      queue_accept_request();
      queue_read_request(cqe->res);
      break;

    case EVENT_TYPE_READ:
      if (cqe->res == 0) {
        fprintf(stderr, "Empty request!\n");
        break;
      }

      handle_read_request(req);
      free(req->iov[0].iov_base);
      break;

    case EVENT_TYPE_WRITE:
      for (int i = 0; i < req->iovec_count; i++) {
        free(req->iov[i].iov_base);
      }
      close(req->client_socket);
      break;

    default:
      fprintf(stderr, "Unexpected event type = %d\n", req->event_type);
      break;
    }

    free(req);
    /* Mark this request as processed */
    io_uring_cqe_seen(&ring, cqe);
  }
}

void sigint_handler(int signo) {
  printf("^C pressed. Shutting down.\n");
  io_uring_queue_exit(&ring);
  exit(0);
}

int main() {
  signal(SIGINT, sigint_handler);

  io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

  server_socket = setup_listening_socket();
  server_loop();

  return 0;
}