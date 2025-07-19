#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

/* If your compilation fails because the header file below is missing,
 * your kernel is probably too old to support io_uring.
 * */
#include <linux/io_uring.h>

#define QUEUE_DEPTH 1
#define BLOCK_SZ 1024

/* This is x86 specific */
#define read_barrier() __asm__ __volatile__("" ::: "memory")
#define write_barrier() __asm__ __volatile__("" ::: "memory")

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))
#define div_round_up(x, y) (((x) + (y) - 1) / (y))

struct app_io_sq_ring {
  unsigned *head;
  unsigned *tail;
  unsigned *ring_mask;
  unsigned *ring_entries;
  unsigned *flags;
  unsigned *array;
};

struct app_io_cq_ring {
  unsigned *head;
  unsigned *tail;
  unsigned *ring_mask;
  unsigned *ring_entries;
  struct io_uring_cqe *cqes;
};

struct submitter {
  int ring_fd;
  struct app_io_sq_ring sq_ring;
  struct io_uring_sqe *sqes;
  struct app_io_cq_ring cq_ring;
};

struct file_info {
  off_t file_sz;
  struct iovec iovecs[0]; /* Flexible Array. Referred by readv/writev */
};

/*
 * This code is written in the days when io_uring-related system calls are not
 * part of standard C libraries. So, we roll our own system call wrapper
 * functions.
 * */

int io_uring_setup(unsigned entries, struct io_uring_params *params) {
  return (int)syscall(__NR_io_uring_setup, entries, params);
}

int io_uring_enter(int ring_fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags) {
  return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                      flags, NULL, 0);
}

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
  void *buf = NULL;

  if (!posix_memalign(&buf, alignment, size)) {
    fprintf(stderr, "posix_memalign");
    exit(-1);
  }

  return buf;
}

void print_io_uring_params(struct io_uring_params *params) {
  fprintf(stdout, "params->sq_entries = %u\n", params->sq_entries);
  fprintf(stdout, "params->cq_entries = %u\n", params->cq_entries);
  fprintf(stdout, "params->flags = %u\n", params->flags);
  fprintf(stdout, "params->sq_thread_cpu = %u\n", params->sq_thread_cpu);
  fprintf(stdout, "params->sq_thread_idle = %u\n", params->sq_thread_idle);
  fprintf(stdout, "params->features = %u\n", params->features);
  fprintf(stdout, "params->wq_fd = %u\n\n", params->wq_fd);

  fprintf(stdout, "params->sq_off.head = %u\n", params->sq_off.head);
  fprintf(stdout, "params->sq_off.tail = %u\n", params->sq_off.tail);
  fprintf(stdout, "params->sq_off.ring_mask = %u\n", params->sq_off.ring_mask);
  fprintf(stdout, "params->sq_off.ring_entries = %u\n",
          params->sq_off.ring_entries);
  fprintf(stdout, "params->sq_off.flags = %u\n", params->sq_off.flags);
  fprintf(stdout, "params->sq_off.dropped = %u\n", params->sq_off.dropped);
  fprintf(stdout, "params->sq_off.array = %u\n", params->sq_off.array);
  fprintf(stdout, "params->sq_off.resv1 = %u\n", params->sq_off.resv1);
  fprintf(stdout, "params->sq_off.user_addr = %llu\n\n",
          params->sq_off.user_addr);

  fprintf(stdout, "params->cq_off.head = %u\n", params->cq_off.head);
  fprintf(stdout, "params->cq_off.tail = %u\n", params->cq_off.tail);
  fprintf(stdout, "params->cq_off.ring_mask = %u\n", params->cq_off.ring_mask);
  fprintf(stdout, "params->cq_off.ring_entries = %u\n",
          params->cq_off.ring_entries);
  fprintf(stdout, "params->cq_off.overflow = %u\n", params->cq_off.overflow);
  fprintf(stdout, "params->cq_off.cqes = %u\n", params->cq_off.cqes);
  fprintf(stdout, "params->cq_off.flags = %u\n", params->cq_off.flags);
  fprintf(stdout, "params->cq_off.resv1 = %u\n", params->cq_off.resv1);
  fprintf(stdout, "params->cq_off.user_addr = %llu\n",
          params->cq_off.user_addr);
}

