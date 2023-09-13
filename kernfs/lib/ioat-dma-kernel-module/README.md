# IOAT-DMA for devdax (device-dax) persistent memory module

This repository contains the following:
- `ioat-dma.ko` (built from `ioat-dma.c`): a kernel module that initiates DMA to a DMA device that supports `DMA_MEMCPY` capability, and handles DMA completion via `ioctl()`. It depends `dax-private.h`, which has been borrowed from Linux kernel (`linux/drivers/dax`).
- `test.py` (and `ioctl_numbers.py`): a unittest file built with Python unittest package.
- `test.c`: a test file that compares performance (DMA vs `memcpy`).

## Build and Test
Tested under Ubuntu 20.04.2 LTS kernel 5.4.0-66-generic.
Might not work with higher or lower version of kernel due to inconsistency of `dax-private.h`.
Sources for `dax-private.h` has been borrowed from [Linux kernel 5.6](https://github.com/torvalds/linux/tree/v5.6).

> `/dev/daxX.X` is required before executing test files.
> There is no order dependency between inserting the kernel module and binding DAX devices.

```
$ make
$ sudo $(ASSISE)/utils/use_dax.sh bind
$ sudo insmod ioat-dma.ko
$ sudo python3 test.py -vv
Randomly generated data: b'(randomly generated)'... (2097152 bytes)
test_01_dax_src_init (__main__.TestIoatDma) ... ok
test_02_ioat_dma (__main__.TestIoatDma) ... ok
test_03_dax_dst_validate (__main__.TestIoatDma) ... ok

----------------------------------------------------------------------
Ran 3 tests in 0.013s

OK
$ sudo ./test
DMA vs memcpy (data size: 0x2000000 bytes)
perform_dma: data verification done!
perform_memcpy: data verification done!
DMA: 0.008604 s, memcpy: 0.015450 s
```

## Using `ioat-dma` Kernel Module

`ioat-dma` receives a request from userspace processes via `ioctl()`, and its ioctl magic number is `0xad`.

> You can also refer to the function `perform_dma()` in `test.c`.

```c
struct ioctl_dma_args {
  char device_name[64];
  uint64_t src_offset;
  uint64_t dst_offset;
  uint64_t size;
} __attribute__ ((packed));
#define IOCTL_IOAT_DMA_SUBMIT _IOW(0xad, 0, struct ioctl_dma_args)

int fd = open("/dev/ioat-dma", O_RDWR);
struct ioctl_dma_args args = {
  .device_name = "/dev/dax0.0",
  .src_offset = src_offset,
  .dst_offset = dst_offset,
  .size = size,
};
ioctl(fd, IOCTL_IOAT_DMA_SUBMIT, &args);
...
```

This will initiate DMA, copying data from `[src_offset, src_offset + size)` to `[dst_offset, dst_offset + size)` with a properly chosen DMA engine.