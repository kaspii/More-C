#ifndef OSPRD_H
#define OSPRD_H

// ioctl constants
#define OSPRDIOCACQUIRE		42
#define OSPRDIOCTRYACQUIRE	43
#define OSPRDIOCRELEASE		44

/* The internal representation of our device. */
typedef struct osprd_info {
	uint8_t *data;                  // The data array. Its size is
	                                // (nsectors * SECTOR_SIZE) bytes.

	osp_spinlock_t mutex;           // Mutex for synchronizing access to
					// this block device

	unsigned ticket_head;		// Currently running ticket for
					// the device lock

	unsigned ticket_tail;		// Next available ticket for
					// the device lock

	mlist_t tickets; //linked list of tickets
	mlist_t *first_ticket; //points to first-served ticket in queue

	wait_queue_head_t blockq;       // Wait queue for tasks blocked on
					// the device lock

	/* HINT: You may want to add additional fields to help
	         in detecting deadlock. */

	int write_locked;				// Should only ever be 0 or 1.
									// Indicates whether the file is write locked

	int write_lock_pid;				// PID of the process that holds the write lock

	int num_read_locks;				// Indicates the number of read locks on the file

	mlist_t read_lock_pids;			// Linked list of read lock pids
	// The following elements are used internally; you don't need
	// to understand them.
	struct request_queue *queue;    // The device request queue.
	spinlock_t qlock;		// Used internally for mutual
	                                //   exclusion in the 'queue'.
	struct gendisk *gd;             // The generic disk.
} osprd_info_t;

#endif
