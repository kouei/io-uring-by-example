#include "liburing.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_DEPTH 8

struct io_uring ring;

char buffer[32];

enum task_type { READ, WRITE, CLOSE };

const char *get_task_type_string(enum task_type type) {
  if (type == READ) {
    return "READ";
  } else if (type == WRITE) {
    return "WRITE";
  } else if (type == CLOSE) {
    return "CLOSE";
  } else {
    fprintf(stderr, "Unexpected type = %d\n", type);
    exit(-1);
  }
}

void link_operations() {
  const char *filename = "test.txt";
  int fd = open(filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
  // int fd = open(filename, O_WRONLY|O_TRUNC|O_CREAT, 0644);
  if (fd < 0) {
    perror("open");
    exit(-1);
  }

  const char str[] = "Hello, io_uring!";

  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "Could not get SQE.\n");
      exit(-1);
    }

    io_uring_prep_write(sqe, fd, str, sizeof(str) - 1, 0);
    enum task_type *type = malloc(sizeof(*type));
    *type = WRITE;
    io_uring_sqe_set_data(sqe, type);
    sqe->flags |= IOSQE_IO_LINK;
  }

  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "Could not get SQE.\n");
      exit(-1);
    }

    io_uring_prep_read(sqe, fd, buffer, sizeof(str) - 1, 0);
    enum task_type *type = malloc(sizeof(*type));
    *type = READ;
    io_uring_sqe_set_data(sqe, type);
    sqe->flags |= IOSQE_IO_LINK;
  }

  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "Could not get SQE.\n");
      exit(-1);
    }

    io_uring_prep_close(sqe, fd);
    enum task_type *type = malloc(sizeof(*type));
    *type = CLOSE;
    io_uring_sqe_set_data(sqe, type);
  }

  io_uring_submit(&ring);

  for (int i = 0; i < 3; i++) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
      exit(-1);
    }

    enum task_type type = *(enum task_type *)cqe->user_data;
    const char *type_str = get_task_type_string(type);
    printf("\nTask Type = %s, Operation Result = %d\n", type_str, cqe->res);

    /* Now that we have the CQE, let's process the data */
    if (cqe->res < 0) {
      fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
    }

    io_uring_cqe_seen(&ring, cqe);
  }

  printf("\nBuffer contents: %s\n", buffer);
}

int main() {
  int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
  if (ret) {
    fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
    exit(-1);
  }

  link_operations();

  io_uring_queue_exit(&ring);

  return 0;
}