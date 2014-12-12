/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the entry point of the application.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////////
//   Preprocessor   //
////////////////////*/
#ifdef  DISKIO_LINUX_STANDALONE
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS    64

#if (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)
#define COMPILER_MFENCE_READ       __sync_synchronize()
#define COMPILER_MFENCE_WRITE      __sync_synchronize()
#define COMPILER_MFENCE_READ_WRITE __sync_synchronize()
#elif defined(__ppc__) || defined(__powerpc__) || defined(__PPC__)
#define COMPILER_MFENCE_READ       asm volatile("sync":::"memory")
#define COMPILER_MFENCE_WRITE      asm volatile("sync":::"memory")
#define COMPILER_MFENCE_READ_WRITE asm volatile("sync":::"memory")
#elif defined(__i386__) || defined(__i486__) || defined(__i586__) || \
      defined(__i686__) || defined(__x86_64__)
#define COMPILER_MFENCE_READ       asm volatile("lfence":::"memory")
#define COMPILER_MFENCE_WRITE      asm volatile("sfence":::"memory")
#define COMPILER_MFENCE_READ_WRITE asm volatile("mfence":::"memory")
#else
#error Unsupported __GNUC__ (need memory fence intrinsics)!
#endif

#define never_inline               __attribute__((noinline))

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS              MAP_ANON
#endif
#endif /* defined(DISKIO_LINUX_STANDALONE) */

/*////////////////
//   Includes   //
////////////////*/
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libaio.h>
#include <pthread.h>
#include <execinfo.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/param.h>
#include <sys/eventfd.h>

#include "bridge.h"

/*/////////////////
//   Constants   //
/////////////////*/
/// The scale used to convert from seconds into nanoseconds.
static uint64_t const SEC_TO_NANOSEC = 1000000000ULL;

/// @summary Define the number of times per-second we want the I/O system to
/// update (assuming it's on a background thread and we have that control).
/// The lower the update rate of the I/O system, the more latency there is in
/// processing and completing I/O requests, and the lower the I/O thoroughput.
static size_t   const IO_SYSTEM_RATE = 60;

/// @summary Define the maximum number of concurrently open files.
static size_t   const MAX_OPEN_FILES = 128;

/// @summary Define the maximum number of concurrently active AIO operations.
/// We set this based on what the maximum number of AIO operations we want to
/// poll during each tick, and the maximum number the underlying OS can handle.
/// TODO: Make this overridable at compile time.
static size_t   const AIO_MAX_ACTIVE = 512;

/// @summary Define the size of the I/O buffer. This is calculated based on an
/// maximum I/O transfer rate of 960MB/s, and an I/O system tick rate of 60Hz;
/// 960MB/sec divided across 60 ticks/sec gives 16MB/tick maximum transfer rate.
/// TODO: Make this overridable at compile time.
static size_t   const VFS_IOBUF_SIZE = 16 * 1024 * 1024;

/// @summary Define the size of the buffer allocated for each I/O request.
static size_t   const VFS_ALLOC_SIZE = VFS_IOBUF_SIZE / AIO_MAX_ACTIVE;

/// @summary Define the supported AIO commands.
enum aio_command_e
{
    AIO_COMMAND_READ     = 0, /// Data should be read from the file.
    AIO_COMMAND_WRITE    = 1, /// Data should be written to the file.
    AIO_COMMAND_FLUSH    = 2, /// Any pending writes should be flushed to disk.
    AIO_COMMAND_CLOSE    = 3, /// The file should be closed.
};

/// @summary Define the supported VFS access modes for a file.
enum vfs_mode_e
{
    READ_LOAD            = 0, /// The file is being loaded entirely and then closed.
    READ_EXPLICIT        = 1, /// The file reads are controlled by the user.
};

/*///////////////////
//   Local Types   //
///////////////////*/
/// @summary Defines the data associated with a fixed-size lookaside queue.
/// Lookaside means that the storage for items are stored and managed outside
/// of the structure. The queue is safe for concurrent access by a single reader
/// and a single writer. Reads and writes of 32-bit values must be atomic. DO
/// NOT DIRECTLY ACCESS THE FIELDS OF THIS STRUCTURE. This is used internally
/// by several of the data types in the platform-specific portions of the I/O
/// module, but this structure and its operations are safe for cross-platform
/// use as long as reads and writes of 32-bit values are atomic on the platform.
struct srsw_flq_t
{
    uint32_t          PushedCount;   /// Number of push operations performed.
    uint32_t          PoppedCount;   /// Number of pop operations performed.
    uint32_t          Capacity;      /// The queue capacity. Always a power-of-two.
};

/// @summary Defines the data associated with a fixed-size queue safe for
/// concurrent access by a single reader and a single writer. Depends on the
/// srsw_flq_t above, so the same restrictions and caveats apply here. The
/// value N must be a power of two greater than zero.
template <typename T, uint32_t N>
struct srsw_fifo_t
{
    srsw_flq_t         Queue;        /// Maintains queue state and capacity.
    T                  Store[N];     /// Storage for the queue items.
};

/// @summary Defines a free list safe for concurrent access by a single reader
/// and multiple writers. The type T is the node type, which must include field:
///     T * Next;
/// Additionally, the type T should be a POD type.
template <typename T>
struct srmw_freelist_t
{
    pthread_mutex_t    Lock;         /// Mutex protecting the Head pointer.
    T                 *Head;         /// The first free node.
    T                  XXXX;         /// A dummy node used to simplify logic.
};

/// @summary An unbounded multi-producer, single-consumer queue.
/// The type T is the node type, which must include the following field:
///     T * Next;
/// Additionally, the type T should be a POD type.
template <typename T>
struct srmw_fifo_t
{
    pthread_mutex_t    HeadLock;     /// Mutex protecting the Head pointer.
    T                 *Head;         /// The current front-of-queue.
    pthread_mutex_t    TailLock;     /// Mutex protecting the Tail pointer.
    T                 *Tail;         /// The current end-of-queue.
    srmw_freelist_t<T> FreeList;     /// The SRMW free list for node recycling.
    T                 *XXXX;         /// Pointer to a dummy node.
};

/// @summary Defines the state associated with a direct I/O buffer manager.
/// This object allocates a single large chunk of memory aligned to a multiple
/// of the physical disk sector size, and then allows the caller to allocate
/// fixed-size chunks from within that buffer. The allocator can only be used
/// from a single thread. This allocator can also be used for cached I/O.
struct iobuf_alloc_t
{
    size_t             TotalSize;    /// The total number of bytes allocated.
    size_t             PageSize;     /// The size of a single page, in bytes.
    size_t             AllocSize;    /// The size of a single allocation, in bytes.
    void              *BaseAddress;  /// The base address of the committed range.
    size_t             FreeCount;    /// The number of unallocated AllocSize blocks.
    void             **FreeList;     /// Pointers to the start of each unallocated block.
};

/// @summary Defines the data associated with an AIO request to read, write,
/// flush or close a file. These requests are populated by the VFS driver and
/// sent to the AIO driver, where they are transformed into an iocb structure.
struct aio_req_t
{
    int32_t            Command;      /// The AIO command type, one of aio_command_e.
    int                Fildes;       /// The file descriptor of the file. Required.
    int                Eventfd;      /// The eventfd descriptor for epoll signaling, or -1.
    uint32_t           DataAmount;   /// The amount of data to transfer, or 0.
    int64_t            FileOffset;   /// The absolute byte offset of the start of the operation, or 0.
    void              *DataBuffer;   /// The source or target buffer, or NULL.
    uint64_t           QTimeNanos;   /// The request submission timestamp, in nanoseconds.
    uint64_t           ATimeNanos;   /// The request activation timestamp, in nanoseconds.
    intptr_t           AFID;         /// The application-defined ID for the file.
    int32_t            Type;         /// The file type, one of file_type_e.
    int32_t            Reserved;     /// Reserved for future use. Set to 0.
};

/// @summary Defines the data associated with a completed AIO operation. These
/// results are populated by the AIO driver and posted to queues polled by the VFS driver.
struct aio_res_t
{
    int                Fildes;       /// The file descriptor of the file.
    int                Eventfd;      /// The eventfd descriptor for epoll signaling, or -1.
    int                OSError;      /// The error code returned by the operation, or 0.
    uint32_t           DataAmount;   /// The amount of data transferred.
    int64_t            FileOffset;   /// The absolute byte offset of the start of the operation, or 0.
    void              *DataBuffer;   /// The source or target buffer, or NULL.
    uint64_t           QTimeNanos;   /// The request submission timestamp, in nanoseconds.
    uint64_t           CTimeNanos;   /// The request completion timestamp, in nanoseconds.
    intptr_t           AFID;         /// The application-defined ID for the file.
    int32_t            Type;         /// The file type, one of file_type_e.
    int32_t            Reserved;     /// Reserved for future use. Set to 0.
};

/// @summary Typedef some queue configurations for use in aio_state_t.
/// Note that queue the queue capacity must always be a power of two.
typedef srsw_fifo_t<aio_res_t,   64> aio_cresultq_t; /// Queue for close operation results.
typedef srsw_fifo_t<aio_res_t,   64> aio_fresultq_t; /// Queue for flush operation results.
typedef srsw_fifo_t<aio_res_t, 1024> aio_rresultq_t; /// Queue for read operation results.
typedef srsw_fifo_t<aio_res_t, 1024> aio_wresultq_t; /// Queue for write operation results.
typedef srsw_fifo_t<aio_req_t, 1024> aio_requestq_t; /// Queue for all operation requests.

