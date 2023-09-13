obj-m += ioat-dma.o

ioat-dma-y := ioat-dma-device.o
ioat-dma-y += ioat-dma-ioctl.o
ioat-dma-y += ioat-dma-mgr.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	$(CC) test.c -o test

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f test