/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <string.h>
#include "aesd-circular-buffer.h"
#include "aesdchar.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Biplav Poudel"); 
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("aesd_open() is invoked");
    struct aesd_dev *device;                                        /* for device information */
    device = container_of(inode->i_cdev, struct aesd_dev, cdev);    /* returns pointer to device state using its cdev field by matching with inode's cdev */
    filp->private_data = device;                                    /* pass pointer to device state for future access*/
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    /* Since we have not alloted anything in aesd_open, no need to do anything!
    filp->private_data can be safely ignored */
    struct aesd_dev *dev = filp->private_data;
    PDEBUG("ased_release() is invoked for device %p", dev);     /* %p is format specifier for pointer address of the aesd device*/
    return 0;
}

/**
 * @brief Returns the data (can be partial) in the order they were written to the userspace (*buf).
 *
 * @param filp    is a struct file pointer
 * @param buf     is a user-space pointer to buffer; can't be dereferenced by kernel for safety
 * @param count   specifies the maximum number of bytes that can be returned
 * @param f_pos   specifies the read position (location) of bytes to return
 *
 * @return size of data read
 *
 * @note The returned data needs to be recently written (from the latest 10 writes) based on position and size.
 * @note The function needs to be re-entrant and interruptible 
 */
ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    struct aesd_dev *dev = filp->private_data;      /* this aesdchar device will be locked for read operation*/

    /* we need to ensure no access to device struct is made without holding mutex*/
    if (mutex_lock_interruptible(&dev->lock)) 
        return -ERESTARTSYS;                        /* if "locking wait" was interrupted, we need to signal kernel to restart*/

    /* from f_pos, we only send count size of writes back */
    struct aesd_buffer_entry *return_entry;     /* not same as the entry used in aesdchar device state*/
    size_t entry_offset;        /* stores the offset from the beginning of the returned entry */
    return_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circ_buf, f_pos, &entry_offset);

    if (return_entry == NULL){
        PDEBUG("EOF is reached! aesd_read() returns %zu as there is no more data to read in %lld!", 0, *f_pos);
        retval = 0;
        goto out;
    }

    /* If the count is greater than the size of remaining entries, we just send the bytes for the remaining entries and update the *f_pos
    i.e. if device data = "hello\nabc\nxyz\n
         and count = 100, remaining = 4
         We return: xyz\n

    But, this assignment doesn't need me to read all the remaining bytes in a go.
    As per the instruction, each read can optionally return a portion of the total data available.
    i.e. only a single Assignment 8 write command! (full or partial depending upon the f_pos)

    As long as return values is set correctly and offset (*offp) is updated correctly,
    the application can retry the read to read all data until all available data is read. Happens automatically with fread (or cat)
    */

    size_t bytes_to_copy = min(return_entry->size - entry_offset, count);   // shouldn't exceed count size

    /** 
    * copy_to_user() returns number of bytes not copied.
    * 0 means all bytes copied.
    * non-zero means bytes remaining.
    */
    if (copy_to_user(buf, (void *) return_entry->buffptr + entry_offset, bytes_to_copy)){
        PDEBUG("Partial read encountered during aesd_read() operation");
        retval = -EFAULT;   // we return error if return value is non-zero, i.e. partial writes to userspace
        goto out;
    } 

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

    out:
        mutex_unlock(&dev->lock);
        return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    struct aesd_dev *dev = filp->private_data;      /* this aesdchar device will be locked for write operation*/
    struct aesd_buffer_entry *old_entry = &dev->entry;  /* stores previous partial writes in dev*/

    /* we need to ensure no access to device struct is made without holding mutex*/
    if (mutex_lock_interruptible(&dev->lock)) 
        return -ERESTARTSYS;                        /* if "locking wait" was interrupted, we need to signal kernel to restart*/

    if (old_entry->buffptr){
        /* we allocate a new temporary entry and set it to zero before writing partial writes in its buffer_ptr!*/
        struct aesd_buffer_entry *new_entry = kzalloc(sizeof(struct aesd_buffer_entry), GFP_KERNEL);
        if (!new_entry){
            PDEBUG("Couldn't allocate memory for new temp entry. No kfree() needed here!");
            goto out;   //retval already set to -ENOMEM
        }

        /* now we allocate space for buffer ptr inside new_entry */
        new_entry->buffptr = kmalloc(old_entry->size + count, GFP_KERNEL);
        new_entry->size = 0;
        if (!new_entry->buffptr){
            PDEBUG("Error allocating memory for buffptr. Freeing the memory alloted for tmp entry!");
            kfree(new_entry);
            goto out;
        }

        size_t old_entry_size_copy = old_entry->size;

        /* Now we copy the old entry with its partial writes to bigger and recently allocated entry*/
        memcpy( (void *) new_entry->buffptr, old_entry->buffptr, old_entry->size);
        new_entry->size = old_entry->size;
        kfree(old_entry->buffptr);
        old_entry->buffptr = NULL;
        old_entry->size = 0;
    }



    out:
        mutex_unlock(&dev->lock);
        return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
