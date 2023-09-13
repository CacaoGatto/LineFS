/**
 * @file ioat-dma.c
 * @author Insu Jang (insujang@casys.kaist.ac.kr)
 * @brief Kernel module for DMA operations with an I/OAT DMA engine.
 * @version 0.1
 * @date 2021-04-15
 * 
 * Reference articles
 * https://linux-kernel-labs.github.io/refs/heads/master/labs/device_drivers.html
 * https://forums.xilinx.com/xlnx/attachments/xlnx/ELINUX/31239/1/axidma.c
 */
// #define DEBUG
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/dax.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/list.h>
#include <linux/types.h>
#include "dax-private.h"
#include "ioat-dma.h"

/* https://stackoverflow.com/a/27901009 */
static struct dax_device *dax_get_device(const char *devpath) {
  struct inode *inode;
	struct path path;
	struct dax_device *dax_dev;
	struct dev_dax *dev_dax;

	int rc = kern_path(devpath, LOOKUP_FOLLOW, &path);
	if (rc) {
		return NULL;
	}
	inode = path.dentry->d_inode;
	dax_dev = inode_dax(inode);
	dev_dax = dax_get_private(dax_dev);
	if (dax_dev && dev_dax) {
		return dax_dev;
	}

	return NULL;
}

static int ioat_dma_open(struct inode *inode, struct file *file) {
  dev_dbg(dev, "%s\n", __func__);

  return 0;
}

static int ioat_dma_release(struct inode *inode, struct file *file) {
  struct ioat_dma_device *dma_device;
  list_for_each_entry(dma_device, &dma_devices, list) {
    if (dma_device->owner == current->tgid) {
      release_ioat_dma_device(dma_device);
    }
  }

  return 0;
}

static long ioat_dma_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  dev_dbg(dev, "%s\n", __func__);

  switch (cmd) {
    case IOCTL_IOAT_DMA_SUBMIT: {
      struct ioctl_dma_args args;
      struct dax_device *dax_device;
      struct dev_dax *dev_dax;
      struct ioat_dma_device *dma_device;

      if (copy_from_user(&args, (void __user *) arg, sizeof(args))) {
        return -EFAULT;
      }
      dev_dbg(dev, "dev name: %s, src offset: 0x%llx, dst offset: 0x%llx, size: 0x%llx\n",
              args.device_name, args.src_offset, args.dst_offset, args.size);

      dax_device = dax_get_device(args.device_name);
      if (dax_device == NULL) {
        return -ENODEV;
      }
      dev_dax = (struct dev_dax *)dax_get_private(dax_device);

      dma_device = find_ioat_dma_device(args.device_id);
      if (dma_device == NULL) return -ENODEV;

      return ioat_dma_ioctl_dma_submit(&args, dev_dax, dma_device);
    }
    case IOCTL_IOAT_DMA_GET_DEVICE_NUM: return ioat_dma_ioctl_get_device_num((void __user *) arg);
    case IOCTL_IOAT_DMA_GET_DEVICE: return ioat_dma_ioctl_get_device((void __user *) arg);
    case IOCTL_IOAT_DMA_WAIT_ALL: {
      struct ioctl_dma_wait_args args;
      struct ioat_dma_device *dma_device;
      int result;

      if (copy_from_user(&args, (void __user *) arg, sizeof(args))) {
        return -EFAULT;
      }

      dma_device = find_ioat_dma_device(args.device_id);
      if (dma_device == NULL) return -ENODEV;

      result = ioat_dma_ioctl_dma_wait_all(dma_device, &args.completed_dma_num);
      if (result) {
        return result;
      }
      
      if (copy_to_user((void __user *) arg, &args, sizeof(args))) {
        return -EFAULT;
      }

      return 0;
    }
    default:
      dev_warn(dev, "unsupported command %x\n", cmd);
  }

  return -EINVAL;
}


static struct file_operations ioat_dma_fops = {
  .open = ioat_dma_open,
  .release = ioat_dma_release,
  .unlocked_ioctl = ioat_dma_ioctl,
};

/**
 * =================================================================================
 */

static dev_t dev_;
static struct cdev cdev;
static struct class *class;
struct device *dev;
#define DEVICE_NAME "ioat-dma"
#define MAX_MINORS 5

/**
 * https://topic.alibabacloud.com/a/the-register_chrdev-and-register_chrdev_region-of-character-devices_8_8_31256510.html
 * Difference between register_chrdev and register_chrdev_region
 */
static int create_chardev(void) {
  int ret = 0;
  ret = alloc_chrdev_region(&dev_, 0, 1, DEVICE_NAME);
  if (ret < 0) {
    printk(KERN_ALERT "%s: alloc_chrdev_region failed with %d.\n", __func__, ret);
    return ret;
  }

  cdev_init(&cdev, &ioat_dma_fops);
  cdev_add(&cdev, dev_, 1);

  // Create /sys/class/ioat-dma in preparation of creating /dev/ioat-dma
  class = class_create(THIS_MODULE, DEVICE_NAME);
  if (IS_ERR(class)) {
    printk(KERN_ALERT "%s: class_create failed.\n", __func__);
    ret = PTR_ERR(class);
    goto unregister;
  }

  // Create /dev/ioat-dma
  dev = device_create(class, NULL, dev_, NULL, DEVICE_NAME);
  if (IS_ERR(dev)) {
    printk(KERN_ALERT "%s: device_create failed.\n", __func__);
    ret = PTR_ERR(dev);
    goto class_destroy;
  }

  return 0;

class_destroy:
  class_destroy(class);
unregister:
  unregister_chrdev_region(dev_, 1);
  return ret;
}

static int __init ioat_dma_init(void) {
  int ret;

  printk(KERN_INFO "%s\n", __func__);
  ret = create_chardev();
  if (ret < 0) {
    return ret;
  }

  return create_dma_devices();
}

static void __exit ioat_dma_exit(void) {
  struct ioat_dma_device *dma_device, *tmp;
  struct ioat_dma_completion_list_item *comp_entry, *comp_tmp;

  dev_dbg(dev, "%s\n", __func__);
  list_for_each_entry_safe(dma_device, tmp, &dma_devices, list) {
    list_del(&dma_device->list);

    list_for_each_entry_safe(comp_entry, comp_tmp, &dma_device->comp_list, list) {
      list_del(&comp_entry->list);
      kfree(comp_entry);
    }

    dma_release_channel(dma_device->chan);
    kfree(dma_device);
  }
  device_destroy(class, dev_);
  class_destroy(class);
  cdev_del(&cdev);
  unregister_chrdev_region(dev_, 1);
}

MODULE_AUTHOR("Insu Jang");
MODULE_LICENSE("GPL v2");
module_init(ioat_dma_init);
module_exit(ioat_dma_exit);