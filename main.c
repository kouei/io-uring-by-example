#include <fcntl.h>
#include <liburing.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define QUEUE_DEPTH 8
#define BUFF_SZ 512

int efd;
char buff[BUFF_SZ];
struct io_uring ring;

void error_exit(char *message) {
  perror(message);
  exit(EXIT_FAILURE);
}

void *listener_thread(void *data) {
  printf("%s: Waiting for completion event...\n", __FUNCTION__);

  eventfd_t v;
  int ret = eventfd_read(efd, &v);
  if (ret < 0) {
    error_exit("eventfd_read");
  }

  printf("%s: Got completion event.\n", __FUNCTION__);

  struct io_uring_cqe *cqe;
  ret = io_uring_wait_cqe(&ring, &cqe);
  if (ret < 0) {
    fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
    return NULL;
  }

  if (cqe->res < 0) {
    fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
  }

  printf("Result of the operation: %d\n", cqe->res);
  io_uring_cqe_seen(&ring, cqe);

  printf("Contents read from file:\n%s\n", buff);
  return NULL;
}

void setup_io_uring() {
  int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
  if (ret) {
    fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
    exit(EXIT_FAILURE);
  }

  io_uring_register_eventfd(&ring, efd);
}

void read_file_with_io_uring() {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    fprintf(stderr, "Could not get SQE.\n");
    exit(EXIT_FAILURE);
  }

  int fd = open("test.txt", O_RDONLY);
  io_uring_prep_read(sqe, fd, buff, BUFF_SZ - 1, 0);
  io_uring_submit(&ring);
}

int main() {
  /* Create an eventfd instance */
  efd = eventfd(0, 0);
  if (efd < 0) {
    error_exit("eventfd");
  }

  /* Setup io_uring instance and register the eventfd */
  setup_io_uring();

  /* Create the listener thread */
  pthread_t thread;
  int ret = pthread_create(&thread, NULL, listener_thread, NULL);
  if (ret < 0) {
    error_exit("pthread_create");
  }

  /* Sleep 5 sec to ensure child thread stuck on eventfd_read() */
  sleep(5);

  /* Initiate a read with io_uring */
  read_file_with_io_uring();

  /* Wait for listener thread to complete */
  pthread_join(thread, NULL);

  /* All done. Clean up and exit. */
  io_uring_queue_exit(&ring);

  return EXIT_SUCCESS;
}