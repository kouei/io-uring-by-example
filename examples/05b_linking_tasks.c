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
    exit(EXIT_FAILURE);
  }
}

void link_operations() {
  const char *filename = "test.txt";
  int fd = open(filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
  // int fd = open(filename, O_WRONLY|O_TRUNC|O_CREAT, 0644);
  if (fd < 0) {
    perror("open");
    exit(EXIT_FAILURE);
  }

  const char str[] = "Hello, io_uring!";

  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "Could not get SQE.\n");
      exit(EXIT_FAILURE);
    }

    io_uring_prep_write(sqe, fd, str, sizeof(str) - 1, 0);
    enum task_type *type = malloc(sizeof(*type));
    *type = WRITE;
    io_uring_sqe_set_data(sqe, type);
    sqe->flags |= IOSQE_IO_LINK;
    printf("Task Created. Task Type = %s\n", get_task_type_string(*type));
  }

  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "Could not get SQE.\n");
      exit(EXIT_FAILURE);
    }

    io_uring_prep_read(sqe, fd, buffer, sizeof(str) - 1, 0);
    enum task_type *type = malloc(sizeof(*type));
    *type = READ;
    io_uring_sqe_set_data(sqe, type);
    sqe->flags |= IOSQE_IO_LINK;
    printf("Task Created. Task Type = %s\n", get_task_type_string(*type));
  }

  {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "Could not get SQE.\n");
      exit(EXIT_FAILURE);
    }

    io_uring_prep_close(sqe, fd);
    enum task_type *type = malloc(sizeof(*type));
    *type = CLOSE;
    io_uring_sqe_set_data(sqe, type);
    printf("Task Created. Task Type = %s\n", get_task_type_string(*type));
  }

  printf("\n");

  io_uring_submit(&ring);

  for (int i = 0; i < 3; i++) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
      exit(EXIT_FAILURE);
    }

    enum task_type type = *(enum task_type *)cqe->user_data;
    const char *type_str = get_task_type_string(type);
    printf("Task Completed. Task Type = %s, Operation Result = %d\n", type_str,
           cqe->res);

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
    exit(EXIT_FAILURE);
  }

  link_operations();

  io_uring_queue_exit(&ring);

  return EXIT_SUCCESS;
}