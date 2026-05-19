/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes, Biplav Poudel
 * @date 2026-04-24
 * @copyright Copyright (c) 2026
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    uint8_t cur_entry = buffer->out_offs;   // current entry starts from out_offset!
    size_t remaining_char_offset = char_offset;

    // Returns void if the pointers point to nowhere
    if ((buffer == NULL) || (entry_offset_byte_rtn == NULL)) return NULL;

    // Returns NULL if buffer pointer is also empty
    if (!buffer->full && (buffer->out_offs == buffer->in_offs)) return NULL;
    // printf("\n\nENTERING  CODE LOGIC>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
    // printf("The given char_offset is: %ld\n", char_offset);

    // it is not guarenteed that the read/write buffer starts together from 0th position, so we find valid entries thorugh relative positions.
    // i.e. inoffs (writeops) = 2, outoffs (readops) = 3, MAX = 4. then valid entries starts from 3 (last)->0(first)->1(second), so only -1 mod 4 = 3.
    size_t total_valid_entries = buffer->full ? AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED : (buffer->in_offs - buffer->out_offs) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    size_t read_entries = 0;

    // we place the condition so the loop only goes over the valid entries starting from "out_offs" and not to infinity!
    while (read_entries < total_valid_entries && buffer->entry[cur_entry].buffptr != NULL)
    {
        // if remaining_char_offset is larger than or equal to the new entry, we subtract the remaining_char_offset and goto new entry
        if (buffer->entry[cur_entry].size <= remaining_char_offset)
        {
            // printf("The size of the current entry %s is: %ld\n",buffer->entry[cur_entry].buffptr, buffer->entry[cur_entry].size);
            remaining_char_offset -= buffer->entry[cur_entry].size;
            // printf("The updated remaining char_offset is %ld\n", remaining_char_offset);

            cur_entry = (cur_entry + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
            // printf("The new value of new current entry is %s\n", buffer->entry[cur_entry].buffptr);
            read_entries ++;
            // printf("The no. of read_entries out of %ld are: %ld\n\n", total_valid_entries, read_entries);
            continue;
        }

        // if the remaining_offset is smaller than the current entry, it means the char_offset points to the "current entry" at remaining_char_offset position!
        // printf("------------\n");
        // printf("The total read_entries out of %ld are: %ld\n", total_valid_entries, ++read_entries);
        // printf("The final remaining char_offset is %ld\n", remaining_char_offset);
        // printf("The final entry is %s\n\n`", buffer->entry[cur_entry].buffptr);
        *entry_offset_byte_rtn = remaining_char_offset;
        return &buffer->entry[cur_entry];
    }
    return NULL;
    }

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    // Return void if the pointers point to null items
    if ((buffer == NULL) || (add_entry == NULL)) return; 

    // adding one entry (add_entry) to in_offset position inside buffer's entry
    // we don't care if buffer was empty or not during insertion
    buffer->entry[buffer->in_offs].buffptr = add_entry->buffptr;
    buffer->entry[buffer->in_offs].size = add_entry->size;

    //FOR TESTING
    // printf("The added entry is:%s", buffer->entry[buffer->in_offs].buffptr);
    // for (uint8_t i=0; i < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; i++) printf("%s", buffer->entry[i].buffptr);
    // printf("\n\n");

    // increase in_offset by 1 after each insertion
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    // if buffer is full, we also shift the out_offset by 1; value becomes equal to in_offset after full
    if (buffer->full) buffer->out_offs = buffer->in_offs;
    
    // by this point, insertion has happened at least once
    // so if buffer is not set to empty and the in_offs and out_offs are equal, the buffer is full
    if (!buffer->full && buffer->in_offs == buffer->out_offs) buffer->full = true;
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
