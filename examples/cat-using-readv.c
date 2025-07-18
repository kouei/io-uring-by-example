#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>

#define BLOCK_SZ 4096

#define min(x, y) ((x) < (y) ? (x) : (y))

/*
 * Returns the size of the file whose open file descriptor is passed in.
 * Properly handles regular file and block devices as well. Pretty.
 * */

off_t get_file_size(int fd) {
  struct stat st;

  if (fstat(fd, &st) < 0) {
    fprintf(stderr, "fstat");
    exit(-1);
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
      exit(-1);
    }

    return bytes;
  }

  exit(-1);
}

void *aligned_malloc(size_t alignment, size_t size) {
  void *buf = aligned_alloc(alignment, size);
  if (!buf) {
    fprintf(stderr, "aligned_alloc");
    exit(-1);
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

void read_and_print_file(char *file_name) {
  int file_fd = open(file_name, O_RDONLY);
  if (file_fd < 0) {
    fprintf(stderr, "open");
    exit(-1);
  }

  off_t file_sz = get_file_size(file_fd);
  int blocks = (file_sz + BLOCK_SZ - 1) / BLOCK_SZ; // rounding-up
  struct iovec *iovecs = malloc(sizeof(struct iovec) * blocks);

  /*
   * For the file we're reading, allocate enough blocks to be able to hold
   * the file data. Each block is described in an iovec structure, which is
   * passed to readv as part of the array of iovecs.
   * */
  off_t bytes_remaining = file_sz;
  for (int i = 0; i < blocks; ++i) {
    off_t bytes_to_read = min(bytes_remaining, BLOCK_SZ);

    iovecs[i].iov_base = aligned_malloc(BLOCK_SZ, BLOCK_SZ);
    iovecs[i].iov_len = bytes_to_read;

    bytes_remaining -= bytes_to_read;
  }

  /*
   * The readv() call will block until all iovec buffers are filled with
   * file data. Once it returns, we should be able to access the file data
   * from the iovecs and print them on the console.
   * */
  int ret = readv(file_fd, iovecs, blocks);
  if (ret < 0) {
    fprintf(stderr, "readv");
    exit(-1);
  }

  for (int i = 0; i < blocks; i++) {
    output_to_console(iovecs[i].iov_base, iovecs[i].iov_len);
  }

  free(iovecs);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <filename1> [<filename2> ...]\n", argv[0]);
    exit(-1);
  }

  /*
   * For each file that is passed in as the argument, call the
   * read_and_print_file() function.
   * */
  for (int i = 1; i < argc; i++) {
    read_and_print_file(argv[i]);
  }

  return 0;
}