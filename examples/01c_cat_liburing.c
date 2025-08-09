#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define QUEUE_DEPTH 1
#define BLOCK_SZ 1024

#define min(x, y) ((x) < (y) ? (x) : (y))
#define div_round_up(x, y) (((x) + (y) - 1) / (y))

struct file_info {
  off_t file_sz;
  struct iovec iovecs[0]; /* Flexible Array. Referred by readv/writev */
};

/*
 * Returns the size of the file whose open file descriptor is passed in.
 * Properly handles regular file and block devices as well. Pretty.
 * */

off_t get_file_size(int fd) {
  struct stat st;

  if (fstat(fd, &st) < 0) {
    fprintf(stderr, "fstat");
    exit(EXIT_FAILURE);
  }

  // Is regular file
  if (S_ISREG(st.st_mode)) {
    return st.st_size;
  }

  // Is block device
  if (S_ISBLK(st.st_mode)) {
    unsigned long long bytes;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
      fprintf(stderr, "ioctl");
      exit(EXIT_FAILURE);
    }

    return bytes;
  }

  exit(EXIT_FAILURE);
}

void *aligned_malloc(size_t alignment, size_t size) {
  void *buf = NULL;

  if (posix_memalign(&buf, alignment, size)) {
    fprintf(stderr, "posix_memalign");
    exit(EXIT_FAILURE);
  }

  return buf;
}

/*
 * Output a string of characters of len length to stdout.
 * We use buffered output here to be efficient,
 * since we need to output character-by-character.
 * */
void output_to_console(char *buf, int len) {
  while (len--) {
    fputc(*buf++, stdout);
  }
}

/*
 * Wait for a completion to be available, fetch the data from
 * the readv operation and print it to the console.
 * */

void get_completion_and_print(struct io_uring *ring) {
  struct io_uring_cqe *cqe;
  int ret = io_uring_wait_cqe(ring, &cqe);
  if (ret < 0) {
    fprintf(stderr, "io_uring_wait_cqe");
    exit(EXIT_FAILURE);
  }

  if (cqe->res < 0) {
    fprintf(stderr, "Async readv failed.\n");
    exit(EXIT_FAILURE);
  }

  // Get the "user_data" defined in struct io_uring_cqe
  struct file_info *fi = io_uring_cqe_get_data(cqe);

  off_t blocks = div_round_up(fi->file_sz, BLOCK_SZ);
  for (int i = 0; i < blocks; i++) {
    output_to_console(fi->iovecs[i].iov_base, fi->iovecs[i].iov_len);
  }

  // Notify the kernel that this CQE has been consumed
  io_uring_cqe_seen(ring, cqe);
}

/*
 * Submit the readv request via liburing
 * */

void submit_read_request(char *file_path, struct io_uring *ring) {
  int file_fd = open(file_path, O_RDONLY);
  if (file_fd < 0) {
    fprintf(stderr, "open");
    exit(EXIT_FAILURE);
  }

  off_t file_sz = get_file_size(file_fd);
  int blocks = div_round_up(file_sz, BLOCK_SZ);
  struct file_info *fi = malloc(sizeof(*fi) + sizeof(fi->iovecs[0]) * blocks);
  if (!fi) {
    fprintf(stderr, "Unable to allocate memory\n");
    exit(EXIT_FAILURE);
  }
  fi->file_sz = file_sz;

  off_t bytes_remaining = file_sz;
  for (off_t i = 0; i < blocks; ++i) {
    off_t bytes_to_read = min(bytes_remaining, BLOCK_SZ);

    fi->iovecs[i].iov_len = bytes_to_read;
    fi->iovecs[i].iov_base = aligned_malloc(BLOCK_SZ, BLOCK_SZ);

    bytes_remaining -= bytes_to_read;
  }

  /* Get an SQE */
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

  /* Setup a readv operation */
  io_uring_prep_readv(sqe, file_fd, fi->iovecs, blocks, 0);

  /* Set user data */
  io_uring_sqe_set_data(sqe, fi);

  /* Finally, submit the request */
  io_uring_submit(ring);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s [file name] <[file name] ...>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  struct io_uring ring;
  /* Initialize io_uring */
  io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

  for (int i = 1; i < argc; i++) {
    submit_read_request(argv[i], &ring);
    get_completion_and_print(&ring);
  }

  /* Call the clean-up function. */
  io_uring_queue_exit(&ring);

  return EXIT_SUCCESS;
}