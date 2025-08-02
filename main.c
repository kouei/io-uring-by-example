#include "liburing.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_DEPTH 8

#define BUF_COUNT 2
#define BUF_SIZE 512

#define STR                                                                    \
  "What is this life if, full of care,\nWe have no time to stand and stare.\n"

struct io_uring ring;

int fixed_buffers() {
  const char *filename = "test.txt";
  int fd = open(filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
  if (fd < 0) {
    perror("open");
    exit(-1);
  }

  struct iovec iov[BUF_COUNT];
  for (int i = 0; i < BUF_COUNT; i++) {
    iov[i].iov_len = BUF_SIZE;
    iov[i].iov_base = malloc(iov[i].iov_len);
  }

  int ret = io_uring_register_buffers(&ring, iov, BUF_COUNT);
  if (ret) {
    fprintf(stderr, "Error registering buffers: %s", strerror(-ret));
    exit(-1);
  }

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    fprintf(stderr, "Could not get SQE.\n");
    exit(-1);
  }

  memcpy(iov[0].iov_base, STR, sizeof(STR));
  io_uring_prep_write_fixed(sqe, fd, iov[0].iov_base, sizeof(STR), 0, 0);

  io_uring_submit(&ring);

  struct io_uring_cqe *cqe;
  ret = io_uring_wait_cqe(&ring, &cqe);
  if (ret < 0) {
    fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
    exit(-1);
  }

  if (cqe->res < 0) {
    fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
  }

  printf("Result of the write operation: %d\n", cqe->res);
  io_uring_cqe_seen(&ring, cqe);

  sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    fprintf(stderr, "Could not get SQE.\n");
    exit(-1);
  }

  io_uring_prep_read_fixed(sqe, fd, iov[1].iov_base, sizeof(STR), 0, 1);

  io_uring_submit(&ring);

  ret = io_uring_wait_cqe(&ring, &cqe);
  if (ret < 0) {
    fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
    exit(-1);
  }

  if (cqe->res < 0) {
    fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
  }

  printf("Result of the read operation: %d\n", cqe->res);
  io_uring_cqe_seen(&ring, cqe);

  printf("\nContents read from file:\n");
  printf("%s", (char *)iov[1].iov_base);

  return 0;
}

int main() {
  int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
  if (ret) {
    fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
    exit(-1);
  }

  fixed_buffers();

  io_uring_queue_exit(&ring);

  return 0;
}