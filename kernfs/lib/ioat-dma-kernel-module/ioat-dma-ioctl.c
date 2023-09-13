// #define DEBUG
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/spinlock.h>
#include "ioat-dma.h"

int ioat_dma_ioctl_get_device_num(void __user *arg) {
  if (copy_to_user(arg, &n_dma_devices, sizeof(n_dma_devices))) {
    return -EFAULT;
  }
  return 0;
}

int ioat_dma_ioctl_get_device(void __user *arg) {
  struct ioat_dma_device *dma_device = get_available_ioat_dma_device();
  if (IS_ERR(dma_device)) {
    return PTR_ERR(dma_device);
  }

  if (copy_to_user((void __user *) arg, &dma_device->device_id, sizeof(dma_device->device_id))) {
    release_ioat_dma_device(dma_device);
    return -EFAULT;
  }

  return 0;
}



static void dma_sync_callback(void *completion) {
  complete(completion);
}

int ioat_dma_ioctl_dma_submit(struct ioctl_dma_args *args, struct dev_dax *dev_dax, struct ioat_dma_device *dma_device) {
  struct resource *res = &dev_dax->region->res;
  struct page *page = pfn_to_page(res->start >> PAGE_SHIFT);
  enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
  
  dma_addr_t src, dst;
  struct dma_async_tx_descriptor *chan_desc;
  struct ioat_dma_completion_list_item *comp_entry;

  unsigned long lock_flags;

  if (IS_ERR(page)) return PTR_ERR(page);

  src = dma_map_page(dma_device->chan->device->dev, page, args->src_offset, args->size, DMA_TO_DEVICE);
  dst = dma_map_page(dma_device->chan->device->dev, page, args->dst_offset, args->size, DMA_FROM_DEVICE);
  dev_dbg(dev, "%s: DMA about to be initialized: 0x%llx -> 0x%llx (size: 0x%llx bytes)\n",
          __func__, src, dst, args->size);
  
  chan_desc = dmaengine_prep_dma_memcpy(dma_device->chan, dst, src, args->size, flags);
  if (chan_desc == NULL) {
    dma_unmap_page(dma_device->chan->device->dev, src, args->size, DMA_BIDIRECTIONAL);
    dma_unmap_page(dma_device->chan->device->dev, dst, args->size, DMA_BIDIRECTIONAL);
    return -EINVAL;
  }

  comp_entry = kzalloc(sizeof(struct ioat_dma_completion_list_item), GFP_KERNEL);
  init_completion(&comp_entry->comp);
  chan_desc->callback = dma_sync_callback;
  chan_desc->callback_param = &comp_entry->comp;
  comp_entry->cookie = dmaengine_submit(chan_desc);
  comp_entry->src = src;
  comp_entry->dst = dst;
  comp_entry->size = args->size;

  dma_async_issue_pending(dma_device->chan);

  spin_lock_irqsave(&dma_device->comp_list_lock, lock_flags);
  list_add_tail(&comp_entry->list, &dma_device->comp_list);
  spin_unlock_irqrestore(&dma_device->comp_list_lock, lock_flags);

  return 0;
}

int ioat_dma_ioctl_dma_wait_all(struct ioat_dma_device *dma_device, u64 *result) {
  unsigned long flags;
  struct ioat_dma_completion_list_item *comp, *tmp;

  unsigned long timeout;
  enum dma_status status;
  int dma_result = 0;
  u64 num_completed = 0;

  spin_lock_irqsave(&dma_device->comp_list_lock, flags);

  list_for_each_entry_safe(comp, tmp, &dma_device->comp_list, list) {
    timeout = wait_for_completion_timeout(&comp->comp, msecs_to_jiffies(5000));
    status = dma_async_is_tx_complete(dma_device->chan, comp->cookie, NULL, NULL);
    dev_dbg(dev, "%s: wait completed.\n", __func__);

    if (timeout == 0) {
      dev_warn(dev, "%s: DMA timed out!\n", __func__);
      dma_result = -ETIMEDOUT;
    } else if (status != DMA_COMPLETE) {
      dev_warn(dev, "%s: DMA returned completion callback status of: %s\n",
        __func__, status == DMA_ERROR ? "error" : "in progress");
      dma_result = -EBUSY;
    } else {
      dev_dbg(dev, "%s: DMA completed!\n", __func__);
      num_completed++;
    }
    list_del(&comp->list);
    dma_unmap_page(dma_device->chan->device->dev, comp->src, comp->size, DMA_BIDIRECTIONAL);
    dma_unmap_page(dma_device->chan->device->dev, comp->dst, comp->size, DMA_BIDIRECTIONAL);
    kfree(comp);
    if (dma_result != 0) {
      break;
    }
  }

  spin_unlock_irqrestore(&dma_device->comp_list_lock, flags);
  *result = num_completed;
  return dma_result;
}