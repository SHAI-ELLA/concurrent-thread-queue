# Concurrent Thread-Safe Queue (C11)

A generic **thread-safe FIFO queue** implemented in C using **C11 threads**.
The queue supports **blocking dequeue** operations and guarantees **FIFO fairness
among sleeping threads**.

The implementation uses **mutexes and condition variables** to ensure correct
synchronization and avoid race conditions, busy waiting, and deadlocks.

---

## Features

- Thread-safe FIFO queue (`void*` items)
- Blocking `dequeue()` when the queue is empty
- **FIFO order for sleeping threads**
- Direct **handoff** of items to waiting threads
- No busy waiting
- Lock-free `visited()` counter using atomics

---

## Design Overview

### Shared State
- FIFO list of queued items (`head_item`, `tail_item`)
- FIFO list of sleeping threads (`wait_head`, `wait_tail`)
- Single mutex protecting all shared state
- Atomic counter for visited items

---

### Enqueue Logic

- **No sleeping threads**  
  → Allocate a new node and append it to the items FIFO.

- **Sleeping threads exist**  
  → Perform **handoff**:
  - Assign the item directly to the **oldest sleeping thread**
  - Wake exactly that thread using its **private condition variable**
  - Avoids waking unnecessary threads

---

### Dequeue Logic

- **Items available**  
  → Remove from FIFO, free the node, return the item.

- **Queue empty**  
  → The thread:
  - Creates a private waiter object on the stack
  - Registers itself in a FIFO waiting list
  - Sleeps on a **private condition variable**
  - Wakes only when an item is handed off to it

This guarantees:
- Correct blocking behavior
- FIFO fairness among waiting threads
- No race conditions or deadlocks

---

### Synchronization Strategy

- A **single mutex** protects all shared pointers and waiter state
- `condition variables` are used for correct sleep/wakeup semantics
- `visited()` is **lock-free**:
  - Implemented using `stdatomic`
  - Safe to call concurrently without acquiring the mutex

---

## API

```c
void   initQueue(void);
void   destroyQueue(void);
void   enqueue(void* item);
void*  dequeue(void);
size_t visited(void);