/*
 * io_uring requires a lot of setup which looks pretty hairy, but isn't all
 * that difficult to understand. Because of all this boilerplate code,
 * io_uring's author has created liburing, which is relatively easy to use.
 * However, you should take your time and understand this code. It is always
 * good to know how it all works underneath. Apart from bragging rights,
 * it does offer you a certain strange geeky peace.
 * */

int app_setup_uring(struct submitter *submitter) {
  /*
   * We need to pass in the io_uring_params structure to the io_uring_setup()
   * call zeroed out. We could set any flags if we need to, but for this
   * example, we don't.
   * */
  struct io_uring_params params = {}; // Has to be zeroed out
  submitter->ring_fd = io_uring_setup(QUEUE_DEPTH, &params);
  if (submitter->ring_fd < 0) {
    fprintf(stderr, "io_uring_setup");
    exit(-1);
  }

  print_io_uring_params(&params);

  /*
   * io_uring communication happens via 2 shared kernel-user space ring buffers,
   * which can be jointly mapped with a single mmap() call in recent kernels.
   * While the completion queue is directly manipulated, the submission queue
   * has an indirection array in between. We map that in as well.
   * */

  int sring_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
  int cring_sz =
      params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);

  /* In kernel version 5.4 and above, it is possible to map the submission and
   * completion buffers with a single mmap() call. Rather than check for kernel
   * versions, the recommended way is to just check the features field of the
   * io_uring_params structure, which is a bit mask. If the
   * IORING_FEAT_SINGLE_MMAP is set, then we can do away with the second mmap()
   * call to map the completion ring.
   * */
  void *sq_ptr, *cq_ptr;
  if (params.features & IORING_FEAT_SINGLE_MMAP) {
    sring_sz = max(sring_sz, cring_sz);

    /* Map in the submission and completion queue ring buffers.
     * Older kernels only map in the submission queue, though.
     * */
    sq_ptr =
        mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
             submitter->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
      fprintf(stderr, "mmap");
      exit(-1);
    }

    cq_ptr = sq_ptr;
  } else {
    /* Map in the submission and completion queue ring buffers.
     * Older kernels only map in the submission queue, though.
     * */
    sq_ptr =
        mmap(NULL, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
             submitter->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
      fprintf(stderr, "mmap");
      exit(-1);
    }

    /* Map in the completion queue ring buffer in older kernels separately */
    cq_ptr =
        mmap(NULL, cring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
             submitter->ring_fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) {
      fprintf(stderr, "mmap");
      exit(-1);
    }
  }
  /* Save useful fields in a global app_io_sq_ring struct for later
   * easy reference */

  submitter->sq_ring.head = sq_ptr + params.sq_off.head;
  submitter->sq_ring.tail = sq_ptr + params.sq_off.tail;
  submitter->sq_ring.ring_mask = sq_ptr + params.sq_off.ring_mask;
  submitter->sq_ring.ring_entries = sq_ptr + params.sq_off.ring_entries;
  submitter->sq_ring.flags = sq_ptr + params.sq_off.flags;
  submitter->sq_ring.array = sq_ptr + params.sq_off.array;

  /* Map in the submission queue entries array */
  submitter->sqes = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
                         PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         submitter->ring_fd, IORING_OFF_SQES);
  if (submitter->sqes == MAP_FAILED) {
    fprintf(stderr, "mmap");
    exit(-1);
  }

  /* Save useful fields in a global app_io_cq_ring struct for later
   * easy reference */
  submitter->cq_ring.head = cq_ptr + params.cq_off.head;
  submitter->cq_ring.tail = cq_ptr + params.cq_off.tail;
  submitter->cq_ring.ring_mask = cq_ptr + params.cq_off.ring_mask;
  submitter->cq_ring.ring_entries = cq_ptr + params.cq_off.ring_entries;
  submitter->cq_ring.cqes = cq_ptr + params.cq_off.cqes;

  return 0;
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
 * Read from completion queue.
 * In this function, we read completion events from the completion queue, get
 * the data buffer that will have the file data and print it to the console.
 * */