/// @summary Define the state associated with the AIO driver. The AIO driver
/// receives requests to read, write, flush and close files, and then queues
/// kernel AIO operations to perform them.
struct aio_state_t
{
    #define MA         AIO_MAX_ACTIVE
    aio_requestq_t     RequestQueue; /// The queue for all pending AIO requests.
    struct iocb        IOCBPool[MA]; /// The static pool of IOCB structures.
    io_context_t       AIOContext;   /// The kernel AIO context descriptor.
    size_t             ActiveCount;  /// The number of in-flight AIO requests.
    aio_req_t          AAIOList[MA]; /// The set of active AIO requests [ActiveCount valid].
    struct iocb       *IOCBList[MA]; /// The dynamic list of active IOCBs [ActiveCount valid].
    size_t             IOCBFreeCount;/// The number of available IOCBs.
    struct iocb       *IOCBFree[MA]; /// The list of available IOCBs [IOCBFreeCount valid].
    aio_rresultq_t     ReadResults;  /// Queue for completed read  operations.
    aio_wresultq_t     WriteResults; /// Queue for completed write operations.
    aio_fresultq_t     FlushResults; /// Queue for completed flush operations.
    aio_cresultq_t     CloseResults; /// Queue for completed close operations.
    #undef MA
};

/// @summary Defines the data associated with a file load request passed to the
/// VFS driver. This structure is intended for storage in a srmw_fifo_t.
struct vfs_lfreq_t
{
    vfs_lfreq_t       *Next;         /// Pointer to the next node in the queue.
    int                Fildes;       /// The file descriptor of the opened file.
    int                Eventfd;      /// The eventfd descriptor of the opened file.
    int64_t            DataSize;     /// The logical size of the file, in bytes.
    int64_t            FileSize;     /// The physical size of the file, in bytes.
    int64_t            FileOffset;   /// The byte offset of the start of the file data.
    intptr_t           AFID;         /// The application-defined ID for the file.
    int32_t            Type;         /// The file type, one of file_type_e.
    uint32_t           Priority;     /// The file access priority (0 = highest).
    size_t             SectorSize;   /// The physical sector size of the disk.
};

/// @summary Defines the data associated with a file close request passed to the
/// VFS driver. This structure is intended for storage in a srmw_fifo_t.
struct vfs_cfreq_t
{
    vfs_cfreq_t       *Next;         /// Pointer to the next node in the queue.
    intptr_t           AFID;         /// The application-defined ID for the file.
};

typedef srmw_fifo_t<vfs_lfreq_t>     vfs_lfq_t; /// Load file request queue.
typedef srmw_fifo_t<vfs_cfreq_t>     vfs_cfq_t; /// Close file request queue.

/// @summary Information that remains constant from the point that a file is opened for reading.
struct vfs_fdinfo_t
{
    int                Fildes;       /// The file descriptor for the file.
    int                Eventfd;      /// The eventfd descriptor for the file, or -1.
    int64_t            FileSize;     /// The physical file size, in bytes.
    int64_t            DataSize;     /// The file size after any size-changing transforms.
    int64_t            FileOffset;   /// The absolute byte offset of the start of the file data.
    size_t             SectorSize;   /// The disk physical sector size, in bytes.
};

/// @summary Defines the data associated with a priority queue of pending AIO operations.
/// TODO: Determine if 32-bits for the insertion ID is enough. This would be enough for
/// 128TB of data, which if you were streaming 32KB chunks continuously at 980MB/sec, is
/// enough for ~39 hours before the counter wraps around.
struct vfs_io_opq_t
{
    #define MO         AIO_MAX_ACTIVE
    int32_t            Count;        /// The number of items in the queue.
    uint32_t           InsertionId;  /// The counter for tagging each AIO request.
    uint32_t           Priority[MO]; /// The priority value for each item.
    uint32_t           InsertId[MO]; /// The inserion order value for each item.
    aio_req_t          Request [MO]; /// The populated AIO request for each item.
    #undef MO
};

/// @summary Defines the data associated with a priority queue of files. This queue
/// is used to determine which files get a chance to submit I/O operations.
struct vfs_io_fpq_t
{
    #define MF         MAX_OPEN_FILES
    int32_t            Count;        /// The number of items in the queue.
    uint32_t           Priority[MF]; /// The priority value for each file.
    uint16_t           RecIndex[MF]; /// The index of the file record.
    #undef MF
};

/// @summary Defines the state data maintained by a VFS driver instance.
struct vfs_state_t
{
    #define MF         MAX_OPEN_FILES
    #define NT         FILE_TYPE_COUNT
    vfs_cfq_t          CloseQueue;   /// The queue for file close requests.
    vfs_lfq_t          LoadQueue;    /// The queue for complete file load requests.
    iobuf_alloc_t      IoAllocator;  /// The I/O buffer allocator.
    size_t             ActiveCount;  /// The number of open files.
    intptr_t           FileAFID[MF]; /// An application-defined ID for each active file.
    int32_t            FileType[MF]; /// One of file_mode_e for each active file.
    int32_t            FileMode[MF]; /// One of vfs_mode_e for each active file.
    uint32_t           Priority[MF]; /// The access priority for each active file.
    int64_t            RdOffset[MF]; /// The current read offset for each active file.
    vfs_fdinfo_t       FileInfo[MF]; /// The constant data for each active file.
    vfs_io_opq_t       IoOperations; /// The priority queue of pending I/O operations.
    #undef NT
    #undef MF
};

/*///////////////
//   Globals   //
///////////////*/
/// @summary A list of all of the file type identifiers we consider to be valid.
static file_type_e FILE_TYPE_LIST[] = {
    FILE_TYPE_DDS,
    FILE_TYPE_TGA,
    FILE_TYPE_WAV,
    FILE_TYPE_JSON
};

/*///////////////////////
//   Local Functions   //
///////////////////////*/
/// @summary Rounds a size up to the nearest even multiple of a given power-of-two.
/// @param size The size value to round up.
/// @param pow2 The power-of-two alignment.
/// @return The input size, rounded up to the nearest even multiple of pow2.
static inline size_t align_up(size_t size, size_t pow2)
{
    assert((pow2 & (pow2-1)) == 0);
    return (size == 0) ? pow2 : ((size + (pow2-1)) & ~(pow2-1));
}

/// @summary Clamps a value to a given maximum.
/// @param size The size value to clamp.
/// @param limit The upper-bound to clamp to.
/// @return The smaller of size and limit.
static inline size_t clamp_to(size_t size, size_t limit)
{
    return (size > limit) ? limit : size;
}

/// @summary Reads the current tick count for use as a timestamp.
/// @return The current timestamp value, in nanoseconds.
static inline uint64_t nanotime(void)
{
    struct timespec tsc;
    clock_gettime(CLOCK_MONOTONIC, &tsc);
    return (SEC_TO_NANOSEC * uint64_t(tsc.tv_sec) + uint64_t(tsc.tv_nsec));
}

/// @summary Atomically writes a 32-bit unsigned integer value to a given address.
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be 32-bit aligned.
/// @param value The value to write to address.
static never_inline void atomic_write_uint32_aligned(uintptr_t address, uint32_t value)
{
    assert((address & 0x03) == 0);                  // assert address is 32-bit aligned
    uint32_t *p  = (uint32_t*) address;
    *p = value;
}

/// @summary Atomically writes a pointer-sized value to a given address.
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be aligned to the pointer size.
/// @param value The value to write to address.
static never_inline void atomic_write_pointer_aligned(uintptr_t address, uintptr_t value)
{
    assert((address & (sizeof(uintptr_t)-1)) == 0); // assert address is pointer-size aligned
    uintptr_t *p = (uintptr_t*) address;
    *p = value;
}

/// @summary Atomically reads a 32-bit unsigned integer value from a given address.
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be 32-bit aligned.
/// @return The value read from the specified address.
static never_inline uint32_t atomic_read_uint32_aligned(uintptr_t address)
{
    assert((address & 0x03) == 0);
    volatile uint32_t *p = (uint32_t*) address;
    return (*p);
}

/// @summary Clears or initializes a SRSW fixed lookaside queue to empty.
/// @param srswq The queue to initialize.
/// @param capacity The queue capacity. This must be a power-of-two.
static inline void srsw_flq_clear(srsw_flq_t &srswq, uint32_t capacity)
{
    assert((capacity & (capacity-1)) == 0); // capacity is a power-of-two.
    srswq.PushedCount = 0;
    srswq.PoppedCount = 0;
    srswq.Capacity    = capacity;
}

/// @summary Retrieves the number of items currently available in a SRSW fixed
/// lookaside queue. Do not pop more than the number of items returned by this call.
/// @param srswq The queue to query.
static inline uint32_t srsw_flq_count(srsw_flq_t &srswq)
{
    uintptr_t pushed_cnt_addr = (uintptr_t) &srswq.PushedCount;
    uintptr_t popped_cnt_addr = (uintptr_t) &srswq.PoppedCount;
    uint32_t  pushed_cnt      = atomic_read_uint32_aligned(pushed_cnt_addr);
    uint32_t  popped_cnt      = atomic_read_uint32_aligned(popped_cnt_addr);
    return (pushed_cnt - popped_cnt); // unsigned; don't need to worry about overflow.
}

/// @summary Checks whether a SRSW fixed lookaside queue is full. Check this
/// before pushing an item into the queue.
/// @param srswq The queue to query.
/// @return true if the queue is full.
static inline bool srsw_flq_full(srsw_flq_t &srswq)
{
    return (srsw_flq_count(srswq) == srswq.Capacity);
}

/// @summary Checks whether a SRSW fixed lookaside queue is empty. Check this
/// before popping an item from the queue.
/// @param srswq The queue to query.
/// @return true if the queue is empty.
static inline bool srsw_flq_empty(srsw_flq_t &srswq)
{
    return (srsw_flq_count(srswq) == 0);
}

/// @summary Gets the index the next push operation will write to. This must be
/// called only by the producer prior to calling srsw_flq_push().
static inline uint32_t srsw_flq_next_push(srsw_flq_t &srswq)
{
    uintptr_t pushed_cnt_addr = (uintptr_t) &srswq.PushedCount;
    uint32_t  pushed_cnt      = atomic_read_uint32_aligned(pushed_cnt_addr);
    return (pushed_cnt & (srswq.Capacity - 1));
}

