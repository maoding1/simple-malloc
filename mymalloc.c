#include "mymalloc.h"

#include <stdbool.h>
#include <stdint.h>
spinlock_t big_lock;

// 小内存与大内存分界点
#define SMALL_ALLOC_THRESHOLD (32 * 1024)  // 32KB
// 内存8字节对齐
#define ALIGNMENT 8
// 快速路径中最大的小内存大小
#define MAX_SMALL_SIZE 256
// 快速路径中大小类的数量
#define NUM_SMALL_CLASSES (MAX_SMALL_SIZE / ALIGNMENT)
// 全局分配器一次性从操作系统申请的最小内存
#define GLOBAL_ARENA_MIN_SIZE (64 * 1024)  // 64KB

static inline size_t align_size(size_t size) {
  return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

// --- 标志位 ---
#define FLAG_ALLOCATED 1UL  // 分配标志位，最低位为1表示已分配, 0表示空闲
#define FLAG_FAST_PATH \
  2UL  // 快速路径标志位，次低位为1表示来自快速路径, 0表示来自慢速路径

/** 慢速路径： 使用带全局锁的分配器 数据结构为带合并的空闲链表 */
typedef struct block_header {
  struct block_header* prev_free;
  struct block_header* next_free;
  size_t size;  // size为aligned后的大小，为节省空间,最低位用作'是否空闲'的标记
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)
#define FOOTER_SIZE sizeof(size_t)  // 尾部只存一个大小

// 从用户指针获取头部
#define GET_HEADER(ptr) ((block_header_t*)((char*)(ptr)-HEADER_SIZE))
// 从头部获取用户指针
#define GET_PAYLOAD(header) ((void*)((char*)(header) + HEADER_SIZE))

// 读写块大小和空闲标记
#define GET_SIZE(header) ((header)->size & ~3UL)
#define IS_FREE(header) \
  (((header)->size & FLAG_ALLOCATED) == 0)  // size 最低位为0表示free
#define SET_FREE(header) ((header)->size &= ~FLAG_ALLOCATED)
#define SET_ALLOCATED(header) ((header)->size |= FLAG_ALLOCATED)

#define GET_FOOTER(header) \
  ((size_t*)((char*)(header) + GET_SIZE(header) - FOOTER_SIZE))
#define NEXT_BLOCK(header) \
  ((block_header_t*)((char*)(header) + GET_SIZE(header)))
#define PREV_BLOCK(header)             \
  ((block_header_t*)((char*)(header) - \
                     (*(size_t*)((char*)(header)-FOOTER_SIZE))))

static block_header_t* global_free_list_head = NULL;

void remove_from_free_list(block_header_t* block) {
  if (block->prev_free) {
    block->prev_free->next_free = block->next_free;
  } else {
    global_free_list_head = block->next_free;
  }

  if (block->next_free) {
    block->next_free->prev_free = block->prev_free;
  }
}

void add_to_free_list(block_header_t* block) {
  block->prev_free = NULL;
  block->next_free = global_free_list_head;
  if (global_free_list_head) {
    global_free_list_head->prev_free = block;
  }
  global_free_list_head = block;
}

void global_free(void* ptr);

static block_header_t* request_from_os(size_t size) {
  size_t total_size =
      align_size(size > GLOBAL_ARENA_MIN_SIZE ? size : GLOBAL_ARENA_MIN_SIZE);

  void* addr = vmalloc(NULL, total_size);
  if (addr == NULL) {
    return NULL;
  }

  block_header_t* header = (block_header_t*)addr;
  header->size = total_size;
  SET_ALLOCATED(header);
  *GET_FOOTER(header) = header->size;

  return header;
}

void* global_alloc(size_t size) {
  size_t total_size = align_size(size + HEADER_SIZE + FOOTER_SIZE);

  spin_lock(&big_lock);
  while (1) {
    block_header_t* current = global_free_list_head;
    while (current) {
      if (GET_SIZE(current) >= total_size) {
        remove_from_free_list(current);
        size_t remainning_size = GET_SIZE(current) - total_size;

        if (remainning_size >= HEADER_SIZE + FOOTER_SIZE + ALIGNMENT) {
          // 剩余空间能够分裂成一个新块
          current->size = total_size;
          SET_ALLOCATED(current);
          *GET_FOOTER(current) = total_size;

          block_header_t* new_block = NEXT_BLOCK(current);
          new_block->size = remainning_size;
          SET_FREE(new_block);
          *GET_FOOTER(new_block) = new_block->size;
          add_to_free_list(new_block);
        } else {
          // 不分裂
          SET_ALLOCATED(current);
          *GET_FOOTER(current) = current->size;
        }

        spin_unlock(&big_lock);
        return GET_PAYLOAD(current);
      }
      current = current->next_free;
    }

    block_header_t* new_arena = request_from_os(total_size);
    if (new_arena == NULL) {
      spin_unlock(&big_lock);
      return NULL;
    }
    add_to_free_list(new_arena);
  }
}

void global_free(void* ptr) {
  if (ptr == NULL) return;

  spin_lock(&big_lock);

  block_header_t* header = GET_HEADER(ptr);
  size_t block_size = GET_SIZE(header);

  // 1. 合并后面的物理块
  block_header_t* next = NEXT_BLOCK(header);
  // fix me: 需要对next不合法的情况做复杂的边界检查
  if (IS_FREE(next)) {
    remove_from_free_list(next);
    block_size += GET_SIZE(next);
  }

  // 2.合并前面的物理块
  size_t prev_footer_size = *(size_t*)((char*)header - FOOTER_SIZE);
  if ((prev_footer_size & 1) == 0) {
    block_header_t* prev = PREV_BLOCK(header);
    remove_from_free_list(prev);
    block_size += GET_SIZE(prev);
    header = prev;
  }

  // 更新合并后的大块的元数据
  header->size = block_size;
  SET_FREE(header);
  *GET_FOOTER(header) = header->size;

  add_to_free_list(header);

  spin_unlock(&big_lock);
}

// --- 快速路径： 线程本地缓存 使用分离式空闲链表 ---
typedef struct small_block {
  struct small_block* next;
} small_block_t;

static _Thread_local small_block_t* thread_local_free_lists[NUM_SMALL_CLASSES] =
    {NULL};

static void refill_thread_local_cache(int class_index) {
  size_t block_size = (class_index + 1) * ALIGNMENT;

  size_t slab_size = block_size * 20;

  char* slab = (char*)global_alloc(slab_size);
  if (slab == NULL) {
    return;
  }

  for (int i = 0; i < 20; ++i) {
    small_block_t* current_block = (small_block_t*)(slab + i * block_size);
    current_block->next = thread_local_free_lists[class_index];
    thread_local_free_lists[class_index] = current_block;
  }
}

void* vmalloc(void* addr, size_t length) {
  // length must be aligned to page size (4096).
  void* result = mmap(addr, length, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (result == MAP_FAILED) {
    return NULL;
  }
  return result;
}

void vmfree(void* addr, size_t length) { munmap(addr, length); }

void* mymalloc(size_t size) {
  if (size == 0) return NULL;

  // 慢速路径
  if (size > SMALL_ALLOC_THRESHOLD) {
    return global_alloc(size);
  }

  // 快速路径

  size_t required_size_with_meta = size + sizeof(size_t);
  size_t aligned_size = align_size(required_size_with_meta);

  if (aligned_size > MAX_SMALL_SIZE) {
    // global_alloc 内部已经处理了更大的元数据，因此不需要自己加元数据
    return global_alloc(size);
  }

  int class_index = aligned_size / ALIGNMENT - 1;

  if (thread_local_free_lists[class_index] == NULL) {
    refill_thread_local_cache(class_index);
    if (thread_local_free_lists[class_index] == NULL) return NULL;
  }

  small_block_t* block = thread_local_free_lists[class_index];
  thread_local_free_lists[class_index] = block->next;

  // 存储大小信息 (不设最低位,表示来自快速路径)
  *((size_t*)block) = aligned_size | FLAG_ALLOCATED | FLAG_FAST_PATH;

  return (void*)((char*)block + sizeof(size_t));
}

void myfree(void* ptr) {
  // 难点是如何判断ptr是来自快速路径还是慢速路径
  if (!ptr) {
    return;
  }

  size_t* size_ptr = (size_t*)((char*)ptr - sizeof(size_t));
  size_t size_info = *size_ptr;

  if (size_info & FLAG_FAST_PATH) {
    // 快速路径
    size_t block_size = size_info & ~3UL;  // 清除标志位
    int class_index = block_size / ALIGNMENT - 1;
    small_block_t* block = (small_block_t*)size_ptr;
    block->next = thread_local_free_lists[class_index];
    thread_local_free_lists[class_index] = block;

  } else {
    // 慢速路径
    global_free(ptr);
  }
}
