#include <linux/version.h>
#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>  /* printk() */
#include <linux/errno.h>   /* error codes */
#include <linux/types.h>   /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/wait.h>
#include <linux/file.h>

#include "spinlock.h"
#include "osprd.h"

/* The size of an OSPRD sector. */
#define SECTOR_SIZE	512

/* This flag is added to an OSPRD file's f_flags to indicate that the file
 * is locked. */
#define F_OSPRD_LOCKED	0x80000

/* eprintk() prints messages to the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("CS 111 RAM Disk");
MODULE_AUTHOR("Shalini and Katie");

#define OSPRD_MAJOR	222

/* This module parameter controls how big the disk will be.
 * You can specify module parameters when you load the module,
 * as an argument to insmod: "insmod osprd.ko nsectors=4096" */
static int nsectors = 32;
module_param(nsectors, int, 0);


/* Linked list implementation */
typedef struct mList
{
	struct list_head list;
	pid_t pid;
	int isFirstTicket;
} mlist_t;

typedef struct ticketList
{
	struct list_head list;
	int ticketNum;
} ticketList_t;

/* Request list implementation */
typedef struct reqList
{
	struct list_head list;
	pid_t pid;
	unsigned is_modified;
	sector_t sector_num;
	unsigned num_sectors;
} reqList_t;

/************************************************************/
/*==================== PID READER LIST =====================*/
/************************************************************/
/* Add the pid to the end of the list */
/* Returns 0 if the add failed, 1 if it succeeded */
int addToList(pid_t pid, mlist_t* l)
{
	mlist_t *tmp;
	tmp = (mlist_t *)kmalloc(sizeof(mlist_t), __GFP_NORETRY);

	if(tmp == NULL)
		return 0;

	tmp->pid = pid;
	list_add_tail(&(tmp->list), &l->list);
	return 1;
}

/* Check whether the pid already exists in the reader list */
int isPidInList(pid_t pid, mlist_t* l)
{
	struct list_head *pos, *q;
	mlist_t *tmp;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, mlist_t, list);

		if(tmp->pid == pid)
		{
			return 1;
		}
	}

	return 0;
}

/* Remove all items with the given pid from the list */
int removeFromList(pid_t pid, mlist_t* l)
{
	struct list_head *pos, *q;
	mlist_t *tmp;
	int count = 0;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, mlist_t, list);

		if(tmp->pid == pid)
		{
			list_del(pos);
			kfree(tmp);
			count++;
		}
	}
	return count;
}

/* Remove one pid from the list and return the number of
readers found with that pid */
int removeOneFromList(pid_t pid, mlist_t* l)
{
	struct list_head *pos, *q;
	mlist_t *tmp;
	int found = 0;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, mlist_t, list);

		if(tmp->pid == pid)
		{
			if (!found)
			{
				list_del(pos);
				kfree(tmp);
			}
			found++;
		}
	}
	return found;
}

/* Delete the list and deallocate */
void deleteList(mlist_t* l)
{
	struct list_head *pos, *q;
	mlist_t *tmp;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, mlist_t, list);
		list_del(pos);
		kfree(tmp);
	}
}


/************************************************************/
/*==================== TICKET LIST =========================*/
/************************************************************/
/* Check whether the ticket already exists in the ticket list */
int isTicketInList(int ticket, ticketList_t* l)
{
	struct list_head *pos, *q;
	ticketList_t *tmp;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, ticketList_t, list);

		if(tmp->ticketNum == ticket)
		{
			return 1;
		}
	}

	return 0;
}

int addToTicketList(int ticket, ticketList_t *l, ticketList_t **first)
{
	ticketList_t *tmp;
	tmp = (ticketList_t *)kmalloc(sizeof(ticketList_t), __GFP_NORETRY);

	if(tmp == NULL)
		return 0;

	tmp->ticketNum = ticket;
	list_add_tail(&(tmp->list), &(l->list));
	
	if(*first == NULL)
	{
		*first = tmp;
	}

	return 1;
}