/// @summary Implements a push operation in a SRSW fixed lookaside queue. This
/// must be called only from the producer.
/// @param srswq The queue to update.
static inline void srsw_flq_push(srsw_flq_t &srswq)
{
    uintptr_t pushed_cnt_addr = (uintptr_t) &srswq.PushedCount;
    uint32_t  pushed_cnt      = atomic_read_uint32_aligned(pushed_cnt_addr) + 1;
    atomic_write_uint32_aligned(pushed_cnt_addr, pushed_cnt);
}

/// @summary Gets the index the next pop operation will read from. This must be
/// called only by the consumer prior to popping an item from the queue.
static inline uint32_t srsw_flq_next_pop(srsw_flq_t &srswq)
{
    uintptr_t popped_cnt_addr = (uintptr_t) &srswq.PoppedCount;
    uint32_t  popped_cnt      = atomic_read_uint32_aligned(popped_cnt_addr);
    return (popped_cnt & (srswq.Capacity - 1));
}

/// @summary Implements a pop operation in a SRSW fixed lookaside queue. This must
/// be called only from the consumer against a non-empty queue.
/// @param srswq The queue to update.
static inline void srsw_flq_pop(srsw_flq_t &srswq)
{
    uintptr_t popped_cnt_addr = (uintptr_t) &srswq.PoppedCount;
    uint32_t  popped_cnt      = atomic_read_uint32_aligned(popped_cnt_addr) + 1;
    atomic_write_uint32_aligned(popped_cnt_addr, popped_cnt);
}

/// @summary Flushes a SRSW concurrent queue. This operation should only be
/// performed after coordination between the producer and the consumer; only
/// one should be accessing the queue at the time.
/// @param fifo The queue to flush.
template <typename T, uint32_t N>
static inline void flush_srsw_fifo(srsw_fifo_t<T, N> *fifo)
{
    srsw_flq_clear(fifo->Queue, N);
}

