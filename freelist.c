/*-------------------------------------------------------------------------
 * 
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/freelist.c,v 1.57 2006/10/04 00:29:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"



/*
 * The shared freelist control information.
 */
typedef struct
{
	/* Clock sweep hand: index of next buffer to consider grabbing */
	int			nextVictimBuffer;

	int			firstFreeBuffer;	/* Head of list of unused buffers */
	int			lastFreeBuffer; /* Tail of list of unused buffers */

	/*
	 * NOTE: lastFreeBuffer is undefined when firstFreeBuffer is -1 (that is,
	 * when the list is empty)
	 */
/*
	 * Statistics.	These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
     */
	uint32		completePasses; /* Complete cycles of the clock sweep */
	uint32		numBufferAllocs;	/* Buffers allocated since last reset */

   
	
} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

/* Backend-local state about whether currently vacuuming */
bool		strategy_hint_vacuum = false;


/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	To ensure that no one else can pin the buffer before we do, we must
 *	return the buffer with the buffer header spinlock still held.  That
 *	means that we return with the BufFreelistLock still held, as well;
 *	the caller must release that lock once the spinlock is dropped.
 */
volatile BufferDesc *
StrategyGetBuffer(void)
{
	volatile BufferDesc *buf;
	int	trycounter;
	int i,last;

	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);

	/*
	 * Try to get a buffer from the freelist.  Note that the freeNext fields
	 * are considered to be protected by the BufFreelistLock not the
	 * individual buffer spinlocks, so it's OK to manipulate them without
	 * holding the spinlock.
	 */
	while (StrategyControl->firstFreeBuffer >= 0)
	{
		buf = &BufferDescriptors[StrategyControl->firstFreeBuffer];
        
		Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);

		/* Unconditionally remove buffer from freelist */
		StrategyControl->firstFreeBuffer = buf->freeNext;
		buf->freeNext = FREENEXT_NOT_IN_LIST;

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot use
		 * it; discard it and retry.  (This can only happen if VACUUM put a
		 * valid buffer in the freelist and then someone else used it before
		 * we got to it.)
		 */
		LockBufHdr(buf);
		
        /* 
		 * LRU Change (RaghunathB)
         * We have to remove usage_count == 0; it serves as the reference bit which is not needed in LRU implementation		 
         */
        
        if (buf->refcount == 0)   // && buf->usage_count == 0) -- this code can be removed as usage_count is not considered in LRU implementation
		{  
			elog(LOG, "Get buf %d\n", buf->buf_id); // Writes to the log whenever a buffer is obtained from the freelist
			return buf;
		}
		UnlockBufHdr(buf);
	}
    /* Nothing on the freelist, so run the LRU replacement algorithm */
    for (;;)
	{
		/* 
		 * LRU Change (RaghunathB)
		 * Postgresql-8.2.19 uses Clock buffer replacement, which removes a buffer from the freelist
		 * if buf->refcount == 0 and buf->usage_count == 0; and the clock pointer rotates with mod(Nbuffers)
		 * But in LRU, the buffer that has been last used (with buf->refcount == 0) has to be taken out 
		 * The basic idea of LRU implementation here is : The index of the buffer last used is stored in a variable
		 * and that buffer is returned.
		 */
		 i=0;
		while(i < NBuffers)   // search all the buffers
		{
			buf = &BufferDescriptors[i]; // Initially  start with first buffer
			LockBufHdr(buf);             // obtain lock so that nobody else uses it
			if (buf->refcount == 0)      // if it is currently free
			{
					last = i;            // store its index
				
			}
			UnlockBufHdr(buf);  		 // unlock the buffer
			i++;
		}
		buf = &BufferDescriptors[last];  // obtain the last used buffer after the loop
		LockBufHdr(buf);
		if (buf->refcount == 0)          // since the lock has been released early itself, checking if its refcount is still 0
        {     
		    elog(LOG, "Get buf %d\n", buf->buf_id); // Writes to the log whenever a buffer is obtained using LRU 
			return buf;
		}
	}

	/* not reached */
	return NULL;
}
    
    
    
	
/*
 * StrategyFreeBuffer: put a buffer on the freelist
 *
 * The buffer is added either at the head or the tail, according to the
 * at_head parameter.  This allows a small amount of control over how
 * quickly the buffer is reused.
 */
void
StrategyFreeBuffer(volatile BufferDesc *buf, bool at_head)
{
	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);

	/*
	 * It is possible that we are told to put something in the freelist that
	 * is already in it; don't screw up the list if so.
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
        /* LRU Change (RaghunathB)
		 * Each time a buffer is added to the freelist, the same action is reflected into the log file 
         */		
        elog(LOG, "Add buf %d\n",buf->buf_id); 
        
		if (at_head) // LRU Change (RaghunathB) this if will never be executed, we always add at the tail in LRU i.e., at_head is always false
		{
			buf->freeNext = StrategyControl->firstFreeBuffer;
			if (buf->freeNext < 0)
				StrategyControl->lastFreeBuffer = buf->buf_id;
			StrategyControl->firstFreeBuffer = buf->buf_id;
		}
		else
		{
			buf->freeNext = FREENEXT_END_OF_LIST;
			if (StrategyControl->firstFreeBuffer < 0)
				StrategyControl->firstFreeBuffer = buf->buf_id;
			else
				BufferDescriptors[StrategyControl->lastFreeBuffer].freeNext = buf->buf_id;
			StrategyControl->lastFreeBuffer = buf->buf_id;
		}
	}

	LWLockRelease(BufFreelistLock);
}

/*
 * StrategySyncStart -- tell BufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BufferSync() will proceed circularly around the buffer array from there.
 */
int
StrategySyncStart(void)
{
	int			result;

	/*
	 * We could probably dispense with the locking here, but just to be safe
	 * ...
	 */
	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);
	result = StrategyControl->nextVictimBuffer;
	LWLockRelease(BufFreelistLock);
	return result;
}

/*
 * StrategyHintVacuum -- tell us whether VACUUM is active
 */
void
StrategyHintVacuum(bool vacuum_active)
{
	strategy_hint_vacuum = vacuum_active;
}


/*
 * StrategyShmemSize
 *
 * estimate the size of shared memory used by the freelist-related structures.
 *
 * Note: for somewhat historical reasons, the buffer lookup hashtable size
 * is also determined here.
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* size of lookup hash table ... see comment in StrategyInitialize */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* size of the shared replacement strategy control block */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	return size;
}

/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *
 * Assumes: All of the buffers are already built into a linked list.
 *		Only called by postmaster and only during initialization.
 */
void
StrategyInitialize(bool init)
{
	bool		found;

	/*
	 * Initialize the shared buffer lookup hashtable.
	 *
	 * Since we can't tolerate running out of lookup table entries, we must be
	 * sure to specify an adequate table size here.  The maximum steady-state
	 * usage is of course NBuffers entries, but BufferAlloc() tries to insert
	 * a new entry before deleting the old.  In principle this could be
	 * happening in each partition concurrently, so we could need as many as
	 * happening in each partition concurrently, so we could need as many as
	 * NBuffers + NUM_BUFFER_PARTITIONS entries.
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * Get or create the shared strategy control block
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		/*
		 * Grab the whole linked list of free buffers for our strategy. We
		 * assume it was previously set up by InitBufferPool().
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;

		/* Initialize the clock sweep pointer */
		StrategyControl->nextVictimBuffer = 0;
	}
	else
		Assert(!init);
}