ticketList_t *removeFromTicketList(int ticket, ticketList_t *l, ticketList_t **first)
{
	ticketList_t* tmp;
	struct list_head *pos, *q;
	int isFirstTicket = 0;

	struct list_head *next;
 	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, ticketList_t, list);

		if(tmp->ticketNum == ticket)
		{
			if(tmp == *first)
			{
				isFirstTicket = 1;
			}

			next = pos->next;
			list_del(pos);
			kfree(tmp);

			if(list_empty(&l->list))
			{
				*first = NULL;
				return tmp;
			}
			break;
		}
	}
	//reset first_ticket if we just deleted first_ticket
	if(isFirstTicket)
	{			
		//find out which node contains the list_head next
		struct list_head *pos, *q;
		ticketList_t *nextNode;

		list_for_each_safe(pos, q, &l->list)
		{
			nextNode = list_entry(pos, ticketList_t, list);
			if(&nextNode->list == next)
			{
				*first = nextNode;
				break;
			} 
		}
	}
	return tmp;	
}



/************************************************************/
/*=============== NOTIFICATION REQUEST LIST ================*/
/************************************************************/

/* Add a request to the list of requests for this ramdisk */
int addToRequestList(pid_t pid, reqList_t* l, sector_t sector, unsigned num)
{
	// If the sector number and range are not specified, assume the request
	// refers to the entire ramdisk
	if (sector == -1)
		sector = 0;
	if (num == -1)
		num = nsectors;

	reqList_t *tmp;
	tmp = (reqList_t *)kmalloc(sizeof(reqList_t), __GFP_NORETRY);

	if(tmp == NULL)
		return 0;

	tmp->pid = pid;
	tmp->sector_num = sector;
	tmp->num_sectors = num;
	tmp->is_modified = 0;

	list_add_tail(&(tmp->list), &l->list);
	return 1;
}

/* Remove all items with the given pid from the request list */
int removeFromRequestList(pid_t pid, reqList_t* l)
{
	struct list_head *pos, *q;
	reqList_t *tmp;
	int count = 0;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, reqList_t, list);

		if(tmp->pid == pid)
		{
			list_del(pos);
			kfree(tmp);
			count++;
		}
	}
	return count;
}

/* Check whether the pid already exists in the request list */
int isRequestInList(pid_t pid, reqList_t* l)
{
	struct list_head *pos, *q;
	reqList_t *tmp;

	int count = 0;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, reqList_t, list);

		if(tmp->pid == pid)
		{
			count++;
		}
	}

	return count;
}

// Notify all processes following this ramdisk by updating their is_modified variable
void notify_followers(reqList_t* l, sector_t sector, size_t num_bytes)
{
	struct list_head *pos, *q;
	reqList_t *tmp;
	unsigned num_sectors = num_bytes/SECTOR_SIZE;

	// avoid truncation
	if (num_sectors * SECTOR_SIZE < num_bytes)
	{
		num_sectors++;
	}

	unsigned write_end = sector + num_sectors;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, reqList_t, list);
		// tmp->is_modified = 1;
		unsigned request_end = tmp->sector_num + tmp->num_sectors;

		// If the actual write overlaps with the requested sectors
		// notify the process that the sector region they were following
		// has been modified by a write
		if ((tmp->sector_num <= sector && request_end >= sector) 
			|| (tmp->sector_num <= write_end && request_end >= write_end))
		{
			tmp->is_modified = 1;
		}
	}
}

//Determines whether the request has been satisfied and can stop blocking
//Returns true if:
	//no requests have this PID (can immediately stop blocking)
	//all requests with the PID are satisified
int ramdiskModified(pid_t pid, reqList_t* l)
{
	struct list_head *pos, *q;
	reqList_t *tmp;

	int count = 0;
	int all_modified = 1;

	list_for_each_safe(pos, q, &l->list)
	{
		tmp = list_entry(pos, reqList_t, list);

		if(tmp->pid == pid)
		{
			all_modified &= tmp->is_modified;
			count++;
		}
	}

	if(!count)
		return 1;
	return all_modified;
}