/// @summary Retrieves the number of items 'currently' in the queue.
/// @param fifo The queue to query.
/// @return The number of items in the queue at the instant of the call.
template <typename T, uint32_t N>
static inline size_t srsw_fifo_count(srsw_fifo_t<T, N> *fifo)
{
    return srsw_flq_count(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' empty.
/// @param fifo The queue to query.
/// @return true if the queue contains zero items at the instant of the call.
template <typename T, uint32_t N>
static inline bool srsw_fifo_is_empty(srsw_fifo_t<T, N> *fifo)
{
    return srsw_flq_empty(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' full.
/// @param fifo The queue to query.
/// @return true if the queue is full at the instant of the call.
template <typename T, uint32_t N>
static inline bool srsw_fifo_is_full(srsw_fifo_t<T, N> *fifo)
{
    return srsw_flq_full(fifo->Queue);
}

/// @summary Enqueues an item.
/// @param fifo The destination queue.
/// @param item The item to enqueue. This must be a POD type.
/// @return true if the item was enqueued, or false if the queue is at capacity.
template <typename T, uint32_t N>
static inline bool srsw_fifo_put(srsw_fifo_t<T, N> *fifo, T const &item)
{
    uint32_t count = srsw_flq_count(fifo->Queue) + 1;
    if (count <= N)
    {
        uint32_t    index  = srsw_flq_next_push(fifo->Queue);
        fifo->Store[index] = item;
        COMPILER_MFENCE_WRITE;
        srsw_flq_push(fifo->Queue);
        return true;
    }
    return false;
}

/// @summary Dequeues an item.
/// @param fifo The source queue.
/// @param item On return, the dequeued item is copied here.
/// @return true if an item was dequeued, or false if the queue is empty.
template <typename T, uint32_t N>
static inline bool srsw_fifo_get(srsw_fifo_t<T, N> *fifo, T &item)
{
    uint32_t count = srsw_flq_count(fifo->Queue);
    if (count > 0)
    {
        uint32_t index = srsw_flq_next_pop(fifo->Queue);
        item = fifo->Store[index];
        COMPILER_MFENCE_READ;
        srsw_flq_pop(fifo->Queue);
        return true;
    }
    return false;
}

/// @summary Initializes a free list and optionally allocates initial storage.
/// @param list The free list to initialize.
/// @param capacity The number of nodes to allocate and place on the free list.
/// @return true if the list is initialized successfully.
template <typename T>
static bool create_srmw_freelist(srmw_freelist_t<T> &list, size_t capacity)
{
    pthread_mutex_init(&list.Lock, NULL);
    list.Head        = &list.XXXX;
    list.XXXX.Next   =  NULL;
    if (capacity)
    {   // allocate and push capacity nodes. ideally these would all
        // be allocated as one block, but that causes problems on free.
        // if allocation fails; that's fine. we can try again later.
        for (size_t i = 0; i < capacity; ++i)
        {
            T  *node  = (T*) malloc(sizeof(T));
            if (node != NULL)
            {
                node->Next      = list.Head->Next;
                list.Head->Next = node;
            }
            else break;
        }
    }
    return true;
}

/// @summary Frees storage and resources associated with a free list.
/// @param list The free list to delete.
template <typename T>
static void delete_srmw_freelist(srmw_freelist_t<T> &list)
{
    if (list.Head != NULL)
    {
        T *iter = list.Head->Next;
        while (iter != NULL)
        {
            T *node  = iter;
            iter     = iter->Next;
            free(node);
        }
        pthread_mutex_destroy(&list.Lock);
        list.Head    = NULL;
    }
}

/// @summary Allocates a node from the free list. If the free list is empty, a
/// new node is allocated on the heap and returned.
/// @param list The free list to allocate from.
/// @return The allocated, uninitialized node or NULL.
template <typename T>
static inline T* srmw_freelist_get(srmw_freelist_t<T> &list)
{
    T *node = NULL;

    if (pthread_mutex_lock(&list.Lock) == 0)
    {   // pop the node at the front of the list.
        node = list.Head->Next;
        if (list.Head->Next != NULL)
        {   // after popping node, the list is not empty.
            list.Head->Next  = node->Next;
        }
        pthread_mutex_unlock(&list.Lock);
    }
    if (node == NULL)
    {   // allocate a new node from the heap.
        node  = (T*) malloc(sizeof(T));
    }
    return node;
}

/// @summary Returns a node to the free list.
/// @param list The free list to update.
/// @param node The node to return to the free list.
template <typename T>
static inline void srmw_freelist_put(srmw_freelist_t<T> &list, T *node)
{
    if (pthread_mutex_lock(&list.Lock) == 0)
    {   // push the node at the front of the free list.
        node->Next        = list.Head->Next;
        list.Head->Next   = node;
        pthread_mutex_unlock(&list.Lock);
    }
}

/// @summary Allocate storage for a new unbounded SRMW FIFO with the specified capacity.
/// @param fifo The FIFO to initialize.
/// @param capacity The initial capacity of the FIFO.
/// @return true if the FIFO is initialized.
template <typename T>
static bool create_srmw_fifo(srmw_fifo_t<T> *fifo, size_t capacity)
{
    pthread_mutex_init  (&fifo->HeadLock, 0);
    pthread_mutex_init  (&fifo->TailLock, 0);
    create_srmw_freelist( fifo->FreeList, capacity);
    fifo->XXXX       = srmw_freelist_get(fifo->FreeList);
    fifo->XXXX->Next = NULL;
    fifo->Head       = fifo->XXXX;
    fifo->Tail       = fifo->XXXX;
    return true;
}

/// @summary Frees resources associated with a SRMW FIFO.
/// @param fifo The FIFO to delete.
template <typename T>
static void delete_srmw_fifo(srmw_fifo_t<T> *fifo)
{   // move all items from the queue to the free list.
    while (fifo->Head != NULL)
    {
        T *old_head = fifo->Head;       // never NULL (will point to XXXX)
        T *new_head = fifo->Head->Next; // NULL if the queue is now empty
        srmw_freelist_put(fifo->FreeList , old_head);
        fifo->Head  = new_head;
    }
    delete_srmw_freelist ( fifo->FreeList);
    pthread_mutex_destroy(&fifo->TailLock);
    pthread_mutex_destroy(&fifo->HeadLock);
    fifo->Head   = NULL;
    fifo->Tail   = NULL;
    fifo->XXXX   = NULL;
}

/// @summary Retrieves the next item from the FIFO.
/// @param fifo The FIFO to update.
/// @param item On return, the contents of the next item are copied here.
/// @return true if an item was retrieved.
template <typename T>
static bool srmw_fifo_get(srmw_fifo_t<T> *fifo, T &item)
{
    T  *old_head = NULL;
    T  *new_head = NULL;
    bool  result = false;
    if (pthread_mutex_lock(&fifo->HeadLock) == 0)
    {
        old_head = fifo->Head;       // never NULL (will point to XXXX)
        new_head = fifo->Head->Next; // NULL if the queue is now empty
        if (new_head  != NULL)
        {   // the queue was not empty (we popped an item)
            result     = true;
            fifo->Head = new_head;
        }
        pthread_mutex_unlock(&fifo->HeadLock);
    }
    if (result)
    {   // if we popped an item, return its node to the free list.
        item = *new_head; // copy the node contents for the caller
        item.Next = NULL; // not necessary, but may as well
        srmw_freelist_put(fifo->FreeList, old_head);
    }
    return result;
}

/// @summary Enqueues an item in the FIFO.
/// @param fifo The FIFO to update.
/// @param item The item to enqueue.
/// @return true if the item was appended to the queue.
template <typename T>
static bool srmw_fifo_put(srmw_fifo_t<T> *fifo, T const &item)
{
    T  *node   = srmw_freelist_get(fifo->FreeList);
    if (node  == NULL) return false;
    *node      = item;
    node->Next = NULL;
    if (pthread_mutex_lock(&fifo->TailLock) == 0)
    {
        fifo->Tail->Next = node;
        fifo->Tail = node;
        pthread_mutex_unlock(&fifo->TailLock);
    }
    return true;
}

/// @summary Create a new I/O buffer allocator with the specified minimum buffer
/// and allocation sizes. The total buffer size and allocation size may be
/// somewhat larger than what is requested in order to meet alignment requirements.
/// @param alloc The I/O buffer allocator to initialize.
/// @param total_size The total number of bytes to allocate. This size is
/// rounded up to the nearest even multiple of the sub-allocation size. It is
/// best to keep the total buffer size as small as is reasonable for the application
/// workload, as this memory may be allocated from the non-paged pool.
/// @param alloc_size The sub-allocation size, in bytes. This is the size of a
/// single buffer that can be returned to the application. This size is rounded
/// up to the nearest even multiple of the largest disk sector size.
/// @return true if the allocator was initialized. Check alloc.TotalSize and
/// alloc.AllocSize to determine the values selected by the system.
static bool create_iobuf_allocator(iobuf_alloc_t &alloc, size_t total_size, size_t alloc_size)
{
    // round the allocation size up to an even multiple of the page size.
    // round the total size up to an even multiple of the allocation size.
    size_t page_size = size_t(sysconf(_SC_PAGESIZE));
    alloc_size       = align_up(alloc_size, page_size);
    total_size       = align_up(total_size, alloc_size);
    size_t nallocs   = total_size / alloc_size;

    // reserve and commit the entire region, and then pin it in physical memory.
    // this prevents the buffers from being paged out during normal execution.
    void  *baseaddr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (baseaddr == NULL)
    {   // the requested amount of memory could not be allocated.
        return false;
    }

    madvise(baseaddr, total_size, MADV_HUGEPAGE);

    void **freelist = (void**) malloc(nallocs * sizeof(void*));
    if (freelist == NULL)
    {   // the requested memory could not be allocated.
        munmap(baseaddr, total_size);
        return false;
    }

    // at this point, everything that could have failed has succeeded.
    // set the fields of the allocator and initialize the free list.
    alloc.TotalSize    = total_size;
    alloc.PageSize     = page_size;
    alloc.AllocSize    = alloc_size;
    alloc.BaseAddress  = baseaddr;
    alloc.FreeCount    = nallocs;
    alloc.FreeList     = freelist;
    uint8_t *buf_it    = (uint8_t*) baseaddr;
    for (size_t i = 0; i < nallocs; ++i)
    {
        freelist[i]    = buf_it;
        buf_it        += alloc_size;
    }
    return true;
}

/// @summary Delete an I/O buffer allocator. All memory is freed, regardless
/// of whether any I/O buffers are in use by the application.
/// @param alloc The I/O buffer allocator to delete.
static void delete_iobuf_allocator(iobuf_alloc_t &alloc)
{
    if (alloc.FreeList    != NULL) free(alloc.FreeList);
    if (alloc.BaseAddress != NULL) munmap(alloc.BaseAddress, alloc.TotalSize);
    alloc.BaseAddress      = NULL;
    alloc.FreeCount        = 0;
    alloc.FreeList         = NULL;
}

/// @summary Returns all I/O buffers to the free list of the allocator, regardless
/// of whether any I/O buffers are in use by the application.
/// @param alloc The I/O buffer allocator to flush.
static void flush_iobuf_allocator(iobuf_alloc_t &alloc)
{
    size_t const nallocs = alloc.TotalSize / alloc.AllocSize;
    size_t const allocsz = alloc.AllocSize;
    uint8_t       *bufit = (uint8_t*) alloc.BaseAddress;
    void         **freel = alloc.FreeList;
    for (size_t i = 0; i < nallocs; ++i)
    {
        freel[i]  = bufit;
        bufit    += allocsz;
    }
    alloc.FreeCount = nallocs;
}

/// @summary Retrieves an I/O buffer from the pool.
/// @param alloc The I/O buffer allocator to query.
/// @return A pointer to the I/O buffer, or NULL if no buffers are available.
static inline void* iobuf_get(iobuf_alloc_t &alloc)
{
    if (alloc.FreeCount > 0)
    {   // return the next buffer from the free list,
        // which is typically the most recently used buffer.
        return alloc.FreeList[--alloc.FreeCount];
    }
    else return NULL; // no buffers available for use.
}

/// @summary Returns an I/O buffer to the pool.
/// @param alloc The I/O buffer allocator that owns the buffer.
/// @param iobuf The address of the buffer returned by iobuf_get().
static inline void iobuf_put(iobuf_alloc_t &alloc, void *iobuf)
{
    assert(iobuf != NULL);
    alloc.FreeList[alloc.FreeCount++] = iobuf;
}

/// @summary Calaculate the number of bytes currently unused.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of bytes currently available for use by the application.
static inline size_t iobuf_bytes_free(iobuf_alloc_t const &alloc)
{
    return (alloc.AllocSize * alloc.FreeCount);
}

/// @summary Calaculate the number of bytes currently allocated.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of bytes currently in-use by the application.
static inline size_t iobuf_bytes_used(iobuf_alloc_t const &alloc)
{
    return  alloc.TotalSize - (alloc.AllocSize * alloc.FreeCount);
}

/// @summary Calculate the number of buffers currently allocated.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of buffers currently in-use by the application.
static inline size_t iobuf_buffers_used(iobuf_alloc_t const &alloc)
{
    size_t const nallocs = alloc.TotalSize / alloc.AllocSize;
    size_t const nunused = alloc.FreeCount;
    return (nallocs - nunused);
}

/// @summary Perform a comparison between two elements in an I/O operation priority queue.
/// @param pq The I/O operation priority queue.
/// @param priority The priority of the item being inserted.
/// @param idx The zero-based index of the item in the queue to compare against.
/// @return -1 if item a should appear before item b, +1 if item a should appear after item b.
static inline int io_opq_cmp_put(vfs_io_opq_t const *pq, uint32_t priority, int32_t idx)
{   // when inserting, the new item is always ordered after the existing item
    // if the priority values of the two items are the same.
    uint32_t const p_a  = priority;
    uint32_t const p_b  = pq->Priority[idx];
    return ((p_a < p_b) ? -1 : +1);
}

/// @summary Perform a comparison between two elements in an I/O operation priority queue.
/// @param pq The I/O operation priority queue.
/// @param a The zero-based index of the first element.
/// @param b The zero-based index of the second element.
/// @return -1 if item a should appear before item b, +1 if item a should appear after item b.
static inline int io_opq_cmp_get(vfs_io_opq_t const *pq, int32_t a, int32_t b)
{   // first order by priority. if priority is equal, the operations should
    // appear in the order they were inserted into the queue.
    uint32_t const p_a  = pq->Priority[a];
    uint32_t const p_b  = pq->Priority[b];
    uint32_t const i_a  = pq->InsertId[a];
    uint32_t const i_b  = pq->InsertId[b];
    if (p_a < p_b) return -1;
    if (p_a > p_b) return +1;
    if (i_a < i_b) return -1;
    else           return +1; // i_a > i_b; i_a can never equal i_b.
}

/// @summary Resets a I/O operation priority queue to empty.
/// @param pq The priority queue to clear.
static void io_opq_clear(vfs_io_opq_t *pq)
{
    pq->Count = 0;
}

/// @summary Attempts to insert an I/O operation in the priority queue.
/// @param pq The I/O operation priority queue to update.
/// @param priority The priority value associated with the item being inserted.
/// @return The AIO request to populate, or NULL if the queue is full.
static aio_req_t* io_opq_put(vfs_io_opq_t *pq, uint32_t priority)
{   // note that the InsertionId counter is enough to represent the transfer
    // of 128TB of data (at 32KB/request) which is enough for ~39 hours of
    // constant streaming at a rate of 980MB/s before the counter wraps around.
    if (pq->Count < AIO_MAX_ACTIVE)
    {   // there's room in the queue for this operation.
        int32_t pos = pq->Count++;
        int32_t idx =(pos - 1) / 2;
        while  (pos > 0 && io_opq_cmp_put(pq, priority, idx) < 0)
        {
            pq->Priority[pos] = pq->Priority[idx];
            pq->InsertId[pos] = pq->InsertId[idx];
            pq->Request [pos] = pq->Request [idx];
            pos = idx;
            idx =(idx - 1) / 2;
        }
        pq->Priority[pos] = priority;
        pq->InsertId[pos] = pq->InsertionId++;
        return &pq->Request[pos];
    }
    else return NULL;
}

/// @summary Retrieves the highest priority pending AIO operation without removing it from the queue.
/// @param pq The I/O operation priority queue to update.
/// @param request On return, the AIO request is copied to this location.
/// @return true if an operation was retrieved for false if the queue is empty.
static inline bool io_opq_top(vfs_io_opq_t *pq, aio_req_t &request)
{
    if (pq->Count > 0)
    {   // the highest-priority operation is located at index 0.
        request = pq->Request[0];
        return true;
    }
    else return false;
}

/// @summary Retrieves the highest priority pending AIO operation.
/// @param pq The I/O operation priority queue to update.
/// @param request On return, the AIO request is copied to this location.
/// @return true if an operation was retrieved, or false if the queue is empty.
static bool io_opq_get(vfs_io_opq_t *pq, aio_req_t &request)
{
    if (pq->Count > 0)
    {   // the highest-priority operation is located at index 0.
        request = pq->Request[0];

        // swap the last item into the position vacated by the first item.
        int32_t       n  = pq->Count - 1;
        pq->Priority[0]  = pq->Priority[n];
        pq->InsertId[0]  = pq->InsertId[n];
        pq->Request [0]  = pq->Request [n];
        pq->Count        = n;

        // now re-heapify and restore the heap order.
        int32_t pos = 0;
        for ( ; ; )
        {
            int32_t l = (2 * pos) + 1; // left child
            int32_t r = (2 * pos) + 2; // right child
            int32_t m; // the child with the lowest frequency.

            // determine the child node with the lowest frequency.
            if  (l >= n) break; // node at pos has no children.
            if  (r >= n) m = l; // node at pos has no right child.
            else m  = io_opq_cmp_get(pq, l, r) < 0 ? l : r;

            // now compare the node at pos with the highest priority child, m.
            if (io_opq_cmp_get(pq, pos, m) < 0)
            {   // children have lower priority than parent. order restored.
                break;
            }

            // swap the parent with the largest child.
            uint32_t  temp_p     = pq->Priority[pos];
            uint32_t  temp_i     = pq->InsertId[pos];
            aio_req_t temp_r     = pq->Request [pos];
            pq->Priority[pos]    = pq->Priority[m];
            pq->InsertId[pos]    = pq->InsertId[m];
            pq->Request [pos]    = pq->Request [m];
            pq->Priority[m]      = temp_p;
            pq->InsertId[m]      = temp_i;
            pq->Request [m]      = temp_r;
            pos = m;
        }
        return true;
    }
    else return false;
}

/// @summary Resets a file priority queue to empty.
/// @param pq The priority queue to clear.
static void io_fpq_clear(vfs_io_fpq_t *pq)
{
    pq->Count = 0;
}

/// @summary Attempts to insert a file into the file priority queue.
/// @param pq The priority queue to update.
/// @param priority The priority value associated with the item being inserted.
/// @param index The zero-based index of the file record being inserted.
/// @return true if the item was inserted in the queue, or false if the queue is full.
static bool io_fpq_put(vfs_io_fpq_t *pq, uint32_t priority, uint16_t index)
{
    if (pq->Count < MAX_OPEN_FILES)
    {   // there's room in the queue for this operation.
        int32_t pos = pq->Count++;
        int32_t idx =(pos - 1) / 2;
        while  (pos > 0 && priority < pq->Priority[idx])
        {
            pq->Priority[pos] = pq->Priority[idx];
            pq->RecIndex[pos] = pq->RecIndex[idx];
            pos = idx;
            idx =(idx - 1) / 2;
        }
        pq->Priority[pos] = priority;
        pq->RecIndex[pos] = index;
        return true;
    }
    else return false;
}

/// @summary Retrieves the highest priority active file.
/// @param pq The priority queue to update.
/// @param index On return, this location is updated with the file record index.
/// @param priority On return, this location is updated with the file priority.
/// @return true if a file was retrieved, or false if the queue is empty.
static bool io_fpq_get(vfs_io_fpq_t *pq, uint16_t &index, uint32_t &priority)
{
    if (pq->Count > 0)
    {   // the highest-priority operation is located at index 0.
        priority  = pq->Priority[0];
        index     = pq->RecIndex[0];

        // swap the last item into the position vacated by the first item.
        int32_t       n  = pq->Count - 1;
        pq->Priority[0]  = pq->Priority[n];
        pq->RecIndex[0]  = pq->RecIndex[n];
        pq->Count        = n;

        // now re-heapify and restore the heap order.
        int32_t pos = 0;
        for ( ; ; )
        {
            int32_t l = (2 * pos) + 1; // left child
            int32_t r = (2 * pos) + 2; // right child
            int32_t m; // the child with the lowest frequency.

            // determine the child node with the lowest frequency.
            if  (l >= n) break; // node at pos has no children.
            if  (r >= n) m = l; // node at pos has no right child.
            else m  = pq->Priority[l] < pq->Priority[r] ? l : r;

            // now compare the node at pos with the highest priority child, m.
            if (pq->Priority[pos] < pq->Priority[m] < 0)
            {   // children have lower priority than parent. order restored.
                break;
            }

            // swap the parent with the largest child.
            uint32_t temp_p   = pq->Priority[pos];
            uint16_t temp_i   = pq->RecIndex[pos];
            pq->Priority[pos] = pq->Priority[m];
            pq->RecIndex[pos] = pq->RecIndex[m];
            pq->Priority[m]   = temp_p;
            pq->RecIndex[m]   = temp_i;
            pos = m;
        }
        return true;
    }
    else return false;
}

/// @summary Builds a list of record indices for all files with a given mode.
/// @param list The output record index array of MAX_OPEN_FILES items.
/// @param vfs The VFS driver state maintaining the input file list.
/// @param mode One of vfs_mode_e indicating the filter mode.
/// @return The number of files in the output list.
static size_t map_files_by_mode(uint16_t *list, vfs_state_t const *vfs, int32_t mode)
{
    size_t         nfilter = 0;
    uint16_t const nactive = uint16_t(vfs->ActiveCount);
    for (uint16_t i = 0; i < nactive; ++i)
    {
        if (vfs->FileMode[i] == mode)
            list[nfilter++]   = i;
    }
    return nfilter;
}

/// @summary Builds a file priority queue for a file index list.
/// @param pq The priority queue to populate. Existing contents are overwritten.
/// @param vfs The VFS driver state maintaining the active file information.
/// @param list The list of input file record indices.
/// @param n The number of elements in the file record index list.
static void build_file_queue(vfs_io_fpq_t *pq, vfs_state_t const *vfs, uint16_t *list, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        io_fpq_put(pq, vfs->Priority[list[i]], list[i]);
    }
}

/// @summary Synchronously opens a file, creates an eventfd, and retrieves
/// various information about the file.
/// @param path The path of the file to open.
/// @param flags The set of flags to pass to open(). Usually, this value will
/// be O_RDONLY | O_LARGEFILE | O_DIRECT, but it may be any combination of values
/// supported by the open() call.
/// @param fd On return, this value is set to the raw file descriptor, or -1 if
/// an error occurred. Close the file using the close() function.
/// @param efd On return, this value is set to an eventfd file descriptor, or -1
/// if an error occurred. Close the file descriptor using the close() function.
/// @param file_size On return, this value is set to the current size of the file, in bytes.
/// @param sector_size On return, this value is set to the size of the physical disk sector, in bytes.
/// @return true if all operations were successful.
static bool open_file_raw(char const *path, int flags, int &fd, int &efd, int64_t &file_size, size_t &sector_size)
{   // typical read flags are O_RDONLY | O_LARGEFILE | O_DIRECT
    struct stat  st;
    int evtdes = -1;
    int fildes = -1;

    // create an eventfd that can be used with epoll.
    // TODO: investigate the possible flags values.
    if ((evtdes = eventfd(0, 0)) == -1)
    {   // unable to create the eventfd. check errno to find out why.
        goto error_cleanup;
    }

    // open the file.
    if ((fildes = open(path, flags)) == -1)
    {   // unable to open the file. check errno to find out why.
        goto error_cleanup;
    }

    // retrieve the physical block size for the disk containing the file.
    // this will also retrieve the current size of the file, in bytes.
    if (fstat(fildes, &st) < 0)
    {   // unable to retrieve file stats; fail.
        goto error_cleanup;
    }

    // the file has been opened, and all resources created successfully.
    fd          = fildes;
    efd         = evtdes;
    file_size   = st.st_size;
    sector_size = st.st_blksize;
    return true;

error_cleanup:
    if (evtdes != -1) close(evtdes);
    if (fildes != -1) close(fildes);
    fd          = -1;
    efd         = -1;
    file_size   =  0;
    sector_size =  0;
    return false;
}

/// @summary Closes the file descriptors associated with a file.
/// @param fd The raw file descriptor of the underlying file. On return, set to -1.
/// @param efd The eventfd file descriptor associated with the file. On return, set to -1.
static void close_file_raw(int &fd, int &efd)
{
    if (efd != -1) close(efd);
    if (fd  != -1) close(fd);
    fd  = -1;
    efd = -1;
}

/// @summary Allocates an iocb instance from the free list.
/// @param aio The AIO driver state managing the free list.
/// @return The next available iocb structure.
static inline struct iocb* iocb_get(aio_state_t *aio)
{
    assert(aio->IOCBFreeCount > 0);
    return aio->IOCBFree[--aio->IOCBFreeCount];
}

/// @summary Returns an iocb instance to the free list.
/// @param aio The AIO driver state managing the free list.
/// @param iocb The IOCB to return to the free list.
static inline void iocb_put(aio_state_t *aio, struct iocb *iocb)
{
    assert(aio->IOCBFreeCount < AIO_MAX_ACTIVE);
    aio->IOCBFree[aio->IOCBFreeCount++] = iocb;
}

/// @summary Builds a read operation IOCB and submits it to kernel AIO.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the read operation.
/// @param error On return, this location stores the error return value.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
static int aio_submit_read(aio_state_t *aio, aio_req_t const &req, int &error)
{
    struct iocb *iocb    = iocb_get(aio);    // allocate from the free list
    iocb->data           = (void*)req.AFID;  // for sanity checking on completion
    iocb->aio_lio_opcode = IO_CMD_PREAD;     // we're reading from the file
    iocb->aio_fildes     = req.Fildes;       // the file descriptor to read from
    iocb->u.c.buf        = req.DataBuffer;   // the buffer to read into
    iocb->u.c.nbytes     = req.DataAmount;   // the maximum number of bytes to read
    iocb->u.c.offset     = req.FileOffset;   // the absolute byte offset of the first byte to read
    int res  = io_submit(aio->AIOContext, 1, &iocb);
    if (res >= 0)
    {   // the operation was queued by kernel AIO. append to the active list.
        size_t index = aio->ActiveCount++;
        aio->AAIOList[index] = req;
        aio->AAIOList[index].ATimeNanos = nanotime();
        aio->IOCBList[index] = iocb;
        error = 0;
        return (0);
    }
    else
    {   // the operation was rejected by kernel AIO. return the error.
        error = -res;
        return (-1);
    }
}

/// @summary Builds a write operation IOCB and submits it to kernel AIO.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the write operation.
/// @param error On return, this location stores the error return value.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
static int aio_submit_write(aio_state_t *aio, aio_req_t const &req, int &error)
{
    struct iocb *iocb    = iocb_get(aio);    // allocate from the free list
    iocb->data           = (void*)req.AFID;  // for sanity checking on completion
    iocb->aio_lio_opcode = IO_CMD_PWRITE;    // we're writing to the the file
    iocb->aio_fildes     = req.Fildes;       // the file descriptor to write to
    iocb->u.c.buf        = req.DataBuffer;   // the buffer to read from
    iocb->u.c.nbytes     = req.DataAmount;   // the number of bytes to write
    iocb->u.c.offset     = req.FileOffset;   // the absolute byte offset of the write location
    int res  = io_submit(aio->AIOContext, 1, &iocb);
    if (res >= 0)
    {   // the operation was queued by kernel AIO. append to the active list.
        size_t index = aio->ActiveCount++;
        aio->AAIOList[index] = req;
        aio->AAIOList[index].ATimeNanos = nanotime();
        aio->IOCBList[index] = iocb;
        error = 0;
        return (0);
    }
    else
    {   // the operation was rejected by kernel AIO. return the error.
        error = -res;
        return (-1);
    }
}

/// @summary Builds an fsync operation IOCB and submits it to kernel AIO.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the flush operation.
/// @param error On return, this location stores the error return value.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
static int aio_submit_fsync(aio_state_t *aio, aio_req_t const &req, int &error)
{   // fsync flushes file buffers and file metadata, always
    struct iocb *iocb    = iocb_get(aio);    // allocate from the free list
    iocb->data           = (void*)req.AFID;  // for sanity checking on completion
    iocb->aio_lio_opcode = IO_CMD_FSYNC;     // we're flushing the file buffers
    iocb->aio_fildes     = req.Fildes;       // the file descriptor to write to
    iocb->u.c.buf        = NULL;             // N/A
    iocb->u.c.nbytes     = 0;                // N/A
    iocb->u.c.offset     = 0;                // N/A
    int res  = io_submit(aio->AIOContext, 1, &iocb);
    if (res >= 0)
    {   // the operation was queued by kernel AIO. append to the active list.
        size_t index = aio->ActiveCount++;
        aio->AAIOList[index] = req;
        aio->AAIOList[index].ATimeNanos = nanotime();
        aio->IOCBList[index] = iocb;
        error = 0;
        return (0);
    }
    else
    {   // the operation was rejected by kernel AIO. return the error.
        error = -res;
        return (-1);
    }
}

/// @summary Builds an fdatasync operation IOCB and submits it to kernel AIO.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the flush operation.
/// @param error On return, this location stores the error return value.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
static int aio_submit_fdsync(aio_state_t *aio, aio_req_t const &req, int &error)
{   // fdatasync flushes file buffers, and only flushes metadata if necessary.
    struct iocb *iocb    = iocb_get(aio);    // allocate from the free list
    iocb->data           = (void*)req.AFID;  // for sanity checking on completion
    iocb->aio_lio_opcode = IO_CMD_FDSYNC;    // we're flushing the file buffers
    iocb->aio_fildes     = req.Fildes;       // the file descriptor to write to
    iocb->u.c.buf        = NULL;             // N/A
    iocb->u.c.nbytes     = 0;                // N/A
    iocb->u.c.offset     = 0;                // N/A
    int res  = io_submit(aio->AIOContext, 1, &iocb);
    if (res >= 0)
    {   // the operation was queued by kernel AIO. append to the active list.
        size_t index = aio->ActiveCount++;
        aio->AAIOList[index] = req;
        aio->AAIOList[index].ATimeNanos = nanotime();
        aio->IOCBList[index] = iocb;
        error = 0;
        return (0);
    }
    else
    {   // the operation was rejected by kernel AIO. return the error.
        error = -res;
        return (-1);
    }
}

/// @summary Synchronously processes a file close operation.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the close operation.
/// @return Zero if the result was successfully submitted, or -1 if the result queue is full.
static int aio_process_close(aio_state_t *aio, aio_req_t const &req)
{   // close the file descriptors associated with the file.
    if (req.Eventfd != -1) close(req.Eventfd);
    if (req.Fildes  != -1) close(req.Fildes);

    // generate the completion result and push it to the queue.
    aio_res_t res = {
        req.Fildes,
        req.Eventfd,
        0,            /* OSError    */
        0,            /* DataAmount */
        0,            /* FileOffset */
        NULL,         /* DataBuffer */
        req.QTimeNanos,
        nanotime(),   /* CTimeNanos */
        req.AFID,
        req.Type,
        0             /* Reserved   */
    };
    return srsw_fifo_put(&aio->CloseResults, res) ? 0 : -1;
}

/// @summary Implements the main loop of the AIO driver using a polling mechanism.
/// @param aio The AIO driver state to update.
/// @param timeout The timeout value indicating the amount of time to wait, or
/// NULL to block indefinitely. Note that aio_poll() just calls aio_tick() with
/// a timeout of zero, which will return immediately if no events are available.
static void aio_tick(aio_state_t *aio, struct timespec *timeout)
{   // poll kernel AIO for any completed events, and process them first.
    io_event events [AIO_MAX_ACTIVE];
    int nevents = io_getevents(aio->AIOContext, 1, AIO_MAX_ACTIVE, events, timeout);
    if (nevents > 0)
    {   // kernel AIO reported one or more events are ready.
        for (int i = 0; i < nevents; ++i)
        {
            io_event     &evt = events[i];
            struct iocb *iocb = evt.obj;
            size_t        idx = 0;
            bool        found = false;

            // search for the current index of this operation by
            // locating the IOCB pointer in the active list. this
            // is necessary because the active list gets reordered.
            size_t const nlive = aio->ActiveCount;
            struct iocb **list = aio->IOCBList;
            for (size_t op = 0 ; op < nlive; ++op)
            {
                if (list[op] == iocb)
                {   // found the item we're looking for.
                    found = true;
                    idx   = op;
                    break;
                }
            }
            if (found)
            {
                aio_res_t res;                      // response, populated below
                aio_req_t req = aio->AAIOList[idx]; // make a copy of the request

                // swap the last active request into this slot.
                aio->AAIOList[idx] = aio->AAIOList[nlive-1];
                aio->IOCBList[idx] = aio->IOCBList[nlive-1];
                aio->ActiveCount--;

                // populate the result descriptor.
                res.Fildes      = req.Fildes;
                res.Eventfd     = req.Eventfd;
                res.OSError     = evt.res <  0 ? -evt.res : 0;
                res.DataAmount  = evt.res >= 0 ?  evt.res : 0;
                res.FileOffset  = req.FileOffset;
                res.DataBuffer  = req.DataBuffer;
                res.QTimeNanos  = req.QTimeNanos;
                res.CTimeNanos  = nanotime();
                res.AFID        = req.AFID;
                res.Type        = req.Type;
                res.Reserved    = req.Reserved;

                // create the result and enqueue it in the appropriate queue.
                // AIO_COMMAND_CLOSE is always processed synchronously and
                // will never be completed through io_getevents().
                switch (req.Command)
                {
                    case AIO_COMMAND_READ:
                        srsw_fifo_put(&aio->ReadResults , res);
                        break;
                    case AIO_COMMAND_WRITE:
                        srsw_fifo_put(&aio->WriteResults, res);
                        break;
                    case AIO_COMMAND_FLUSH:
                        srsw_fifo_put(&aio->FlushResults, res);
                        break;
                    default:
                        break;
                }

                // return the IOCB to the free list.
                iocb_put(aio, iocb);
            }
        }
    }

    // now dequeue and submit as many AIO requests as we can.
    int  error = 0;
    int result = 0;
    while (aio->ActiveCount < AIO_MAX_ACTIVE)
    {   // grab the next request from the queue.
        aio_req_t req;
        if (srsw_fifo_get(&aio->RequestQueue, req) == false)
        {   // there are no more requests waiting in the queue.
            break;
        }

        // update the activation time for the request.
        req.ATimeNanos = nanotime();

        // dispatch the request based on its type.
        switch (req.Command)
        {
            case AIO_COMMAND_READ:
                result = aio_submit_read (aio, req, error);
                break;
            case AIO_COMMAND_WRITE:
                result = aio_submit_write(aio, req, error);
                break;
            case AIO_COMMAND_FLUSH:
                // TODO: aio_submit_fdsync might be more appropriate?
                result = aio_submit_fsync(aio, req, error);
                break;
            case AIO_COMMAND_CLOSE:
                result = aio_process_close(aio, req);
                break;
            default:
                result = -1;
                error  = EINVAL;
                break;
        }
    }
}

/// @summary Implements the main loop of the AIO driver.
/// @param aio The AIO driver state to update.
static inline void aio_poll(aio_state_t *aio)
{   // configure a zero timeout so we won't block.
    struct timespec timeout;
    timeout.tv_sec  = 0;
    timeout.tv_nsec = 0;
    aio_tick(aio , &timeout);
}

/// @summary Allocates a new AIO context and initializes the AIO state.
/// @param aio The AIO state to allocate and initialize.
/// @return 0 if the operation completed successfully; otherwise, the errno value.
static int create_aio_state(aio_state_t *aio)
{
    io_context_t  io_ctx = 0;
    int result  = io_setup(AIO_MAX_ACTIVE, &io_ctx);
    if (result != 0)
    {   // unable to create the AIO context; everything else fails.
        return -result;
    }
    // setup the iocb free list. all items are initially available.
    for (size_t i = 0, n =  AIO_MAX_ACTIVE; i < n; ++i)
    {
        aio->IOCBFree[i] = &aio->IOCBPool[i];
    }
    aio->AIOContext    = io_ctx;
    aio->ActiveCount   = 0;
    aio->IOCBFreeCount = AIO_MAX_ACTIVE;
    flush_srsw_fifo(&aio->RequestQueue);
    flush_srsw_fifo(&aio->ReadResults );
    flush_srsw_fifo(&aio->WriteResults);
    flush_srsw_fifo(&aio->CloseResults);
    return 0;
}

/// @summary Cancels all pending AIO operations and frees associated resources.
/// This call may block until pending operations have completed.
/// @param aio The AIO state to delete.
static void delete_aio_state(aio_state_t *aio)
{
    if (aio->AIOContext != 0)
    {
        io_destroy(aio->AIOContext);
    }
    aio->AIOContext    = 0;
    aio->ActiveCount   = 0;
    aio->IOCBFreeCount = 0;
    flush_srsw_fifo(&aio->RequestQueue);
    flush_srsw_fifo(&aio->ReadResults );
    flush_srsw_fifo(&aio->WriteResults);
    flush_srsw_fifo(&aio->CloseResults);
}

/// @summary Submits a file read request to the AIO pending operation queue.
/// @param aio The AIO context that will be processing the I/O.
/// @param fd The file descriptor of the file to read from.
/// @param efd The eventfd file descriptor to associate with the request, or -1.
/// @param offset The absolute byte offset within the file at which to being reading data.
/// The caller is responsible for ensuring that alignment requirements are met.
/// @param amount The maximum number of bytes to read. The caller is responsible for
/// ensuring that this size meets any alignment requirements (sector size multiple, etc.)
/// @param iobuf The buffer in which to place data read from the file. The caller is
/// responsible for ensuring that this buffer meets any alignment requirements.
/// @param afid The application-defined file ID to pass along to the result.
/// @param type The file type, one of file_type_e, to pass along to the result.
/// @return true if the request was accepted.
static bool aio_request_read(aio_state_t *aio, int fd, int efd, int64_t offset, uint32_t amount, void *iobuf, intptr_t afid, int32_t type)
{
    aio_req_t req;
    req.Command    = AIO_COMMAND_READ;
    req.Fildes     = fd;
    req.Eventfd    = efd;
    req.DataAmount = amount;
    req.FileOffset = offset;
    req.DataBuffer = iobuf;
    req.QTimeNanos = nanotime();
    req.ATimeNanos = 0;
    req.AFID       = afid;
    req.Type       = type;
    req.Reserved   = 0;
    return srsw_fifo_put(&aio->RequestQueue, req);
}

/// @summary Submits a file write request to the AIO pending operation queue.
/// @param aio The AIO context that will be processing the I/O.
/// @param fd The file descriptor of the file to write to.
/// @param efd The eventfd file descriptor to associate with the request, or -1.
/// @param offset The absolute byte offset within the file at which to being writing data.
/// The caller is responsible for ensuring that alignment requirements are met.
/// @param amount The number of bytes to write. The caller is responsible for ensuring
/// that this size meets any alignment requirements (sector size multiple, etc.)
/// @param iobuf The buffer containing the data to write to the file. The caller is
/// responsible for ensuring that this buffer meets any alignment requirements.
/// @param afid The application-defined file ID to pass along to the result.
/// @param type The file type, one of file_type_e, to pass along to the result.
/// @return true if the request was accepted.
static bool aio_request_write(aio_state_t *aio, int fd, int efd, int64_t offset, uint32_t amount, void *iobuf, intptr_t afid, int32_t type)
{
    aio_req_t req;
    req.Command    = AIO_COMMAND_WRITE;
    req.Fildes     = fd;
    req.Eventfd    = efd;
    req.DataAmount = amount;
    req.FileOffset = offset;
    req.DataBuffer = iobuf;
    req.QTimeNanos = nanotime();
    req.ATimeNanos = 0;
    req.AFID       = afid;
    req.Type       = type;
    req.Reserved   = 0;
    return srsw_fifo_put(&aio->RequestQueue, req);
}

/// @summary Submits a file flush request to the AIO pending operation queue.
/// @param aio The AIO context that will be processing the I/O.
/// @param fd The file descriptor of the file to flush.
/// @param efd The eventfd file descriptor to associate with the request, or -1.
/// @param afid The application-defined file ID to pass along to the result.
/// @param type The file type, one of file_type_e, to pass along to the result.
/// @return true if the request was accepted.
static bool aio_request_flush(aio_state_t *aio, int fd, int efd, intptr_t afid, int32_t type)
{
    aio_req_t req;
    req.Command    = AIO_COMMAND_FLUSH;
    req.Fildes     = fd;
    req.Eventfd    = efd;
    req.DataAmount = 0;
    req.FileOffset = 0;
    req.DataBuffer = 0;
    req.QTimeNanos = nanotime();
    req.ATimeNanos = 0;
    req.AFID       = afid;
    req.Type       = type;
    req.Reserved   = 0;
    return srsw_fifo_put(&aio->RequestQueue, req);
}

/// @summary Submits a file close request to the AIO pending operation queue.
/// @param aio The AIO context that will be processing the I/O.
/// @param fd The file descriptor of the file to close.
/// @param efd The eventfd file descriptor to associate with the request, or -1.
/// @param afid The application-defined file ID to pass along to the result.
/// @param type The file type, one of file_type_e, to pass along to the result.
/// @return true if the request was accepted.
static bool aio_request_close(aio_state_t *aio, int fd, int efd, intptr_t afid, int32_t type)
{
    aio_req_t req;
    req.Command    = AIO_COMMAND_CLOSE;
    req.Fildes     = fd;
    req.Eventfd    = efd;
    req.DataAmount = 0;
    req.FileOffset = 0;
    req.DataBuffer = 0;
    req.QTimeNanos = nanotime();
    req.ATimeNanos = 0;
    req.AFID       = afid;
    req.Type       = type;
    req.Reserved   = 0;
    return srsw_fifo_put(&aio->RequestQueue, req);
}

/// @summary Determine whether a path references a file within an archive, (and
/// if so, which one) or whether it references a native file. Open the file if
/// necessary, and return basic file information to the caller. This function
/// should only be used for read-only files, files cannot be written in an archive.
/// @param path The NULL-terminated UTF-8 path of the file to resolve.
/// @param fd On return, stores the file descriptor of the archive or native file.
/// @param efd On return, stores the eventfd descriptor of the archive or native file.
/// @param lsize On return, stores the logical size of the file, in bytes. This is the
/// size of the file after all size-changing transformations (decompression) is performed.
/// For native files, the logical and physical file size are the same.
/// @param psize On return, stores the physical size of the file, in bytes. This is the
/// corresponds to the number of bytes that must be read to read all file data on disk.
/// For native files, the logical  and physical file size are the same.
/// @param offset On return, stores the byte offset of the first byte of the file.
/// @param sector_size On return, stores the physical sector size of the disk.
/// @return true if the file could be resolved.
static bool vfs_resolve_file(char const *path, int &fd, int &efd, int64_t &lsize, int64_t &psize, int64_t &offset, size_t &sector_size)
{
    // TODO: determine whether this path references a file contained within an archive.
    // for now, we only handle native file paths, which may be absolute or relative.
    bool native_path = true;
    if  (native_path)
    {
        int flags = O_RDONLY | O_LARGEFILE | O_DIRECT;
        if (open_file_raw(path, flags, fd, efd, psize, sector_size))
        {   // native files always begin at the first byte.
            // logical and physical size are the same.
            lsize  = psize;
            offset = 0;
            return true;
        }
        else
        {   // unable to open the file, so fail immediately.
            fd = efd = -1; lsize = psize = offset = sector_size = 0;
            return false;
        }
    }
}

/// @summary Processes queued file open operations.
/// @param vfs The VFS driver state.
static void vfs_process_opens(vfs_state_t *vfs)
{   // TODO: process pending explicit file open requests.
}

/// @summary Processes queued file load operations.
/// @param vfs The VFS driver state.
static void vfs_process_loads(vfs_state_t *vfs)
{
    while (vfs->ActiveCount < MAX_OPEN_FILES)
    {
        vfs_lfreq_t   req;
        if (srmw_fifo_get(&vfs->LoadQueue, req) == false)
        {   // there are no pending loads, so we're done.
            break;
        }

        // the file is already open; it was opened during platform_load_file().
        // all we need to do is update our internal active file list.
        size_t index = vfs->ActiveCount++;
        vfs->FileAFID[index]            = req.AFID;
        vfs->FileType[index]            = req.Type;
        vfs->FileMode[index]            = READ_LOAD;
        vfs->Priority[index]            = req.Priority;
        vfs->RdOffset[index]            = 0;  // the *relative* offset
        vfs->FileInfo[index].Fildes     = req.Fildes;
        vfs->FileInfo[index].Eventfd    = req.Eventfd;
        vfs->FileInfo[index].FileSize   = req.FileSize;
        vfs->FileInfo[index].DataSize   = req.DataSize;
        vfs->FileInfo[index].FileOffset = req.FileOffset;
        vfs->FileInfo[index].SectorSize = req.SectorSize;
    }
}

/// @summary Processes any pending file close requests.
/// @param vfs The VFS driver state.
static void vfs_process_closes(vfs_state_t *vfs)
{
    while (vfs->ActiveCount > 0)
    {   // grab the next close command from the queue.
        vfs_cfreq_t   req;
        if (srmw_fifo_get(&vfs->CloseQueue, req) == false)
        {   // there are no more pending closes, so we're done.
            break;
        }

        // locate the corresponding file record and queue a close to VFS.
        // it's important that the operation go through the VFS queue, so
        // that we can be sure any pending I/O operations on the file have
        // been submitted prior to the file being closed.
        intptr_t const  AFID     = req.AFID;
        intptr_t const *AFIDList = vfs->FileAFID;
        for (size_t  i = 0 ; i < vfs->ActiveCount; ++i)
        {
            if (AFIDList[i] == AFID)
            {
                aio_req_t *aio_req = io_opq_put(&vfs->IoOperations, vfs->Priority[i]);
                if (aio_req != NULL)
                {   // fill out the request. it will be processed at a later time.
                    aio_req->Command    = AIO_COMMAND_CLOSE;
                    aio_req->Fildes     = vfs->FileInfo[i].Fildes;
                    aio_req->Eventfd    = vfs->FileInfo[i].Eventfd;
                    aio_req->DataAmount = 0;
                    aio_req->FileOffset = 0;
                    aio_req->DataBuffer = NULL;
                    aio_req->QTimeNanos = nanotime();
                    aio_req->ATimeNanos = 0;
                    aio_req->AFID       = AFID;
                    aio_req->Type       = vfs->FileType[i];
                    aio_req->Reserved   = 0;

                    // delete the file from our internal state immediately.
                    size_t const last   = vfs->ActiveCount - 1;
                    vfs->FileAFID[i]    = vfs->FileAFID[last];
                    vfs->FileType[i]    = vfs->FileType[last];
                    vfs->FileMode[i]    = vfs->FileMode[last];
                    vfs->Priority[i]    = vfs->Priority[last];
                    vfs->RdOffset[i]    = vfs->RdOffset[last];
                    vfs->FileInfo[i]    = vfs->FileInfo[last];
                    vfs->ActiveCount    = last;
                    break; // dequeue the next request.
                }
                else
                {   // there's no more space in the pending I/O operation queue.
                    // put this request back in the queue and try again later.
                    // no point in continuing on with further processing.
                    // TODO: track this statistic somewhere.
                    srmw_fifo_put(&vfs->CloseQueue, req);
                    return;
                }
            } // if (AFIDList[i] == AFID)
        } // for (each active file)
    } // while (active files)
}

/// @summary Processes all completed file close notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
static void vfs_process_completed_closes(vfs_state_t *vfs, aio_state_t *aio)
{   // poll aio->CloseResults and perform any necessary operations.
    // our internal file state has already been deleted.
    // there's probably nothing to do here.
}

/// @summary Processes all completed file read notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
static void vfs_process_completed_reads(vfs_state_t *vfs, aio_state_t *aio)
{   // poll aio->ReadResults and perform any necessary operations.
    // for each read, call the appropriate callback based on type,
    // and then return the allocated buffer to the free pool.
}

/// @summary Processes all completed file write notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
static void vfs_process_completed_writes(vfs_state_t *vfs, aio_state_t *aio)
{   // poll aio->WriteResults and perform any necessary operations.
    // for each write, call the appropriate callback based on type,
    // and then return the allocated buffer to the free pool.
}

/// @summary Processes all completed file flush notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
static void vfs_process_completed_flushes(vfs_state_t *vfs, aio_state_t *aio)
{   // poll aio->FlushResults and perform any necessary operations.
    // there's probably nothing to do here.
}

/// @summary Updates the status of all active file loads, and submits I/O operations.
/// @param vfs The VFS driver state.
/// @return true if the tick should continue submitting I/O operations, or false if
/// either buffer space is full or the I/O operation queue is full.
static bool vfs_update_loads(vfs_state_t *vfs)
{
    iobuf_alloc_t &allocator = vfs->IoAllocator;
    size_t const read_amount = allocator.AllocSize;
    size_t       file_count  = 0;
    uint32_t     priority    = 0;
    uint16_t     index       = 0;
    uint16_t     index_list[MAX_OPEN_FILES];
    vfs_io_fpq_t file_queue;

    io_fpq_clear(&file_queue);
    file_count = map_files_by_mode(index_list, vfs, READ_LOAD);
    build_file_queue(&file_queue, vfs, index_list, file_count);
    while(io_fpq_get(&file_queue, index, priority))
    {
        size_t nqueued = 0;
        while (iobuf_bytes_free(allocator) > 0)
        {   // allocate a new request in our internal operation queue.
            aio_req_t *req  = io_opq_put(&vfs->IoOperations, priority);
            if (req != NULL)
            {
                req->Command    = AIO_COMMAND_READ;
                req->Fildes     = vfs->FileInfo[index].Fildes;
                req->Eventfd    = vfs->FileInfo[index].Eventfd;
                req->DataAmount = read_amount;
                req->FileOffset = vfs->FileInfo[index].FileOffset + vfs->RdOffset[index];
                req->DataBuffer = iobuf_get(allocator);
                req->QTimeNanos = nanotime();
                req->ATimeNanos = 0;
                req->AFID       = vfs->FileAFID[index];
                req->Type       = vfs->FileType[index];
                req->Reserved   = 0;
                nqueued++;

                // update the byte offset to the next read.
                int64_t newofs  = vfs->RdOffset[index] + read_amount;
                vfs->RdOffset[index] = newofs;
                if (newofs >= vfs->FileInfo[index].FileSize)
                {   // reached or passed end-of-file; queue a close.
                    // we will continue on with the next queued file.
                    vfs_cfreq_t creq;
                    creq.AFID = vfs->FileAFID[index];
                    srmw_fifo_put(&vfs->CloseQueue, creq);
                    break;
                }
            }
            // we ran out of I/O queue space; no point in continuing.
            // TODO: track this statistic somewhere, we want to know
            // how often this happens.
            else return false;
        }
        if (nqueued == 0)
        {   // we ran out of I/O buffer space; no point in continuing.
            // TODO: track this statistic somewhere, we want to know
            // how often this happens.
            return false;
        }
    }
    return true;
}

/// @summary Processes all queued explicit read operations.
/// @param vfs The VFS driver state.
/// @return true if the tick should continue submitting I/O operations, or false if
/// either buffer space is full or the I/O operation queue is full.
static bool vfs_process_reads(vfs_state_t *vfs)
{   // TODO: process any pending explicit read operations.
    return true;
}

/// @summary Processes all queued explicit write operations.
/// @param vfs The VFS driver state.
/// @return true if the tick should continue submitting I/O operations, or false if
/// either buffer space is full or the I/O operation queue is full.
static bool vfs_process_writes(vfs_state_t *vfs)
{   // TODO: process any pending explicit write operations.
    return true;
}

/// @summary Implements the main body of the VFS update loop, which processes
/// requests from the application layer, submits I/O requests to the AIO driver,
/// and dispatches completion notifications from the AIO layer back to the application.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
static void vfs_tick(vfs_state_t *vfs, aio_state_t *aio)
{
    // process completions and free up as much buffer state as possible.
    vfs_process_completed_reads  (vfs, aio);
    vfs_process_completed_writes (vfs, aio);
    vfs_process_completed_flushes(vfs, aio);
    vfs_process_completed_closes (vfs, aio);

    // generate read and write I/O operations.
    vfs_update_loads(vfs);
    vfs_process_reads(vfs);
    vfs_process_writes(vfs);

    // close file requests should be processed after all read and write requests.
    // this ensures that all I/O has been submitted before closing the file.
    vfs_process_closes(vfs);

    // we're done generating operations, so push as much as possible to AIO.
    aio_req_t request;
    while (io_opq_top(&vfs->IoOperations, request))
    {   // we were able to retrieve an operation from our internal queue.
        if (srsw_fifo_put(&aio->RequestQueue, request))
        {   // we were able to push it to AIO, so remove it from our queue.
            io_opq_get(&vfs->IoOperations, request);
        }
    }

    // open file requests should be processed after all close requests.
    vfs_process_opens(vfs);
    vfs_process_loads(vfs);
}
/*struct vfs_state_t
{
    #define MF         MAX_OPEN_FILES
    #define NT         FILE_TYPE_COUNT
    vfs_cfq_t          CloseQueue;   /// The queue for file close requests.
    vfs_lfq_t          LoadQueue;    /// The queue for complete file load requests.
    iobuf_alloc_t      IoAllocator;  /// The I/O buffer allocator.
    size_t             ActiveCount;  /// The number of open files.
    intptr_t           FileAFID[MF]; /// An application-defined ID for each active file.
    int32_t            FileType[MF]; /// One of file_mode_e for each active file.
    int32_t            FileMode[MF]; /// One of vfs_mode_e for each active file.
    uint32_t           Priority[MF]; /// The access priority for each active file.
    int64_t            RdOffset[MF]; /// The current read offset for each active file.
    vfs_fdinfo_t       FileInfo[MF]; /// The constant data for each active file.
    vfs_io_opq_t       IoOperations; /// The priority queue of pending I/O operations.
    #undef NT
    #undef MF
};*/

/*////////////////////////
//   Public Functions   //
////////////////////////*/
/// @summary Queues a file for loading. The file is read from beginning to end and
/// data is returned to the application on the thread appropriate for the given type.
/// @param path The NULL-terminated UTF-8 path of the file to load.
/// @param id The application-defined identifier for the load request.
/// @param type One of file_type_e indicating the type of file being loaded. This allows
/// the platform to decide the thread on which data should be returned to the application.
/// @param priority The file loading priority, with 0 indicating the highest possible priority.
/// @param file_size On return, this location is updated with the logical size of the file.
/// @return true if the file was successfully opened and the load was queued.
bool platform_load_file(char const *path, intptr_t id, int32_t type, uint32_t priority, int64_t &file_size)
{
    int     fd     = -1;
    int     efd    = -1;
    size_t  ssize  =  0;
    int64_t lsize  =  0;
    int64_t psize  =  0;
    int64_t offset =  0;
    if (vfs_resolve_file(path, fd, efd, lsize, psize, offset, ssize))
    {   // queue a load file request to be processed by the VFS driver.
        vfs_lfreq_t req;
        req.Next       = NULL;
        req.Fildes     = fd;
        req.Eventfd    = efd;
        req.DataSize   = lsize;  // size of the file after decompression
        req.FileSize   = psize;  // number of bytes to read from the file
        req.FileOffset = offset; // offset of first byte relative to fd 0, SEEK_SET
        req.AFID       = id;
        req.Type       = type;
        req.Priority   = priority;
        req.SectorSize = ssize;
        file_size      = lsize;  // return the logical size to the caller
        srmw_fifo_put(&VFS_STATE.LoadQueue, req);
        return true;
    }
    else
    {   // unable to open the file, so fail immediately.
        file_size = 0;
        return false;
    }
}

int main(int argc, char **argv)
{
    exit(EXIT_SUCCESS);
}

