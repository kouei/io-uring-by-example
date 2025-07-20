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

#define QUEUE_DEPTH 32
#define BLOCK_SZ (16 * 1024)
#define min(x, y) ((x) < (y) ? (x) : (y))

static int infd, outfd;

enum io_type { READ, WRITE };

struct io_data {
  enum io_type type;
  off_t initial_offset;
  off_t initial_size;
  off_t offset;
  size_t size;
  struct iovec iov;
  char bytes[0]; /* Flexible Array. Real Data Payload */
};

static void setup_context(unsigned entries, struct io_uring *ring) {
  int ret = io_uring_queue_init(entries, ring, 0);
  if (ret < 0) {
    fprintf(stderr, "queue_init: %s\n", strerror(-ret));
    exit(-1);
  }
}

static off_t get_file_size(int fd) {
  struct stat st;

  if (fstat(fd, &st) < 0) {
    fprintf(stderr, "get_file_size() failed. fstat() failed.");
    exit(-1);
  }

  if (S_ISREG(st.st_mode)) {
    return st.st_size;
  }

  if (S_ISBLK(st.st_mode)) {
    off_t bytes;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
      fprintf(stderr, "get_file_size() failed. ioctl() failed.");
      exit(-1);
    }

    return bytes;
  }

  fprintf(stderr, "Unsupported st_mode = %u", st.st_mode);
  exit(-1);
}

struct io_data *allocate_io_data(enum io_type type, off_t size) {
  struct io_data *data = malloc(sizeof(*data) + size);
  if (!data) {
    fprintf(stderr, "allocate_io_data() failed.");
    exit(-1);
  }

  return data;
}

void deallocate_io_data(struct io_data *data) { free(data); }

static int prepare_sqe(struct io_uring *ring, struct io_data *data) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    return 1;
  }

  if (data->type == READ) {
    io_uring_prep_readv(sqe, infd, &data->iov, 1, data->offset);
  } else {
    io_uring_prep_writev(sqe, outfd, &data->iov, 1, data->offset);
  }

  io_uring_sqe_set_data(sqe, data);
  return 0;
}

void copy_file(struct io_uring *ring, off_t bytes_to_read) {
  off_t bytes_to_write = bytes_to_read;
  unsigned long read_tasks = 0;
  unsigned long write_tasks = 0;

  off_t read_offset = 0;
  while (bytes_to_read > 0 || bytes_to_write > 0) {
    /* Queue up as many reads as we can */
    bool is_any_new_read_task = false;
    while (bytes_to_read > 0) {
      if (read_tasks + write_tasks >= QUEUE_DEPTH) {
        break;
      }

      off_t read_size = min(bytes_to_read, BLOCK_SZ);

      struct io_data *data = allocate_io_data(READ, read_size);
      data->type = READ;
      data->initial_offset = read_offset;
      data->initial_size = read_size;
      data->offset = read_offset;
      data->size = read_size;
      data->iov.iov_base = data->bytes;
      data->iov.iov_len = read_size;

      if (prepare_sqe(ring, data)) {
        deallocate_io_data(data);
        break;
      }

      bytes_to_read -= read_size;
      read_offset += read_size;
      read_tasks += 1;
      is_any_new_read_task = true;
    }

    if (is_any_new_read_task) {
      int ret = io_uring_submit(ring); // Submit new read tasks to SQ
      if (ret < 0) {
        fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
        exit(-1);
      }
    }

    /*
     * Now we have submitted read tasks, let's deal with write tasks.
     * When shall we start any write task?
     * The answer is when some previous read task has completed.
     * That means we have to fetch some CQE before we can start a write task.
     * */
    struct io_uring_cqe *cqe;
    bool is_any_completed_task = false;
    while (bytes_to_write > 0) {
      int ret;
      if (!is_any_completed_task) {
        ret = io_uring_wait_cqe(ring, &cqe);
        is_any_completed_task = true;
      } else {
        ret = io_uring_peek_cqe(ring, &cqe);
        if (ret == -EAGAIN) { // EAGAIN means try again.
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
          if (prepare_sqe(ring, data)) {
            fprintf(stderr,
                    "Retry prepare_sqe() failed when handling EAGAIN error.");
            exit(-1);
          }
          io_uring_cqe_seen(ring, cqe);
          continue;
        }
        fprintf(stderr, "cqe failed: %s\n", strerror(-cqe->res));
        exit(-1);
      }

      if (cqe->res != data->iov.iov_len) {
        /* short read/write; adjust and requeue */
        data->offset += cqe->res;
        data->size -= cqe->res;
        data->iov.iov_base += cqe->res;
        data->iov.iov_len -= cqe->res;
        if (prepare_sqe(ring, data)) {
          fprintf(stderr,
                  "Retry prepare_sqe() failed when handling short read/write.");
          exit(-1);
        }
        io_uring_cqe_seen(ring, cqe);
        continue;
      }

      /*
       * All done. If write, nothing else to do. If read,
       * queue up corresponding write.
       * */

      if (data->type == READ) { // A read task has completed
        read_tasks -= 1;

        data->type = WRITE;
        data->offset = data->initial_offset;
        data->size = data->initial_size;
        data->iov.iov_base = data->bytes;
        data->iov.iov_len = data->size;
        if (prepare_sqe(ring, data)) {
          fprintf(stderr, "prepare_sqe() failed when preparing write task.");
          exit(-1);
        }

        io_uring_submit(ring);
        write_tasks += 1;
      } else { // A write task has completed
        bytes_to_write -= data->size;
        deallocate_io_data(data);
        write_tasks -= 1;
      }

      // Notify the kernel that a CQE has been consumed successfully.
      io_uring_cqe_seen(ring, cqe);
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <infile> <outfile>\n", argv[0]);
    exit(-1);
  }

  infd = open(argv[1], O_RDONLY);
  if (infd < 0) {
    fprintf(stderr, "open infile failed.");
    exit(-1);
  }

  outfd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (outfd < 0) {
    fprintf(stderr, "open outfile failed.");
    exit(-1);
  }

  struct io_uring ring;
  setup_context(QUEUE_DEPTH, &ring);

  off_t insize = get_file_size(infd);

  copy_file(&ring, insize);

  close(infd);
  close(outfd);
  io_uring_queue_exit(&ring);
  return 0;
}