/************************************************************/
/*==================== OSPRD INFO ==========================*/
/************************************************************/
/* The internal representation of our device. */
typedef struct osprd_info {
	uint8_t *data;                          // The data array. Its size is
	                                        // (nsectors * SECTOR_SIZE) bytes.

	osp_spinlock_t mutex;                   // Mutex for synchronizing access to
	                                        // this block device

	/********************TICKET LIST******************************************/
	unsigned ticket_head;					// Currently running ticket for the device lock

	unsigned ticket_tail;					// Next available ticket for the device lock

	ticketList_t tickets; 					// linked list of tickets
	ticketList_t *first_ticket; 			// points to first-served ticket in queue

	wait_queue_head_t blockq;       		// Wait queue for tasks blocked on the device lock
	wait_queue_head_t notifq;       		// Wait queue for tasks blocked on memory notification requests

	/**************************NOTIFICATION LIST******************************/
	reqList_t notify_pids;					// linked list of processes that want to be notified
	                                        // when there is a write to this ramdisk

	/**************************DEADLOCK***************************************/
	/* HINT: You may want to add additional fields to help
	         in detecting deadlock. */

	int write_locked;				        // Should only ever be 0 or 1.
	                                        // Indicates whether the file is write locked

	int write_lock_pid;			            // PID of the process that holds the write lock

	int num_read_locks;			            // Indicates the number of read locks on the file

	mlist_t read_lock_pids;			        // Linked list of read lock pids
	
	// The following elements are used internally; you don't need
	// to understand them.
	struct request_queue *queue;    	   // The device request queue.
	spinlock_t qlock;			           // Used internally for mutual exclusion in the 'queue'.
	struct gendisk *gd;             	   // The generic disk.
} osprd_info_t;


#define NOSPRD 4
static osprd_info_t osprds[NOSPRD];
// Declare useful helper functions


/*
 * file2osprd(filp)
 *   Given an open file, check whether that file corresponds to an OSP ramdisk.
 *   If so, return a pointer to the ramdisk's osprd_info_t.
 *   If not, return NULL.
 */
static osprd_info_t *file2osprd(struct file *filp);

/*
 * for_each_open_file(task, callback, user_data)
 *   Given a task, call the function 'callback' once for each of 'task's open
 *   files.  'callback' is called as 'callback(filp, user_data)'; 'filp' is
 *   the open file, and 'user_data' is copied from for_each_open_file's third
 *   argument.
 */
static void for_each_open_file(struct task_struct *task,
			       void (*callback)(struct file *filp,
						osprd_info_t *user_data),
			       osprd_info_t *user_data);


/*
 * osprd_process_request(d, req)
 *   Called when the user reads or writes a sector.
 *   Should perform the read or write, as appropriate.
 */

static void osprd_process_request(osprd_info_t *d, struct request *req)
{
	if (!blk_fs_request(req)) {
		end_request(req, 0);
		return;
	}

	// EXERCISE: Perform the read or write request by copying data between
	// our data array and the request's buffer.
	// Hint: The 'struct request' argument tells you what kind of request
	// this is, and which sectors are being read or written.
	// Read about 'struct request' in <linux/blkdev.h>.
	// Consider the 'req->sector', 'req->current_nr_sectors', and
	// 'req->buffer' members, and the rq_data_dir() function.

	// Your code here.

	unsigned long offset = req->sector * SECTOR_SIZE;
	size_t num_bytes = req->current_nr_sectors * SECTOR_SIZE;
	uint8_t *data_ptr = d->data + offset;

	// Determine whether the request as a READ or WRITE request
	unsigned int requestType = rq_data_dir(req);

	if ((num_bytes + offset) > (nsectors * SECTOR_SIZE))
	{
		// Too much data
		end_request(req, 0);
	}

	if (requestType == READ)
	{
		// Copy data from our data array to the request's buffer
 		memcpy((void*)req->buffer, (void*)data_ptr, num_bytes);
	}
 	else if (requestType == WRITE)
 	{
 		// Copy data from the request's buffer to our data array
 		memcpy((void*)data_ptr, (void*)req->buffer, num_bytes);
 		notify_followers(&d->notify_pids, req->sector, num_bytes);
 		wake_up_all(&d->notifq);

 	}
 	else
 	{
 		// Not a read or write request. We are done with this request.
 		end_request(req, 0);
 	}

	end_request(req, 1);
}


