#include "liburing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_DEPTH 16

struct io_uring ring;

void run_write_tasks() {
  size_t str_len = 8 * 1024;
  char *str = malloc(str_len);
  for (size_t i = 0; i < str_len; ++i) {
    str[i] = 'a' + (i % 26);
  }

  for (int i = 1; i <= QUEUE_DEPTH; ++i) {
    char filename[100];
    sprintf(filename, "test%d.txt", i);
    int fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd < 0) {
      perror("open");
      exit(-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      fprintf(stderr, "Could not get SQE.\n");
      exit(-1);
    }

    io_uring_prep_write(sqe, fd, str, str_len, 0);
    int *task_id = malloc(sizeof(*task_id));
    *task_id = i;
    io_uring_sqe_set_data(sqe, task_id);
  }

  io_uring_submit(&ring);

  for (int i = 0; i < QUEUE_DEPTH; ++i) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
      exit(-1);
    }

    int *task_id = (int *)cqe->user_data;
    printf("\nTask Id = %d, Operation Result = %d\n", *task_id, cqe->res);

    if (cqe->res < 0) {
      fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
    }

    io_uring_cqe_seen(&ring, cqe);
  }
}

int main() {
  int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
  if (ret) {
    fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
    exit(-1);
  }

  run_write_tasks();

  io_uring_queue_exit(&ring);

  return 0;
}