void read_from_cq(struct submitter *s) {
  struct file_info *fi;
  struct app_io_cq_ring *cring = &s->cq_ring;
  struct io_uring_cqe *cqe;
  unsigned head = 0;

  head = *cring->head;

  do {
    read_barrier();
    /*
     * Remember, this is a ring buffer. If head == tail, it means that the
     * buffer is empty.
     * */
    if (head == *cring->tail)
      break;

    /* Get the entry */
    cqe = &cring->cqes[head & *s->cq_ring.ring_mask];
    fi = (struct file_info *)cqe->user_data;
    if (cqe->res < 0)
      fprintf(stderr, "Error: %s\n", strerror(abs(cqe->res)));

    int blocks = div_round_up(fi->file_sz, BLOCK_SZ);

    for (int i = 0; i < blocks; i++) {
      output_to_console(fi->iovecs[i].iov_base, fi->iovecs[i].iov_len);
    }

    head++;
  } while (1);

  *cring->head = head;
  write_barrier();
}
/*
 * Submit to submission queue.
 * In this function, we submit requests to the submission queue. You can submit
 * many types of requests. Ours is going to be the readv() request, which we
 * specify via IORING_OP_READV.
 *
 * */
int submit_to_sq(char *file_path, struct submitter *s) {
  struct file_info *fi;

  int file_fd = open(file_path, O_RDONLY);
  if (file_fd < 0) {
    fprintf(stderr, "open");
    exit(-1);
  }

  struct app_io_sq_ring *sring = &s->sq_ring;
  unsigned index = 0, current_block = 0, tail = 0;

  off_t file_sz = get_file_size(file_fd);
  if (file_sz < 0) {
    exit(-1);
  }

  off_t bytes_remaining = file_sz;
  int blocks = div_round_up(file_sz, BLOCK_SZ);

  fi = malloc(sizeof(*fi) + sizeof(struct iovec) * blocks);
  if (!fi) {
    fprintf(stderr, "Unable to allocate memory\n");
    exit(-1);
  }
  fi->file_sz = file_sz;

  /*
   * For each block of the file we need to read, we allocate an iovec struct
   * which is indexed into the iovecs array. This array is passed in as part
   * of the submission. If you don't understand this, then you need to look
   * up how the readv() and writev() system calls work.
   * */
  while (bytes_remaining) {
    off_t bytes_to_read = min(bytes_remaining, BLOCK_SZ);

    fi->iovecs[current_block].iov_len = bytes_to_read;

    void *buf = aligned_alloc(BLOCK_SZ, BLOCK_SZ);
    if (!buf) {
      fprintf(stderr, "aligned_alloc");
      exit(-1);
    }
    fi->iovecs[current_block].iov_base = buf;

    current_block++;
    bytes_remaining -= bytes_to_read;
  }

  /* Add our submission queue entry to the tail of the SQE ring buffer */
  tail = *sring->tail;
  read_barrier();
  index = tail & *s->sq_ring.ring_mask;
  struct io_uring_sqe *sqe = &s->sqes[index];
  sqe->fd = file_fd;
  sqe->flags = 0;
  sqe->opcode = IORING_OP_READV;
  sqe->addr = (unsigned long)fi->iovecs;
  sqe->len = blocks;
  sqe->off = 0;
  sqe->user_data = (unsigned long long)fi;
  sring->array[index] = index;
  tail++;

  /* Update the tail so the kernel can see it. */
  if (*sring->tail != tail) {
    *sring->tail = tail;
    write_barrier();
  }

  /*
   * Tell the kernel we have submitted events with the io_uring_enter() system
   * call. We also pass in the IOURING_ENTER_GETEVENTS flag which causes the
   * io_uring_enter() call to wait until min_complete events (the 3rd param)
   * complete.
   * */
  int ret = io_uring_enter(s->ring_fd, 1, 1, IORING_ENTER_GETEVENTS);
  if (ret < 0) {
    fprintf(stderr, "io_uring_enter");
    return 1;
  }

  return 0;
}

int main(int argc, char *argv[]) {
  struct submitter submitter = {}; //

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
    exit(-1);
  }

  if (app_setup_uring(&submitter)) {
    fprintf(stderr, "Unable to setup uring!\n");
    exit(-1);
  }

  for (int i = 1; i < argc; i++) {
    if (submit_to_sq(argv[i], &submitter)) {
      fprintf(stderr, "Error reading file\n");
      exit(-1);
    }

    read_from_cq(&submitter);
  }

  return 0;
}