// This function is called when a /dev/osprdX file is opened.
// You aren't likely to need to change this.
static int osprd_open(struct inode *inode, struct file *filp)
{
	// Always set the O_SYNC flag. That way, we will get writes immediately
	// instead of waiting for them to get through write-back caches.
	filp->f_flags |= O_SYNC;
	return 0;
}


// This function is called when a /dev/osprdX file is finally closed.
// (If the file descriptor was dup2ed, this function is called only when the
// last copy is closed.)
static int osprd_close_last(struct inode *inode, struct file *filp)
{
	if (filp) {
		osprd_info_t *d = file2osprd(filp);
		// Indicates whether the file was opened for reading or writing
		int filp_writable = filp->f_mode & FMODE_WRITE;

		// EXERCISE: If the user closes a ramdisk file that holds
		// a lock, release the lock.  Also wake up blocked processes
		// as appropriate.

		// Your code here.

		// Check if the file is locked
		if (filp->f_flags & F_OSPRD_LOCKED)
		{
			osp_spin_lock(&d->mutex);

			if (filp_writable) // opened for writing
			{
				if (current->pid == d->write_lock_pid)
				{
					// Remove the write lock
					d->write_locked = 0;
					// Reset the pid to -1 (no process has the write lock)
					d->write_lock_pid = -1;

					// Clear the file of the locked flag
					filp->f_flags ^= F_OSPRD_LOCKED;
				}
			}
			else // opened for reading
			{
				// Delete all of the current process' locks
				int numLocks = removeFromList(current->pid, &d->read_lock_pids);
				d->num_read_locks -= numLocks;

				// If we were able to remove a lock, clear the file of the locked flag
				if (numLocks > 0)
				{
					filp->f_flags ^= F_OSPRD_LOCKED;
				}	
			}

			osp_spin_unlock(&d->mutex);
			// wake up all blocked processes
			wake_up_all(&d->blockq);
		}
		else
		{
			// the file was not locked
			return 0;
		}

		// This line avoids compiler warnings; you may remove it.
		(void) filp_writable, (void) d;

	}

	return 0;
}


/*
 * osprd_lock
 */

/*
 * osprd_ioctl(inode, filp, cmd, arg)
 *   Called to perform an ioctl on the named file.
 */
