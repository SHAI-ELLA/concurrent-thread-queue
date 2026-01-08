#include <stdlib.h>
#include <threads.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdbool.h>

//gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 -pthread -c queue.c

/* =======================
   Data structures
   ======================= */

typedef struct node {
    void* item;            /* generic item (HW API is void*) */
    struct node* next;     /* next node in FIFO queue */
} node_t;

/* 
 * A "waiter" represents ONE thread that called dequeue() and had to sleep.
 * We keep waiters in a FIFO list to satisfy the HW requirement:
 * oldest sleeping thread must get the next item first.
 */

typedef struct waiter {
    cnd_t cv;              /* private CV for this specific sleeping thread */
    void* assigned_item;        /* item handed directly to this waiter (handoff) */
    bool has_item;         /* set to true by enqueue() before waking this thread */
    struct waiter* next;   /* FIFO list of sleeping threads */
} waiter_t;

/* Queue state (shared) */
static node_t* head_item = NULL;   /* FIFO items list head */
static node_t* tail_item = NULL;   /* FIFO items list tail */

static waiter_t* wait_head = NULL;
static waiter_t* wait_tail = NULL;

/* 
 * One mutex protects ALL shared queue state:
 * - head/tail (items list)
 * - wait_head/wait_tail (sleepers list)
 * Without this, enqueue/dequeue could race and corrupt pointers.
 */
static mtx_t q_lock;

/* 
 * visited counter:
 * HW says visited() is NOT allowed to take a lock.
 * So we store this counter as atomic to avoid UB/data races.
 */
static _Atomic size_t visited_cnt;

/* =======================
   5 functions - init, destroy, enqueue, dequeue, visited
   ======================= */

/* Called once before use (single-threaded) */
void initQueue(void) {
    head_item = NULL;
    tail_item = NULL;
    wait_head = NULL;
    wait_tail = NULL;

    /* atomic init so visited() can read safely without locks */
    atomic_store(&visited_cnt, 0);

    mtx_init(&q_lock, mtx_plain);
}

/* 
 * Called when the queue is no longer needed.
 * IMPORTANT HW assumptions for destroyQueue():
 * - destroyQueue() runs "solo" (no concurrent enqueue/dequeue)
 * - no threads are blocked/sleeping in dequeue()
 * - queue is empty when destroyQueue() is called
 *
 * Because of those assumptions:
 * - we do NOT need to free nodes in the items FIFO (it should already be empty)
 * - we do NOT need to wake sleepers / destroy their CVs (there are none)
 * - we only tear down our global synchronization object and reset pointers
 */
void destroyQueue(void) {
    /* no one else is using q_lock right now (HW assumption), so it's safe to destroy */
    mtx_destroy(&q_lock);

    /*reseting all shared data*/
    head_item = NULL;
    tail_item = NULL;
    wait_head = NULL;
    wait_tail = NULL;
    /* visited_cnt is reset in initQueue(), no need to touch it here */
}

/* 
 * Adds an item to the queue.
 *
 * Key HW requirement: FIFO order for sleeping threads.
 * If the queue is empty and threads are sleeping, the next items should go to them
 * in the order they went to sleep.
 *
 * Implementation idea here:
 * - If there are NO sleeping threads => push item into normal FIFO (head/tail).
 * - If there ARE sleeping threads => do "handoff":
 *      give the item directly to the oldest waiter (wait_head) and wake ONLY it.
 * This avoids waking the wrong thread and avoids a thread waking just to sleep again.
 */
void enqueue(void* item) {
    
    /* protect shared pointers + waiter fields while we modify them */
    mtx_lock(&q_lock);

   /* Case 1: no sleepers => regular FIFO enqueue into the items list */
   if(wait_head == NULL){

        /* create a node that must live after enqueue returns => malloc */
        node_t* n = malloc(sizeof(node_t)); /* malloc failure handling not required */
        n->item = item;
        n->next = NULL;

        /* empty items queue => head and tail both become n */
        if(head_item == NULL){
            head_item = n;
            tail_item = n;
        }else{ /* insert the item to an existing FIFO of items*/
            tail_item->next = n;
            tail_item = n;
        }
   }else{ 

    /* Case 2: there are sleeping dequeue threads.
         * HW says the oldest sleeping thread must get the next item.
         * So we "handoff" item directly to wait_head and wake it.
         */

        waiter_t* w = wait_head; /* oldest sleeper */

        /* assign the item to that waiter before waking it */
        w->assigned_item = item;
        w->has_item = true;

        /* pop from sleepers FIFO */
        wait_head = w->next;
        if (wait_head == NULL) {
            wait_tail = NULL;
        }

        /* wake exactly this specific sleeping thread (not random) */
        cnd_signal(&w->cv);
   }
    mtx_unlock(&q_lock);
}

