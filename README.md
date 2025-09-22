# simple-malloc
a simple implementation of glibc's malloc

## introduction
inspired from [njuos 2025 M5: mymalloc](https://jyywiki.cn/OS/2025/labs/M5.md);

It hasn't undergone enough testing yet, and there may still exists many bugs.It is still far from being ready for production use and  can only be used for learning purposes.

Testing(mimalloc-test-stress) has shown that mimalloc's performance is roughly equivalent to that of glibc's malloc and jemalloc (however, its usability and robustness are far inferior to those two).


| Key performance indicators | simple-malloc | glibc malloc | jemalloc |
|---|---|---|---|
| Elapsed Time(s) | 1.398 | 1.388 | 1.419 |
| RSS(Resident Set Size) | 398.2MiB | 435.2MiB | 341.1MiB |
| User CPU Time(s) | 2.992 | 2.874 | 2.968 |

## test steps

### get and compile mimalloc(for benchmark)

```bash
# 1. clone mimalloc 仓库
git clone https://github.com/microsoft/mimalloc.git
cd mimalloc

# 2. compile benchmark
mkdir -p out/release
cd out/release
cmake ../..
make -j mimalloc-test-stress
```

### prepare jemalloc
``` bash
sudo apt-get update
sudo apt-get install libjemalloc-dev
```

### start test
glibc:
```bash
./mimalloc-test-stress 
```

simple-malloc:
```bash
LD_PRELOAD=/path/to/libmyalloc.so ./mimalloc-test-stress
```

jemalloc:
```bash
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./mimalloc-test-stress  
```

## design details

This memory allocator employs a layered design common in modern high-performance allocators. The core principle is to reduce lock contention in multithreaded environments by differentiating the allocation paths for small and large memory blocks, thereby improving concurrent performance. The design is primarily divided into a Fast Path for small memory and a Slow Path for large memory.

The Fast Path (for Small Allocations)
Objective: To provide the fastest possible lock-free allocation experience for small (less than 32KB), frequently requested memory blocks.

Core Mechanism: Thread-Local Storage (TLS). Each thread possesses its own exclusive memory pool. All operations on this pool are lock-free, which fundamentally eliminates contention between threads.

Data Structure: Segregated Free Lists, also known as Size Classes.

A series of fixed-size buckets are predefined (e.g., 8 bytes, 16 bytes, 24 bytes...).

Within each thread's local cache, a singly-linked list is maintained for each size class, linking together free blocks of that specific size.

Workflow:

Allocation (malloc): When a small memory request is made (e.g., 20 bytes), it is rounded up to the nearest size class (24 bytes). A block is then popped from the head of the corresponding linked list. This operation involves only a few pointer manipulations and has a time complexity of O(1).

Deallocation (free): The freed memory block is pushed back onto the head of its corresponding size class list, which is also an O(1) operation.

Cache Refill (refill_thread_local_cache): If a size class's free list is empty, it signifies that the thread-local cache is depleted. In this case, the fast path performs a bulk allocation (requesting a large "Slab") from the slow path. This Slab is then carved up into many smaller blocks to replenish the free list before a block is returned to the user.

The Slow Path (for Large Allocations)
Objective: To manage large memory blocks, handle bulk memory requests from the fast path, and combat memory fragmentation by merging free blocks. All operations must be thread-safe.

Core Mechanism: A single global memory pool protected by a spin mutex. Any thread that needs to allocate from or release memory to this global pool must first acquire the lock.

Data Structure: A Doubly-Linked Free List with a Coalescing strategy.

All large, free memory blocks in the system are linked together in this doubly-linked list.

To efficiently merge adjacent free blocks, I use the Boundary Tags technique. The header and footer of every memory block (whether allocated or free) store metadata about the block's size.

Workflow:

Allocation (global_alloc): After acquiring the lock, the allocator traverses the global free list to find a sufficiently large block (First-Fit strategy). If a suitable block is found, it may be split into two parts: one returned to the user, and the remainder (if large enough) stays in the free list. If no block is found, a new, large segment of memory is requested from the operating system via mmap.

Deallocation (global_free): After acquiring the lock, when a block is freed, it uses its boundary tags to check if its physically adjacent neighbors are also free. If so, they are coalesced into a single, larger free block before being placed in the free list. This mechanism is crucial for reducing External Fragmentation.

Core Design: Unified Metadata and free()
A key feature of this project is its unified myfree(ptr) function, which can automatically identify the origin of any pointer ptr (fast path or slow path) without requiring extra information from the user.

The Challenge: How can free(ptr) know the size of the memory block and whether it should be returned to a thread-local list or the global pool, based only on the ptr?

The Solution: A Unified Metadata Access Layout. I designed a metadata structure that ensures a block's size and flag information is always located exactly sizeof(size_t) bytes before the user pointer ptr.

Metadata Layout
Slow Path Block:
```
+-----------------+-----------------+-----------+-----------------+-----------------+
| prev_free (8B)  | next_free (8B)  | size (8B) |  User Payload...| size_copy (8B)  |
+-----------------+-----------------+-----------+-----------------+-----------------+
^                                               ^                 ^
| Header Start                                  | ptr             | Footer
```
Note that the size field is intentionally placed at the end of the block_header_t struct.

Fast Path Block:
```
+-----------+-----------------+
| size (8B) |  User Payload...  |
+-----------+-----------------+
^           ^
| Block Start | ptr
```
The Flag System
The two least significant bits of the size field are used as flags to store additional information:

Bit 0 (FLAG_ALLOCATED): 1 for allocated, 0 for free.

Bit 1 (FLAG_FAST_PATH): 1 if from the fast path, 0 if from the slow path.

myfree's Decision Logic
When myfree(ptr) is called, it first inspects the value at (size_t*)ptr - 1.

By checking Bit 1 (FLAG_FAST_PATH) of this value, it can instantly determine the block's origin.

If the flag is 1, it executes the fast path deallocation logic (pushing to the thread-local list).

If the flag is 0, it executes the slow path deallocation logic (calling global_free).

This design elegantly solves the unified free problem and is fundamental to the allocator's ability to function correctly and efficiently.