int osprd_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, reqParams_t *reqParams)
{
	osprd_info_t *d = file2osprd(filp);	// device info
	int r = 0;			// return value: initially 0

	// is file open for writing?
	int filp_writable = (filp->f_mode & FMODE_WRITE) != 0;

	// This line avoids compiler warnings; you may remove it.
	(void) filp_writable, (void) d;

	unsigned int local_ticket = 0;

	if (d == NULL)
		return -1;

	// Set 'r' to the ioctl's return value: 0 on success, negative on error

	if (cmd == OSPRDIOCACQUIRE) {

		// EXERCISE: Lock the ramdisk.
		//
		// If *filp is open for writing (filp_writable), then attempt
		// to write-lock the ramdisk; otherwise attempt to read-lock
		// the ramdisk.
		//
        // This lock request must block using 'd->blockq' until:
		// 1) no other process holds a write lock;
		// 2) either the request is for a read lock, or no other process
		//    holds a read lock; and
		// 3) lock requests should be serviced in order, so no process
		//    that blocked earlier is still blocked waiting for the
		//    lock.
		//
		// If a process acquires a lock, mark this fact by setting
		// 'filp->f_flags |= F_OSPRD_LOCKED'.  You also need to
		// keep track of how many read and write locks are held:
		// change the 'osprd_info_t' structure to do this.
		//
		// Also wake up processes waiting on 'd->blockq' as needed.
		//
		// If the lock request would cause a deadlock, return -EDEADLK.
		// If the lock request blocks and is awoken by a signal, then
		// return -ERESTARTSYS.
		// Otherwise, if we can grant the lock request, return 0.

		// 'd->ticket_head' and 'd->ticket_tail' should help you
		// service lock requests in order.  These implement a ticket
		// order: 'ticket_tail' is the next ticket, and 'ticket_head'
		// is the ticket currently being served.  You should set a local
		// variable to 'd->ticket_head' and increment 'd->ticket_head'.
		// Then, block at least until 'd->ticket_tail == local_ticket'.
		// (Some of these operations are in a critical section and must
		// be protected by a spinlock; which ones?)

		// Your code here (instead of the next two lines).
		// r = -ENOTTY;

		/* DEADLOCK CASES */
		// If the current process already has the write lock, it cannot
		// request another lock
		osp_spin_lock(&d->mutex);
		if (current->pid == d->write_lock_pid)
		{
			osp_spin_unlock(&d->mutex);
			return -EDEADLK;
		}

		//add pid to end of ticket list
		int success = addToTicketList(d->ticket_tail, &d->tickets, &d->first_ticket);

		//if adding pid unsuccessful, return error
		if(!success)
		{
			return -ENOMEM;
		}

		local_ticket = d->ticket_tail;
		d->ticket_tail++;
		osp_spin_unlock(&d->mutex);

		if (filp_writable) // opened for writing
		{
			// DEADLOCK: If current process has read lock, it can't request write lock
			osp_spin_lock(&d->mutex);
			if (isPidInList(current->pid, &d->read_lock_pids))
			{
				// Remove ticket from list to avoid deadlock
				removeFromTicketList(local_ticket, &d->tickets, &d->first_ticket);
				osp_spin_unlock(&d->mutex);
				return -EDEADLK;
			}
			osp_spin_unlock(&d->mutex);

			// Wait until local ticket can be serviced
			int wait_signal = wait_event_interruptible(d->blockq, d->write_locked == 0 
														&& d->num_read_locks == 0 
														&& local_ticket == d->first_ticket->ticketNum);
			// If the lock request blocks and is awoken by a signal, then
			// return -ERESTARTSYS.
			if (wait_signal == -ERESTARTSYS)
			{
				osp_spin_lock(&d->mutex);
				removeFromTicketList(local_ticket, &d->tickets, &d->first_ticket);
				osp_spin_unlock(&d->mutex);
				return -ERESTARTSYS;
			}

			osp_spin_lock(&d->mutex);
			// The file is now write locked by the current process
			d->write_locked = 1;
			d->write_lock_pid = current->pid;

			// Ticket was serviced so remove it from list 
			removeFromTicketList(local_ticket, &d->tickets, &d->first_ticket);

			// Mark file as locked
			filp->f_flags |= F_OSPRD_LOCKED;
			osp_spin_unlock(&d->mutex);

			r = 0;
		}
		else // opened for reading
		{
			// Wait until local ticket can be serviced
			int wait_signal = wait_event_interruptible(d->blockq, d->write_locked == 0 
														&& local_ticket == d->first_ticket->ticketNum);
			// If the lock request blocks and is awoken by a signal, then
			// return -ERESTARTSYS.
			if (wait_signal == -ERESTARTSYS)
			{
				osp_spin_lock(&d->mutex);
				removeFromTicketList(local_ticket, &d->tickets, &d->first_ticket);
				osp_spin_unlock(&d->mutex);
				return -ERESTARTSYS;
			}
			osp_spin_lock(&d->mutex);
			
			// Add new read lock to this file's list
			int success = addToList(current->pid, &d->read_lock_pids);
			// If we are not able to allocate memory for this reader, return
			if(!success)
			{
				return -ENOMEM;
			}
			d->num_read_locks++;
			
			// Ticket was serviced so remove it from list
			removeFromTicketList(local_ticket, &d->tickets, &d->first_ticket);

			// Mark file as locked
			filp->f_flags |= F_OSPRD_LOCKED;
			osp_spin_unlock(&d->mutex);

			r = 0;
		}

		// If the ticket list is not empty, wake up blocked processes
		if (!list_empty(&d->tickets.list))
		{
			wake_up_all(&d->blockq);
		}

	} else if (cmd == OSPRDIOCTRYACQUIRE) {

		// EXERCISE: ATTEMPT to lock the ramdisk.
		//
		// This is just like OSPRDIOCACQUIRE, except it should never
		// block.  If OSPRDIOCACQUIRE would block or return deadlock,
		// OSPRDIOCTRYACQUIRE should return -EBUSY.
		// Otherwise, if we can grant the lock request, return 0.

		// Your code here (instead of the next two lines).
		// r = -ENOTTY;

		/* DEADLOCK CASES */
		// If the current process already has the write lock, it cannot
		// request another lock
		osp_spin_lock(&d->mutex);
		if (d->first_ticket != NULL)
		{
			osp_spin_unlock(&d->mutex);
			return -EBUSY;
		}
		osp_spin_unlock(&d->mutex);


		if (filp_writable) // opened for writing
		{
			osp_spin_lock(&d->mutex);

			// Grant write lock if no other locks exist on file
			if (d->write_locked == 0 && d->num_read_locks == 0)
			{
				// The file is now write locked by the current process
				d->write_locked = 1;
				d->write_lock_pid = current->pid;
				osp_spin_unlock(&d->mutex);

				r = 0;
			}
			else
			{
				osp_spin_unlock(&d->mutex);	
				// If not possible to lock, return BUSY (instead of blocking)
				return -EBUSY;
			}
		}
		else  // opened for reading
		{
			osp_spin_lock(&d->mutex);
			if (d->write_locked == 0)
			{
				// Add new read lock to file's list
				int success = addToList(current->pid, &d->read_lock_pids);

				// If we are not able to allocate memory for the reader, return.
				if(!success)
				{
					return -ENOMEM;
				}
				d->num_read_locks++;
				
				osp_spin_unlock(&d->mutex);

				r = 0;
			}
			else
			{
				osp_spin_unlock(&d->mutex);
				// If not possible to lock, return BUSY (instead of blocking)
				return -EBUSY;
			}
		}

		osp_spin_lock(&d->mutex);
		filp->f_flags |= F_OSPRD_LOCKED;
		d->ticket_tail++;
		osp_spin_unlock(&d->mutex);

	} else if (cmd == OSPRDIOCRELEASE) {

		// EXERCISE: Unlock the ramdisk.
		//
		// If the file hasn't locked the ramdisk, return -EINVAL.
		// Otherwise, clear the lock from filp->f_flags, wake up
		// the wait queue, perform any additional accounting steps
		// you need, and return 0.

		// Your code here (instead of the next line).
		//r = -ENOTTY;

		// If the file hasn't locked the ramdisk, return -EINVAL.
		if (!(filp->f_flags & F_OSPRD_LOCKED))
		{
			return -EINVAL;
		}
		else
		{
			osp_spin_lock(&d->mutex);

			if (filp_writable)	// opened for writing
			{
				// Remove write lock if in correct process
				if (current->pid == d->write_lock_pid)
				{
					d->write_locked = 0;
					d->write_lock_pid = -1;

					// Clear the lock from filp->f_flags
					filp->f_flags ^= F_OSPRD_LOCKED;
				}
				else
				{
					osp_spin_unlock(&d->mutex);
					return -EINVAL;
				}
			}
			else	// opened for reading
			{
				// Delete this pid from read lock list
				int count = removeOneFromList(current->pid, &d->read_lock_pids);

				if (count == 1)
				{
					// Clear the lock from filp->f_flags
					filp->f_flags ^= F_OSPRD_LOCKED;
				}
				else if (count == 0)
				{
					osp_spin_unlock(&d->mutex);

					// Return error if it wasn't possible to remove the read lock
					return -EINVAL;
				}

				d->num_read_locks--;

			}

			osp_spin_unlock(&d->mutex);
			// Wake up the wait queue
			wake_up_all(&d->blockq);

			r = 0;
		}

	} else if (cmd == OSPRDIONOTIFY) {

		int success = addToRequestList(current->pid, &d->notify_pids, reqParams->sector, reqParams->nSectors);
		if (!success)
		{
			return -ENOMEM;
		}

		int wait_signal = wait_event_interruptible(d->notifq, ramdiskModified(current->pid, &d->notify_pids));

		osp_spin_lock(&d->mutex);
		removeFromRequestList(current->pid, &d->notify_pids);
		osp_spin_unlock(&d->mutex);

		if (wait_signal == -ERESTARTSYS)
		{
			return -ERESTARTSYS;
		}

		r = 0;
	} 
	else
	{
		r = -ENOTTY; /* unknown command */
	}
	return r;
}


