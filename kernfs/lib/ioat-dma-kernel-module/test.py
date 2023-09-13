import unittest
import struct
import os
import errno
import fcntl
import mmap
import concurrent.futures
from ioctl_numbers import _IO, _IOR, _IOW, _IOWR

src_offset = 0x0
dst_offset = 0x10
size = 0x200000
data = os.urandom(size)    

ioctl_dma_get_device_num = _IOR(0xad, 0, 8)
ioctl_dma_get_device = _IOR(0xad, 1, 8)
ioctl_dma_submit = _IOW(0xad, 0, 64)
ioctl_dma_wait = _IOWR(0xad, 0, 16)

class TestIoatDma(unittest.TestCase):
    def setUp(self):
        self.dax = os.open("/dev/dax0.0", os.O_RDWR)
        self.assertGreater(self.dax, 0)
        self.ioat = os.open("/dev/ioat-dma", os.O_RDWR)
        self.assertGreater(self.ioat, 0)

    def tearDown(self):
        os.close(self.dax)
        os.close(self.ioat)

    def test_00_get_device_num(self):
        try:
            arg = struct.pack('Q', 0)
            result = fcntl.ioctl(self.ioat, ioctl_dma_get_device_num, arg)
            result = struct.unpack('Q', result)
            print("available devices: {}.".format(result[0]), end=' ', flush=True)
            self.assertGreater(result[0], 0)
        except OSError as err:
            self.fail("ioctl() returns an error: {}".format(err))
        
    def test_01_get_device(self):
        try:
            arg = struct.pack('Q', 0)
            result = fcntl.ioctl(self.ioat, ioctl_dma_get_device, arg)
            result = struct.unpack('Q', result)
            print("device id: {}.".format(result[0]), end=' ', flush=True)
            self.assertGreaterEqual(result[0], 0)
        except OSError as err:
            self.fail("ioctl() returns an error: {}".format(err))

    def test_02_get_different_device_for_another_request(self):
        try:
            arg = struct.pack('Q', 0)
            result = fcntl.ioctl(self.ioat, ioctl_dma_get_device, arg)
            id1 = struct.unpack('Q', result)[0]
            result = fcntl.ioctl(self.ioat, ioctl_dma_get_device, arg)
            id2 = struct.unpack('Q', result)[0]
            self.assertNotEqual(id1, id2)
        except OSError as err:
            self.fail("ioctl() returns an error: {}".format(err))

    def thread_function(self, ioat):
        arg = struct.pack('Q', 0)
        result = struct.unpack('Q', fcntl.ioctl(ioat, ioctl_dma_get_device, arg))[0]
        return result

    def test_03_get_different_device_for_different_threads(self):
        executor = concurrent.futures.ThreadPoolExecutor()
        future1 = executor.submit(self.thread_function, self.ioat)
        future2 = executor.submit(self.thread_function, self.ioat)
        self.assertNotEqual(future1.result(), future2.result())

    # @unittest.skip
    def test_11_dax_src_init(self):
        mm = mmap.mmap(self.dax, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                        mmap.ACCESS_DEFAULT, src_offset * size)
        mm.write(data)
        mm.seek(0)
        written = mm.read(size)
        mm.close()
        self.assertEqual(data, written)

    # @unittest.skip
    def test_12_ioat_dma(self):
        # 64sQQQ format (https://docs.python.org/3/library/struct.html#format-characters)
        # 64 bytes char[], 3 consecutive unsigned long longs, int
        # Equivalent to:
        # struct {
        # char device_name[64];
        # u64 src_offset;
        # u64 dst_offset;
        # u64 size;
        # }
        
        try:
            arg = struct.pack('Q', 0)
            id = struct.unpack('Q', fcntl.ioctl(self.ioat, ioctl_dma_get_device, arg))[0]
            arg = struct.pack('Q32sQQQ',
                            id,
                            '/dev/dax0.0'.encode(),
                            src_offset * size,
                            dst_offset * size,
                            size)
            fcntl.ioctl(self.ioat, ioctl_dma_submit, arg)
            arg = struct.pack('QQ',
                            id,
                            0)
            result = fcntl.ioctl(self.ioat, ioctl_dma_wait, arg)
            result = struct.unpack('QQ', result)
            self.assertEqual(result[1], 1)
        except OSError as err:
            self.fail("ioctl() returns an error: {}".format(err))

    # @unittest.skip
    def test_13_dax_dst_validate(self):
        mm = mmap.mmap(self.dax, size, mmap.MAP_SHARED, mmap.PROT_READ,
                        mmap.ACCESS_DEFAULT, dst_offset * size)
        dmaed = mm.read(size)
        mm.close()
        self.assertEqual(data, dmaed)

    def test_14_try_dma_without_device_get(self):
        try:
            arg = struct.pack('Q32sQQQ',
                            255, # not authorized device ID
                            '/dev/dax0.0'.encode(),
                            src_offset * size,
                            dst_offset * size,
                            size)
            fcntl.ioctl(self.ioat, ioctl_dma_submit, arg)
        except OSError as err:
            self.assertEqual(err.errno, errno.ENODEV)

    def test_15_multiple_dma_wait_once(self):
        try:
            arg = struct.pack('Q', 0)
            id = struct.unpack('Q', fcntl.ioctl(self.ioat, ioctl_dma_get_device, arg))[0]
            arg = struct.pack('Q32sQQQ',
                            id,
                            '/dev/dax0.0'.encode(),
                            src_offset * size,
                            dst_offset * size,
                            size)
            fcntl.ioctl(self.ioat, ioctl_dma_submit, arg)
            arg = struct.pack('Q32sQQQ',
                            id,
                            '/dev/dax0.0'.encode(),
                            src_offset * size * 2,
                            dst_offset * size * 2,
                            size)
            fcntl.ioctl(self.ioat, ioctl_dma_submit, arg)
            arg = struct.pack('QQ',
                            id,
                            0)
            result = fcntl.ioctl(self.ioat, ioctl_dma_wait, arg)
            result = struct.unpack('QQ', result)
            self.assertEqual(result[1], 2)
        except OSError as err:
            self.fail("ioctl() returns an error: {}".format(err))

# Reference article: https://stackoverflow.com/a/16364514
if __name__ == '__main__':
    print("Randomly generated data: {}... ({} bytes)".format(data[:16], len(data)))
    unittest.main(failfast=True, exit=False)