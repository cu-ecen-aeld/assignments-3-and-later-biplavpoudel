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
#include <linux/string.h>
#include <linux/slab.h>

#include "aesd-circular-buffer.h"
#include "aesdchar.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Biplav Poudel"); 
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *device;  
    PDEBUG("aesd_open() is invoked");                                      /* for device information */
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
    struct aesd_dev *dev = filp->private_data;      /* this aesdchar device will be locked for read operation*/
    struct aesd_buffer_entry *return_entry;     /* not same as the entry used in aesdchar device state*/
    size_t entry_offset;        /* stores the offset from the beginning of the returned entry */
    size_t bytes_to_copy;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    /* we need to ensure no access to device struct is made without holding mutex*/
    if (mutex_lock_interruptible(&dev->lock)) 
        return -ERESTARTSYS;                        /* if "locking wait" was interrupted, we need to signal kernel to restart*/

    /* from f_pos, we only send count size of writes back */
    return_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circ_buf, *f_pos, &entry_offset);

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

    bytes_to_copy = min(return_entry->size - entry_offset, count);   // shouldn't exceed count size

    /** 
    * copy_to_user() returns number of bytes not copied.
    * 0 means all bytes copied.
    * non-zero means bytes remaining.
    * NOTE: if all bytes copied, returns 0 else size of uncopied bytes! So the condition doesn't apply for success.
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
    size_t old_entry_size_copy;
    size_t index;
    size_t writesize;
    bool newline_found = false;
    char *tmp;
    char *temp_buffer;
    const char *ret_ptr = NULL;

    struct aesd_dev *dev = filp->private_data;      /* this aesdchar device will be locked for write operation*/
    struct aesd_buffer_entry *entry = dev->partial_entry;  /* stores previous partial writes in dev*/

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    /* we need to ensure no access to device struct is made without holding mutex*/
    if (mutex_lock_interruptible(&dev->lock)) 
        return -ERESTARTSYS;                        /* if "locking wait" was interrupted, we need to signal kernel to restart*/

    old_entry_size_copy = entry->size;   // for backup as an offset to start write from

    /* We create/allocate a new char buffer with original data preserved. */
    PDEBUG("Reallocating the buffer inside entry with an increased size by count bytes.");
    tmp = krealloc((const void *) entry->buffptr, entry->size + count, GFP_KERNEL);
    if (!tmp){
        PDEBUG("Failed to increase the size of buffer by count");
        goto out;
    }
    entry->buffptr = tmp;

    /** Now we write to the buffer from userspace.
        If partial copy is returned, it is not becuase the userspace buffer is exhausted.
        It may be due to addressing error or page fault.
        copy_from_user() will copy garbage values until count size, if needed.
        NOTE: if all bytes copied, returns 0 else size of uncopied bytes! So the condition doesn't apply for success.
    */
    if (copy_from_user((void *) entry->buffptr + old_entry_size_copy, buf, count)){
        PDEBUG("Only partial copy from userspace occurred! Returning error.");
        entry->size = old_entry_size_copy;
        retval = -EFAULT;
        goto out;
    }
    entry->size += count;
    retval = count;

    /* Now we check the freshly written buffer for null termination and append it to the circular buffer*/

    for (index = 0; index < entry->size; index++){
        if (*(entry->buffptr + index) == '\n'){
            newline_found = true;
            break;
        }
    }

    if (newline_found){
        writesize = index + 1;
        /* (index+1) because index is not incremented and breaks off the for...loop when '\n' is found! */

        temp_buffer = kzalloc(writesize, GFP_KERNEL);
        if (!temp_buffer){
            PDEBUG("Error allocating memory for temporary buffer!");
            goto out;
        }
        struct aesd_buffer_entry new_entry;
        new_entry.buffptr = temp_buffer;
        new_entry.size = writesize;

        // we copy the writes from index 0 until \n and assign it to temp_buffer pointer
        memcpy( (void *) temp_buffer, entry->buffptr, writesize);
        ret_ptr = aesd_circular_buffer_add_entry(&dev->circ_buf, &new_entry);
        if (ret_ptr != NULL) {
            PDEBUG("Freeing overwritten buffer entry\n");
            kfree(ret_ptr);
        }
        
        // after successful write, we check if entry size is 0 or not
        entry->size -= writesize;

        if (entry->size > 0) {
            memmove((void *) entry->buffptr, entry->buffptr + writesize, entry->size);
        } else {
            kfree(entry->buffptr);              // if entry is empty, we free the pointer and reset the buffer
            entry->buffptr = NULL;
        }
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
    struct aesd_buffer_entry *entry;

    // we use dynamic allocation to get major and minor numbers
    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");      // dev is a output-only param
    aesd_major = MAJOR(dev);
    
    if (result < 0) {       // On success, returns 0. else, negative error code
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * we initialize the AESD specific portion of the device
     */
    PDEBUG("Address of device is: &aesd_device: %p\n", (void *) &aesd_device);
    PDEBUG("Address of circular buffer is: &aesd_device->circ_buf: %p\n", (void *) &aesd_device.circ_buf );
    PDEBUG("Address of interruptible mutex lock is: &aesd_device->lock: %p\n", (void *) &aesd_device.lock );
    PDEBUG("Address of entry struct is: &aesd_device->partial_entry: %p\n", (void *) &aesd_device.partial_entry );
    PDEBUG("Address of heap buffer in buffptr of entry struct is: aesd_device->partial_entry: %p\n", aesd_device.partial_entry );

    aesd_circular_buffer_init(&aesd_device.circ_buf);   // initializing the circular buffer
    mutex_init(&aesd_device.lock);                              // initializing the mutex

    // dynamically allocate the entry struct and reset fields (to 0, false, or NULL based on data types)
    entry = kzalloc(sizeof(struct aesd_buffer_entry), GFP_KERNEL);
    if (!entry){
        PDEBUG("Couldn't allocate memory for entry struct!");
        result = -ENOMEM;
        goto init_failure;
    }
    aesd_device.partial_entry = entry;
    

    result = aesd_setup_cdev(&aesd_device); 

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

    init_failure:
        unregister_chrdev_region(dev, 1);
        return result;

}

void aesd_cleanup_module(void)
{
    uint8_t index;                          // defaults to 0
    struct aesd_buffer_entry *entryptr;     // holds the first entry pointer as entryptr=&((buffer)->entry[index])
    
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * Cleaning up the AESD specific portions here as necessary (circular buffer, entry struct, and aesddevice)
     */

    /* Cleaning the circular buffer using the predefined macro*/

    AESD_CIRCULAR_BUFFER_FOREACH(entryptr, &aesd_device.circ_buf, index){
        if (entryptr->buffptr != NULL){
            PDEBUG("Freeing aesd_buffer_entry pointer: %p\n", entryptr->buffptr);
            kfree((void *) entryptr->buffptr);
        }
    }

    /* clearing the partial buffer entry struct field*/
    PDEBUG("Freeing aesd_device->partial_entry...");
    if (aesd_device.partial_entry) {
        kfree(aesd_device.partial_entry->buffptr);
        kfree(aesd_device.partial_entry);
        aesd_device.partial_entry = NULL;
    }

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