// Initialize internal fields for an osprd_info_t.

static void osprd_setup(osprd_info_t *d)
{
	/* Initialize the wait queue. */
	init_waitqueue_head(&d->blockq);
	init_waitqueue_head(&d->notifq);
	osp_spin_lock_init(&d->mutex);
	d->ticket_head = d->ticket_tail = 0;
	d->first_ticket = NULL;

	/* Initialize reader list */
	INIT_LIST_HEAD(&(d->read_lock_pids.list));
	// Initialize ticket list
	INIT_LIST_HEAD(&(d->tickets.list));
	// Initialize notification list
	INIT_LIST_HEAD(&(d->notify_pids.list));

	/* Add code here if you add fields to osprd_info_t. */
	d->num_read_locks = 0;
	d->write_lock_pid = -1;
	d->write_locked = 0;
}


/*****************************************************************************/
/*         THERE IS NO NEED TO UNDERSTAND ANY CODE BELOW THIS LINE!          */
/*                                                                           */
/*****************************************************************************/

// Process a list of requests for a osprd_info_t.
// Calls osprd_process_request for each element of the queue.

static void osprd_process_request_queue(request_queue_t *q)
{
	osprd_info_t *d = (osprd_info_t *) q->queuedata;
	struct request *req;

	while ((req = elv_next_request(q)) != NULL)
		osprd_process_request(d, req);
}