/* Removes an item from the queue; blocks if empty
consumer has to wait when there are no items but he is not allowed to wait while it holds mutex
otherwise enqueue will not work, so instead of just mutex we'll use condition veriable
*/
void* dequeue(void) {

    /* 
     * Lock the mutex before accessing shared data.
     * head/tail and the waiting threads queue are shared between threads,
     * so without a mutex we could have race conditions.
     */
    mtx_lock(&q_lock);

    /* 
     * CASE 1: There is already an item in the queue.
     * In this case we should NOT go to sleep.
     * According to the assignment, a thread may sleep only if no item is available.
     */
    if (head_item != NULL) {

        /* Save the current head node */
        node_t* n = head_item;

        /* Remove the node from the queue (FIFO order) */
        head_item = n->next;

        /* If the queue became empty, update tail as well */
        if (head_item == NULL) {
            tail_item = NULL;
        }

        /* Extract the actual item stored in the node */
        void* item = n->item;

        /* Free the node.
         * enqueue() allocated it with malloc, so dequeue() is responsible for freeing it.
         */
        free(n);

        /* 
         * Update visited counter.
         * The assignment defines "visited" as items that were enqueued AND dequeued.
         * We update it here because the item is now actually consumed.
         * Atomic operation is used so visited() does not need a lock.
         */
        atomic_fetch_add(&visited_cnt, 1);

        /* Release the mutex before returning */
        mtx_unlock(&q_lock);

        /* Return the dequeued item to the caller */
        return item;
    }

    /* 
     * CASE 2: The queue is empty.
     * According to the assignment, dequeue must block in this case.
     * We implement blocking using a waiter object and a condition variable.
     */

    /* 
     * Create a waiter object on the stack.
     * We do NOT use malloc here because the waiter is only needed
     * while this thread is inside dequeue().
     */
    waiter_t w;

    /* Initialize waiter fields */
    w.assigned_item = NULL;     /* Item will be assigned by enqueue() */
    w.has_item = false;    /* Indicates whether we already received an item */
    w.next = NULL;         /* For FIFO waiting threads list */

    /* 
     * Initialize a private condition variable for this waiting thread.
     * Using a private CV allows us to wake exactly the correct thread,
     * which is required to preserve FIFO order of sleeping threads.
     */
    cnd_init(&w.cv);

    /* 
     * Insert this waiter into the FIFO queue of waiting threads.
     * This ensures that the first thread that went to sleep
     * will be the first to receive the next item.
     */
    if (wait_tail == NULL) {
        /* No waiting threads yet */
        wait_head = &w;
        wait_tail = &w;
    } else {
        /* Append to the end of the waiting queue */
        wait_tail->next = &w;
        wait_tail = &w;
    }

    /* 
     * Wait until an item is assigned to this waiter.
     * We use a while loop (and not if) because:
     * 1. cnd_wait releases the mutex while sleeping
     * 2. When we wake up, the condition must be rechecked
     * This is the standard and correct condition-variable pattern.
     */
    while (!w.has_item) {

        /* 
         * cnd_wait atomically:
         *  - releases the mutex
         *  - puts the thread to sleep
         *  - re-acquires the mutex when woken up
         *
         * This allows enqueue() to run while we are sleeping,
         * and prevents deadlocks.
         */
        cnd_wait(&w.cv, &q_lock);
    }

    /* 
     * At this point enqueue() has:
     *  - assigned an item to this waiter
     *  - set has_item = true
     *  - signaled this condition variable
     */
    void* item = w.assigned_item;

    /* 
     * Destroy the condition variable.
     * It was used only for this dequeue call and should not leak resources.
     */
    cnd_destroy(&w.cv);

    /* Update visited counter (item has been successfully dequeued) */
    atomic_fetch_add(&visited_cnt, 1);

    /* Release the mutex */
    mtx_unlock(&q_lock);

    /* Return the item that was handed directly to this waiting thread */
    return item;
}


/* 
 * visited():
 * HW requirement:
 * - must NOT take a lock (no mtx_lock here)
 * - may be slightly inaccurate under concurrency, but must NOT cause UB
 *
 * How we satisfy it:
 * - visited_cnt is atomic
 * - dequeue() updates it using atomic_fetch_add
 * - visited() reads it using atomic_load
 *
 * This way visited() is lock-free and data-race-free.
 * we therefore return the number of items that entered in enqueue() and left in dequeue()
 */
size_t visited(void) {
    return atomic_load(&visited_cnt);
}