#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <sys/time.h>

/**
 * @brief This is a test code that compares performance between
 * DMA and manual memcpy().
 */

static unsigned long long src_offset = 0x0;
static unsigned long long dst_offset = 0x10;
static unsigned long long size = 0x2000000;

static char *generate_random_bytestream(size_t num_bytes) {
  char *stream = malloc(num_bytes);
  for (size_t i=0; i<num_bytes; i++) {
    stream[i] = rand();
  }
  return stream;
}

static int check_same_bytestream(const char *a, const char *b, size_t num_bytes) {
  for (size_t i=0; i<num_bytes; i++) {
    if (a[i] != b[i])
      return -1;
  }
  return 0;
}

double perform_dma(int ioat_fd, void *src, void *dst);
double perform_memcpy(void *src, void *dst);

int main() {
  int dax_fd = open("/dev/dax0.0", O_RDWR);
  int ioat_fd = open("/dev/ioat-dma", O_RDWR);

  if (dax_fd < 0 || ioat_fd < 0) {
    printf("Failed to open device\n");
    return -1;
  }

  void *src = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, dax_fd, src_offset * size);
  void *dst = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, dax_fd, dst_offset * size);
  if (!src || !dst) {
    printf("Failed to mmap\n");
    return -1;
  }

  printf("DMA vs memcpy (data size: 0x%llx bytes)\n", size);

  double dma_time = perform_dma(ioat_fd, src, dst);
  double memcpy_time = perform_memcpy(src, dst);

  printf("DMA: %lf s, memcpy: %lf s\n", dma_time, memcpy_time);
}

struct ioctl_dma_args {
  uint64_t device_id;
  char device_name[32];
  uint64_t src_offset;
  uint64_t dst_offset;
  uint64_t size;
} __attribute__ ((packed));

struct ioctl_dma_wait_args {
  uint64_t device_id;
  uint64_t result;
} __attribute__ ((packed));

#define IOCTL_IOAT_GET_DEVICE_ID _IOR(0xad, 1, uint64_t)
#define IOCTL_IOAT_DMA_SUBMIT _IOW(0xad, 0, struct ioctl_dma_args)
#define IOCTL_IOAT_DMA_WAIT _IOWR(0xad, 0, struct ioctl_dma_wait_args)
double perform_dma(int ioat_fd, void *src, void *dst) {
  struct timeval start, end;
  char *data = data = generate_random_bytestream(size);

  memcpy(src, data, size);
  if (check_same_bytestream(src, data, size)) {
    printf("src - data are different!");
    exit(1);
  }

  uint64_t device_id;
  ioctl(ioat_fd, IOCTL_IOAT_GET_DEVICE_ID, &device_id);

  struct ioctl_dma_args args = {
    .device_id = device_id,
    .device_name = "/dev/dax0.0",
    .src_offset = src_offset * size,
    .dst_offset = dst_offset * size,
    .size = size,
  };
  gettimeofday(&start, 0);
  int result = ioctl(ioat_fd, IOCTL_IOAT_DMA_SUBMIT, &args);
  gettimeofday(&end, 0);

  double submit_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) * 1e-6;

  struct ioctl_dma_wait_args args2 = {
    .device_id = device_id,
    .result = 0
  };
  gettimeofday(&start, 0);
  result = ioctl(ioat_fd, IOCTL_IOAT_DMA_WAIT, &args2);
  gettimeofday(&end, 0);

  double wait_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) * 1e-6;

  printf("%s: DMA submit time: %lf s, wait time: %lf s (# of DMA completed: %lu)\n",
          __func__, submit_time, wait_time, args2.result);

  if (check_same_bytestream(dst, data, size)) {
    printf("dst - data are different!");
    exit(1);
  }

  printf("%s: data verification done!\n", __func__);
  free(data);
  return submit_time + wait_time;
}

double perform_memcpy(void *src, void *dst) {
  struct timeval start, end;
  char *data = data = generate_random_bytestream(size);

  memcpy(src, data, size);
  if (check_same_bytestream(src, data, size)) {
    printf("src - data are different!");
    exit(1);
  }

  gettimeofday(&start, 0);
  memcpy(dst, src, size);
  gettimeofday(&end, 0);

  if (check_same_bytestream(dst, data, size)) {
    printf("dst - data are different!");
    exit(1);
  }

  printf("%s: data verification done!\n", __func__);
  free(data);
  return (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) * 1e-6;
}