// Some particularly horrible stuff to get around some Linux issues:
// the Linux block device interface doesn't let a block device find out
// which file has been closed.  We need this information.

static struct file_operations osprd_blk_fops;
static int (*blkdev_release)(struct inode *, struct file *);

static int _osprd_release(struct inode *inode, struct file *filp)
{
	if (file2osprd(filp))
		osprd_close_last(inode, filp);
	return (*blkdev_release)(inode, filp);
}

static int _osprd_open(struct inode *inode, struct file *filp)
{
	if (!osprd_blk_fops.open) {
		memcpy(&osprd_blk_fops, filp->f_op, sizeof(osprd_blk_fops));
		blkdev_release = osprd_blk_fops.release;
		osprd_blk_fops.release = _osprd_release;
	}
	filp->f_op = &osprd_blk_fops;
	return osprd_open(inode, filp);
}


// The device operations structure.

static struct block_device_operations osprd_ops = {
	.owner = THIS_MODULE,
	.open = _osprd_open,
	// .release = osprd_release, // we must call our own release
	.ioctl = osprd_ioctl
};


// Given an open file, check whether that file corresponds to an OSP ramdisk.
// If so, return a pointer to the ramdisk's osprd_info_t.
// If not, return NULL.

static osprd_info_t *file2osprd(struct file *filp)
{
	if (filp) {
		struct inode *ino = filp->f_dentry->d_inode;
		if (ino->i_bdev
		    && ino->i_bdev->bd_disk
		    && ino->i_bdev->bd_disk->major == OSPRD_MAJOR
		    && ino->i_bdev->bd_disk->fops == &osprd_ops)
			return (osprd_info_t *) ino->i_bdev->bd_disk->private_data;
	}
	return NULL;
}


