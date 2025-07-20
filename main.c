#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define QUEUE_DEPTH 2
#define BLOCK_SZ (16 * 1024)
#define min(x, y) ((x) < (y) ? (x) : (y))

static int infd, outfd;

struct io_data {
  int read;
  off_t first_offset, offset;
  size_t first_len;
  struct iovec iov;
  char bytes[0]; /* Flexible Array. Real data payload. */
};

static void setup_context(unsigned entries, struct io_uring *ring) {
  int ret;

  ret = io_uring_queue_init(entries, ring, 0);
  if (ret < 0) {
    fprintf(stderr, "queue_init: %s\n", strerror(-ret));
    exit(-1);
  }
}

static off_t get_file_size(int fd) {
  struct stat st;
  if (fstat(fd, &st) < 0) {
    fprintf(stderr, "get_file_size() failed.\n");
    exit(-1);
  }

  if (S_ISREG(st.st_mode)) {
    return st.st_size;
  }

  if (S_ISBLK(st.st_mode)) {
    off_t bytes;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
      fprintf(stderr, "ioctl() failed");
      exit(-1);
    }

    return bytes;
  }

  fprintf(stderr, "get_file_size() failed. st_mode = %u\n", st.st_mode);
  exit(-1);
}

static void queue_prepped(struct io_uring *ring, struct io_data *data) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    fprintf(stderr, "io_uring_get_sqe() failed");
    exit(-1);
  }

  if (data->read) {
    io_uring_prep_readv(sqe, infd, &data->iov, 1, data->offset);
  } else {
    io_uring_prep_writev(sqe, outfd, &data->iov, 1, data->offset);
  }

  io_uring_sqe_set_data(sqe, data);
}

static int queue_read(struct io_uring *ring, off_t size, off_t offset) {
  struct io_data *data = malloc(sizeof(*data) + size);
  if (!data) {
    fprintf(stderr, "queue_read() failed. malloc failed.");
    exit(-1);
  }

  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    free(data);
    return 1;
  }

  data->read = 1;
  data->offset = data->first_offset = offset;

  data->iov.iov_base = data->bytes;
  data->iov.iov_len = size;
  data->first_len = size;

  io_uring_prep_readv(sqe, infd, &data->iov, 1, offset);
  io_uring_sqe_set_data(sqe, data);
  return 0;
}

static void queue_write(struct io_uring *ring, struct io_data *data) {
  data->read = 0;
  data->offset = data->first_offset;

  data->iov.iov_base = data->bytes;
  data->iov.iov_len = data->first_len;

  queue_prepped(ring, data);
  io_uring_submit(ring);
}

int copy_file(struct io_uring *ring, off_t insize) {
  int ret;

  off_t write_left = insize;
  off_t offset = 0;
  unsigned long reads = 0;
  unsigned long writes = 0;

  while (insize || write_left) {
    /* Queue up as many reads as we can */
    bool is_sq_full = false;
    while (insize) {
      if (reads + writes >= QUEUE_DEPTH) {
        is_sq_full = true;
        break;
      }

      off_t this_size = min(insize, BLOCK_SZ);

      int queue_read_failed = queue_read(ring, this_size, offset);
      if (queue_read_failed) {
        is_sq_full = true;
        break;
      }

      insize -= this_size;
      offset += this_size;
      reads++;
    }

    /* Queue is not full. Submit new I/O requests */
    if (!is_sq_full) {
      ret = io_uring_submit(ring);
      if (ret < 0) {
        fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
        exit(-1);
      }
    }

    /* Queue is full at this point. Let's find at least one completion */
    int got_comp = 0;
    while (write_left) {
      struct io_uring_cqe *cqe;

      if (!got_comp) {
        ret = io_uring_wait_cqe(ring, &cqe);
        got_comp = 1;
      } else {
        ret = io_uring_peek_cqe(ring, &cqe);
        if (ret == -EAGAIN) {
          cqe = NULL;
          ret = 0;
        }
      }

      if (ret < 0) {
        fprintf(stderr, "io_uring_peek_cqe: %s\n", strerror(-ret));
        exit(-1);
      }

      if (!cqe) {
        break;
      }

      struct io_data *data = io_uring_cqe_get_data(cqe);
      if (cqe->res < 0) {
        if (cqe->res == -EAGAIN) {
          queue_prepped(ring, data);
          io_uring_cqe_seen(ring, cqe);
          continue;
        }
        fprintf(stderr, "cqe failed: %s\n", strerror(-cqe->res));
        exit(-1);
      }

      if (cqe->res != data->iov.iov_len) {
        /* short read/write; adjust and requeue */
        data->iov.iov_base += cqe->res;
        data->iov.iov_len -= cqe->res;
        queue_prepped(ring, data);
        io_uring_cqe_seen(ring, cqe);
        continue;
      }

      /*
       * All done. If write, nothing else to do. If read,
       * queue up corresponding write.
       * */

      if (data->read) {
        queue_write(ring, data);
        write_left -= data->first_len;
        reads--;
        writes++;
      } else {
        free(data);
        writes--;
      }

      io_uring_cqe_seen(ring, cqe);
    }
  }

  return 0;
}

int main(int argc, char *argv[]) {
  struct io_uring ring;
  off_t insize;
  int ret;

  if (argc < 3) {
    printf("Usage: %s <infile> <outfile>\n", argv[0]);
    return 1;
  }

  infd = open(argv[1], O_RDONLY);
  if (infd < 0) {
    perror("open infile");
    return 1;
  }

  outfd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (outfd < 0) {
    perror("open outfile");
    return 1;
  }

  setup_context(QUEUE_DEPTH, &ring);

  insize = get_file_size(infd);

  ret = copy_file(&ring, insize);

  close(infd);
  close(outfd);
  io_uring_queue_exit(&ring);
  return ret;
}