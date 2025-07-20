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

struct io_task {
  bool is_read;
  off_t initial_offset;
  off_t offset;
  size_t initial_len;
  struct iovec iov;
  char bytes[0]; /* Flexible Array. Real Data Payload. */
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
    fprintf(stderr, "fstat() failed.");
    exit(-1);
  }

  if (S_ISREG(st.st_mode)) {
    return st.st_size;
  }

  if (S_ISBLK(st.st_mode)) {
    off_t bytes;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
      fprintf(stderr, "ioctl() failed.");
      exit(-1);
    }

    return bytes;
  }

  fprintf(stderr, "Unsupported st_mode = %u", st.st_mode);
  exit(-1);
}

static void requeue_task(struct io_uring *ring, struct io_task *task) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (sqe == NULL) {
    fprintf(stderr, "io_uring_get_sqe() failed.");
    exit(-1);
  }

  if (task->is_read) {
    io_uring_prep_readv(sqe, infd, &task->iov, 1, task->offset);
  } else {
    io_uring_prep_writev(sqe, outfd, &task->iov, 1, task->offset);
  }

  io_uring_sqe_set_data(sqe, task);
}

static int queue_read(struct io_uring *ring, off_t size, off_t offset) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (sqe == NULL) {
    return -1;
  }

  struct io_task *task = malloc(sizeof(*task) + size);
  if (!task) {
    return -1;
  }

  task->is_read = true;
  task->initial_offset = offset;
  task->offset = offset;
  task->initial_len = size;

  task->iov.iov_base = task->bytes;
  task->iov.iov_len = task->initial_len;

  io_uring_prep_readv(sqe, infd, &task->iov, 1, offset);
  io_uring_sqe_set_data(sqe, task);
  return 0;
}

static void queue_write(struct io_uring *ring, struct io_task *task) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (sqe == NULL) {
    fprintf(stderr, "io_uring_get_sqe() failed.");
    exit(-1);
  }

  task->is_read = false;
  task->offset = task->initial_offset;

  task->iov.iov_base = task->bytes;
  task->iov.iov_len = task->initial_len;

  io_uring_prep_writev(sqe, outfd, &task->iov, 1, task->offset);
  io_uring_sqe_set_data(sqe, task);
}

void spawn_read_tasks(struct io_uring *ring, unsigned long *read_tasks,
                      unsigned long *write_tasks, off_t *bytes_to_read,
                      off_t *read_offset) {
  /* Queue up as many reads as we can */
  unsigned long previous_read_tasks = *read_tasks;
  while (*bytes_to_read > 0) {
    if (*read_tasks + *write_tasks >= QUEUE_DEPTH) {
      break;
    }

    off_t read_size = min(*bytes_to_read, BLOCK_SZ);

    int ret = queue_read(ring, read_size, *read_offset);
    if (ret < 0) {
      break;
    }

    *bytes_to_read -= read_size;
    *read_offset += read_size;
    *read_tasks += 1;
  }

  if (previous_read_tasks < *read_tasks) {
    int ret = io_uring_submit(ring);
    if (ret < 0) {
      fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
      exit(-1);
    }
  }
}

void spawn_write_tasks(struct io_uring *ring, off_t *bytes_to_write,
                       unsigned long *read_tasks, unsigned long *write_tasks) {
  /* Queue is full at this point. Let's find at least one completion */
  bool already_found_completed_task = false;
  while (*bytes_to_write > 0) {
    struct io_uring_cqe *cqe;
    if (!already_found_completed_task) {
      int ret = io_uring_wait_cqe(ring, &cqe);
      if (ret < 0) {
        fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-ret));
        exit(-1);
      }

      already_found_completed_task = true;
    } else {
      int ret = io_uring_peek_cqe(ring, &cqe);
      if (ret == -EAGAIN) { // EAGAIN means retry. It also means currently CQ
                            // is empty.
        break;
      }

      if (ret < 0) {
        fprintf(stderr, "io_uring_peek_cqe: %s\n", strerror(-ret));
        exit(-1);
      }
    }

    struct io_task *task = io_uring_cqe_get_data(cqe);
    if (cqe->res == -EAGAIN) { // EAGAIN means retry.
      requeue_task(ring, task);
      /* Notify kernel that a CQE has been consumed successfully. */
      io_uring_cqe_seen(ring, cqe);
      continue;
    }

    if (cqe->res < 0) {
      fprintf(stderr, "cqe failed: %s\n", strerror(-cqe->res));
      exit(-1);
    }

    if (cqe->res != task->iov.iov_len) {
      /* short read/write; adjust and requeue */
      task->iov.iov_base += cqe->res;
      task->iov.iov_len -= cqe->res;
      requeue_task(ring, task);
      /* Notify kernel that a CQE has been consumed successfully. */
      io_uring_cqe_seen(ring, cqe);
      continue;
    }

    /*
     * All done. If write, nothing else to do. If read,
     * queue up corresponding write.
     * */
    if (task->is_read) {
      queue_write(ring, task);
      io_uring_submit(ring);
      *read_tasks -= 1;
      *write_tasks += 1;
    } else {
      *bytes_to_write -= task->initial_len;
      free(task);
      *write_tasks -= 1;
    }

    io_uring_cqe_seen(ring, cqe);
  }
}

int copy_file(struct io_uring *ring, off_t bytes_to_read) {
  off_t bytes_to_write = bytes_to_read;
  unsigned long read_tasks = 0;
  unsigned long write_tasks = 0;

  off_t read_offset = 0;
  while (bytes_to_read > 0 || bytes_to_write > 0) {
    spawn_read_tasks(ring, &read_tasks, &write_tasks, &bytes_to_read,
                     &read_offset);
    spawn_write_tasks(ring, &bytes_to_write, &read_tasks, &write_tasks);
  }

  return 0;
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <infile> <outfile>\n", argv[0]);
    exit(-1);
  }

  infd = open(argv[1], O_RDONLY);
  if (infd < 0) {
    fprintf(stderr, "open infile");
    exit(-1);
  }

  outfd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (outfd < 0) {
    fprintf(stderr, "open outfile");
    exit(-1);
  }

  struct io_uring ring;
  setup_context(QUEUE_DEPTH, &ring);

  off_t insize = get_file_size(infd);

  int ret = copy_file(&ring, insize);

  close(infd);
  close(outfd);
  io_uring_queue_exit(&ring);
  return ret;
}