// Call the function 'callback' with data 'user_data' for each of 'task's
// open files.

static void for_each_open_file(struct task_struct *task,
		  void (*callback)(struct file *filp, osprd_info_t *user_data),
		  osprd_info_t *user_data)
{
	int fd;
	task_lock(task);
	spin_lock(&task->files->file_lock);
	{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 13)
		struct files_struct *f = task->files;
#else
		struct fdtable *f = task->files->fdt;
#endif
		for (fd = 0; fd < f->max_fds; fd++)
			if (f->fd[fd])
				(*callback)(f->fd[fd], user_data);
	}
	spin_unlock(&task->files->file_lock);
	task_unlock(task);
}


// Destroy a osprd_info_t.

static void cleanup_device(osprd_info_t *d)
{
	wake_up_all(&d->blockq);
	if (d->gd) {
		del_gendisk(d->gd);
		put_disk(d->gd);
	}
	if (d->queue)
		blk_cleanup_queue(d->queue);
	if (d->data)
		vfree(d->data);
}


// Initialize a osprd_info_t.

static int setup_device(osprd_info_t *d, int which)
{
	memset(d, 0, sizeof(osprd_info_t));

	/* Get memory to store the actual block data. */
	if (!(d->data = vmalloc(nsectors * SECTOR_SIZE)))
		return -1;
	memset(d->data, 0, nsectors * SECTOR_SIZE);

	/* Set up the I/O queue. */
	spin_lock_init(&d->qlock);
	if (!(d->queue = blk_init_queue(osprd_process_request_queue, &d->qlock)))
		return -1;
	blk_queue_hardsect_size(d->queue, SECTOR_SIZE);
	d->queue->queuedata = d;

	/* The gendisk structure. */
	if (!(d->gd = alloc_disk(1)))
		return -1;
	d->gd->major = OSPRD_MAJOR;
	d->gd->first_minor = which;
	d->gd->fops = &osprd_ops;
	d->gd->queue = d->queue;
	d->gd->private_data = d;
	snprintf(d->gd->disk_name, 32, "osprd%c", which + 'a');
	set_capacity(d->gd, nsectors);
	add_disk(d->gd);

	/* Call the setup function. */
	osprd_setup(d);

	return 0;
}

static void osprd_exit(void);


// The kernel calls this function when the module is loaded.
// It initializes the 4 osprd block devices.

static int __init osprd_init(void)
{
	int i, r;

	// shut up the compiler
	(void) for_each_open_file;
#ifndef osp_spin_lock
	(void) osp_spin_lock;
	(void) osp_spin_unlock;
#endif

	/* Register the block device name. */
	if (register_blkdev(OSPRD_MAJOR, "osprd") < 0) {
		printk(KERN_WARNING "osprd: unable to get major number\n");
		return -EBUSY;
	}

	/* Initialize the device structures. */
	for (i = r = 0; i < NOSPRD; i++)
		if (setup_device(&osprds[i], i) < 0)
			r = -EINVAL;

	if (r < 0) {
		printk(KERN_EMERG "osprd: can't set up device structures\n");
		osprd_exit();
		return -EBUSY;
	} else
		return 0;
}


// The kernel calls this function to unload the osprd module.
// It destroys the osprd devices.

static void osprd_exit(void)
{
	int i;
	for (i = 0; i < NOSPRD; i++)
		cleanup_device(&osprds[i]);
	unregister_blkdev(OSPRD_MAJOR, "osprd");
}


// Tell Linux to call those functions at init and exit time.
module_init(osprd_init);
module_exit(osprd_exit);
