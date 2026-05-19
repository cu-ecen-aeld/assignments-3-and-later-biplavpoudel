/*
 * aesdchar.h
 *
 *  Created on: Oct 23, 2019
 *      Author: Dan Walkes
 */

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#include "aesd-circular-buffer.h"
#define AESD_DEBUG 1  //Remove comment on this line to enable debug

#undef PDEBUG             /* undef it, just in case */
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_DEBUG "aesdchar: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

/* Adding structure(s) and locks needed to complete assignment requirements*/
struct aesd_dev
{    
     struct aesd_circular_buffer buffer;     /* Circular buffer structure */
     struct aesd_buffer_entry entry[AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED];        /* For storing partial writes before "\n" */
     struct mutex lock;            /* locking primitive */
     struct cdev cdev;             /* Char device structure */
};


#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
