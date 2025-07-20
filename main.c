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
  if (data == NULL) {
    fprintf(stderr, "allocate_io_data() failed.");
    exit(-1);
  }

  return data;
}

void deallocate_io_data(struct io_data *data) { free(data); }

static void prepare_sqe(struct io_uring_sqe *sqe, struct io_data *data) {
  if (data->type == READ) {
    io_uring_prep_readv(sqe, infd, &data->iov, 1, data->offset);
  } else {
    io_uring_prep_writev(sqe, outfd, &data->iov, 1, data->offset);
  }

  io_uring_sqe_set_data(sqe, data);
}

void spawn_read_tasks(struct io_uring *ring, off_t *bytes_to_read,
                      off_t *read_offset) {
  /* Queue up as many reads as we can */
  bool is_any_new_task = false;
  while (*bytes_to_read > 0) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) { // SQ is full
      break;
    }

    off_t read_size = min(*bytes_to_read, BLOCK_SZ);

    struct io_data *data = allocate_io_data(READ, read_size);
    data->type = READ;
    data->initial_offset = *read_offset;
    data->initial_size = read_size;
    data->offset = data->initial_offset;
    data->size = data->initial_size;
    data->iov.iov_base = data->bytes;
    data->iov.iov_len = data->initial_size;
    prepare_sqe(sqe, data);

    *bytes_to_read -= read_size;
    *read_offset += read_size;
    is_any_new_task = true;
  }

  if (!is_any_new_task) {
    return;
  }

  int ret = io_uring_submit(ring); // Submit new read tasks to SQ
  if (ret < 0) {
    fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
    exit(-1);
  }
}

void spawn_write_tasks(struct io_uring *ring, off_t *bytes_to_write) {
  /*
   * Now we have submitted read tasks, let's deal with write tasks.
   * When shall we start any write task?
   * The answer is when some previous read task has completed.
   * That means we have to fetch some CQE before we can start a write task.
   * */
  bool is_any_new_task = false;
  while (*bytes_to_write > 0) {
    struct io_uring_cqe *cqe;

    int ret = io_uring_peek_cqe(ring, &cqe);
    if (ret == -EAGAIN) { // EAGAIN means try again. Which means currently
                          // there is no CQE available.
      usleep(50 * 1000);  // Sleep 50 ms
      break;
    }

    if (ret < 0) {
      fprintf(stderr, "io_uring_peek_cqe: %s\n", strerror(-ret));
      exit(-1);
    }

    struct io_data *data = io_uring_cqe_get_data(cqe);

    if (cqe->res == -EAGAIN) { // EAGAIN means try again.
      struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
      if (sqe == NULL) { // SQ is full
        break;
      }

      // Requeue the same task
      prepare_sqe(sqe, data);
      io_uring_cqe_seen(ring, cqe);
      is_any_new_task = true;
      continue;
    }

    if (cqe->res < 0) {
      fprintf(stderr, "cqe failed: %s\n", strerror(-cqe->res));
      exit(-1);
    }

    /* short read/write; update offset and size, then requeue the task */
    if (cqe->res != data->iov.iov_len) {
      struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
      if (sqe == NULL) { // SQ is full
        break;
      }

      data->offset += cqe->res;
      data->size -= cqe->res;
      data->iov.iov_base += cqe->res;
      data->iov.iov_len -= cqe->res;

      // Requeue the updated task
      prepare_sqe(sqe, data);
      io_uring_cqe_seen(ring, cqe);
      is_any_new_task = true;
      continue;
    }

    /*
     * All done. If write, nothing else to do. If read,
     * queue up corresponding write.
     * */
    if (data->type == READ) { // A read task has completed
      struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
      if (sqe == NULL) { // SQ is full
        break;
      }

      data->type = WRITE;
      data->offset = data->initial_offset;
      data->size = data->initial_size;
      data->iov.iov_base = data->bytes;
      data->iov.iov_len = data->size;
      prepare_sqe(sqe, data);
      is_any_new_task = true;
    } else { // A write task has completed
      *bytes_to_write -= data->initial_size;
      deallocate_io_data(data);
    }

    // Notify the kernel that a CQE has been consumed successfully.
    io_uring_cqe_seen(ring, cqe);
  }

  if (!is_any_new_task) {
    return;
  }

  int ret = io_uring_submit(ring); // Submit new tasks to SQ
  if (ret < 0) {
    fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
    exit(-1);
  }
}

void copy_file(struct io_uring *ring, off_t bytes_to_read) {
  off_t bytes_to_write = bytes_to_read;
  off_t read_offset = 0;
  while (bytes_to_read > 0 || bytes_to_write > 0) {
    spawn_read_tasks(ring, &bytes_to_read, &read_offset);
    spawn_write_tasks(ring, &bytes_to_write);
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