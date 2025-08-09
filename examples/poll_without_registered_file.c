#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 512
const char STR[] = "What is this life if, full of care,\nWe have no time to "
                   "stand and stare.\n";

struct io_uring ring;
int fds[1];
char buff1[BUF_SIZE];
char buff2[BUF_SIZE];
const char *filename = "test.txt";

void list_sq_poll_kernel_threads() {
  printf("\n*********** List SQ Poll Kernel Threads ***********\n");
  system("ps -eT | head -n 1"); // To print out the header
  system("ps -eT | grep iou-sqp");
  printf("***************************************************\n\n");
}

void open_files() {
  fds[0] = open(filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
  if (fds[0] < 0) {
    perror("open failed");
    exit(-1);
  }
}

void start_sq_polling_ops() {
  memcpy(buff1, STR, sizeof(STR));

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    fprintf(stderr, "Could not get SQE.\n");
    exit(-1);
  }

  io_uring_prep_write(sqe, fds[0], buff1, sizeof(STR), 0);

  io_uring_submit(&ring);

  struct io_uring_cqe *cqe;
  int ret = io_uring_wait_cqe(&ring, &cqe);
  if (ret < 0) {
    fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
    exit(-1);
  }
  /* Now that we have the CQE, let's process the data */
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
  io_uring_prep_read(sqe, fds[0], buff2, sizeof(STR), 0);

  io_uring_submit(&ring);

  ret = io_uring_wait_cqe(&ring, &cqe);
  if (ret < 0) {
    fprintf(stderr, "Error waiting for completion: %s\n", strerror(-ret));
    exit(-1);
  }
  /* Now that we have the CQE, let's process the data */
  if (cqe->res < 0) {
    fprintf(stderr, "Error in async operation: %s\n", strerror(-cqe->res));
  }
  printf("Result of the read operation: %d\n", cqe->res);
  io_uring_cqe_seen(&ring, cqe);

  printf("Contents read from file:\n");
  printf("%s", buff2);
}

int main() {
  if (geteuid() != 0) {
    fprintf(stderr, "\n********************* WARNING *********************\n");
    fprintf(stderr, " You don't have root privileges.                     \n");
    fprintf(stderr, " However, this is fine for kernel version > 5.11     \n");
    fprintf(stderr, "***************************************************\n\n");
  }

  struct io_uring_params params = {};
  params.flags |= IORING_SETUP_SQPOLL;
  params.sq_thread_idle = 600000;

  int ret = io_uring_queue_init_params(8, &ring, &params);
  if (ret) {
    fprintf(stderr, "Unable to setup io_uring: %s\n", strerror(-ret));
    exit(-1);
  }

  list_sq_poll_kernel_threads();

  open_files();

  start_sq_polling_ops();

  io_uring_queue_exit(&ring);

  return 0;
}
