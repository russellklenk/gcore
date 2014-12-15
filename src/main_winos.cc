/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the entry point of the application.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////////
//   Preprocessor   //
////////////////////*/
#define COMPILER_MFENCE_READ       _ReadBarrier()
#define COMPILER_MFENCE_WRITE      _WriteBarrier()
#define COMPILER_MFENCE_READ_WRITE _ReadWriteBarrier()
#define never_inline               __declspec(noinline)

#ifdef __GNUC__
    #ifndef QUOTA_LIMITS_HARDWS_MIN_ENABLE
        #define QUOTA_LIMITS_HARDWS_MIN_ENABLE     0x00000001
    #endif

    #ifndef QUOTA_LIMITS_HARDWS_MAX_DISABLE
        #define QUOTA_LIMITS_HARDWS_MAX_DISABLE    0x00000008
    #endif
#endif

/*////////////////
//   Includes   //
////////////////*/
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include <windows.h>

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

/// @summary The spin count used on critical sections protecting shared resources
/// of srmw_freelist_t and srmw_fifo_t.
static DWORD    const SPIN_COUNT_Q   = 4096;

/*///////////////////
//   Local Types   //
///////////////////*/
/// @summary Define the supported AIO commands.
enum aio_command_e
{
    AIO_COMMAND_READ  = 0, /// Data should be read from the file.
    AIO_COMMAND_WRITE = 1, /// Data should be written to the file.
    AIO_COMMAND_FLUSH = 2, /// Any pending writes should be flushed to disk.
    AIO_COMMAND_CLOSE = 3, /// The file should be closed.
};

/// @summary Define the supported VFS access modes for a file.
enum vfs_mode_e
{
    VFS_MODE_LOAD     = 0, /// The file is being loaded entirely and then closed.
    VFS_MODE_EXPLICIT = 1, /// The file reads are controlled by the user.
};

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
    CRITICAL_SECTION   Lock;         /// Mutex protecting the Head pointer.
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
    CRITICAL_SECTION   HeadLock;     /// Mutex protecting the Head pointer.
    T                 *Head;         /// The current front-of-queue.
    CRITICAL_SECTION   TailLock;     /// Mutex protecting the Tail pointer.
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
    int64_t            BaseOffset;   /// The absolute byte offset of the start of the file, or 0.
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
    OVERLAPPED         ASIOPool[MA]; /// The static pool of OVERLAPPED structures.
    HANDLE             ASIOContext;  /// The I/O completion port handle.
    size_t             ActiveCount;  /// The number of in-flight AIO requests.
    aio_req_t          AAIOList[MA]; /// The set of active AIO requests [ActiveCount valid].
    OVERLAPPED        *ASIOList[MA]; /// The dynamic list of active OVERLAPPED [ActiveCount valid].
    size_t             ASIOFreeCount;/// The number of available OVERLAPPED.
    OVERLAPPED        *ASIOFree[MA]; /// The list of available OVERLAPPED [ASIOFreeCount valid].
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

/// @summary Defines the data associated with a completed I/O (read or write) operation.
struct vfs_res_t
{
    intptr_t           AFID;         /// The application-defined ID for the file.
    void              *DataBuffer;   /// The source or target buffer.
    int64_t            FileOffset;   /// The absolute byte offset of the start of the operation. (CONVERT BACK TO RELATIVE!)
    uint32_t           DataAmount;   /// The amount of data transferred.
    int                OSError;      /// The error code returned by the operation, or 0.
};

#define QC  (AIO_MAX_ACTIVE * 2)
typedef srmw_fifo_t<vfs_lfreq_t>     vfs_lfq_t;     /// Load file request queue.
typedef srmw_fifo_t<vfs_cfreq_t>     vfs_cfq_t;     /// Close file request queue.
typedef srsw_fifo_t<vfs_res_t, QC>   vfs_resultq_t; /// AIO result queue.
typedef srsw_fifo_t<void*, QC>       vfs_returnq_t; /// I/O buffer return queue.
#undef  QC

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
    vfs_resultq_t      IoResult[NT]; /// The per-file type queue for I/O results.
    vfs_returnq_t      IoReturn[NT]; /// The per-file type queue for I/O buffer returns.
    #undef NT
    #undef MF
};

/*///////////////
//   Globals   //
///////////////*/
/// @summary The frequency of the high-resolution timer on the system.
static LARGE_INTEGER CLOCK_FREQUENCY  = {0};

/// @summary A list of all of the file type identifiers we consider to be valid.
static file_type_e   FILE_TYPE_LIST[] = {
    FILE_TYPE_DDS,
    FILE_TYPE_TGA,
    FILE_TYPE_WAV,
    FILE_TYPE_JSON
};

// The following functions are not available under MinGW, so kernel32.dll is
// loaded and these functions will be resolved manually.
typedef void (WINAPI *GetNativeSystemInfoFn)(SYSTEM_INFO*);
typedef BOOL (WINAPI *SetProcessWorkingSetSizeExFn)(HANDLE, SIZE_T, SIZE_T, DWORD);

static GetNativeSystemInfoFn        GetNativeSystemInfo_Func        = NULL;
static SetProcessWorkingSetSizeExFn SetProcessWorkingSetSizeEx_Func = NULL;

/*///////////////////////
//   Local Functions   //
///////////////////////*/
/// @summary Redirect a call to GetNativeSystemInfo to GetSystemInfo.
/// @param sys_info The SYSTEM_INFO structure to populate.
static void WINAPI GetNativeSystemInfo_Fallback(SYSTEM_INFO *sys_info)
{
    GetSystemInfo(sys_info);
}

/// @summary Redirect a call to SetProcessWorkingSetSizeEx to SetProcessWorkingSetSize
/// on systems that don't support the Ex version.
/// @param process A handle to the process, or GetCurrentProcess() pseudo-handle.
/// @param minimum The minimum working set size, in bytes.
/// @param maximum The maximum working set size, in bytes.
/// @param flags Ignored. See MSDN for SetProcessWorkingSetSizeEx.
/// @return See MSDN for SetProcessWorkingSetSize.
static BOOL WINAPI SetProcessWorkingSetSizeEx_Fallback(HANDLE process, SIZE_T minimum, SIZE_T maximum, DWORD /*flags*/)
{
    return SetProcessWorkingSetSize(process, minimum, maximum);
}

/// @summary Loads function entry points that may not be available at compile
/// time with some build environments.
static void resolve_kernel_apis(void)
{   // it's a safe assumption that kernel32.dll is mapped into our process
    // address space already, and will remain mapped for the duration of execution.
    // note that some of these APIs are Vista/WS2008+ only, so make sure that we
    // have an acceptable fallback in each case to something available earlier.
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (kernel != NULL)
    {
        GetNativeSystemInfo_Func        = (GetNativeSystemInfoFn)        GetProcAddress(kernel, "GetNativeSystemInfo");
        SetProcessWorkingSetSizeEx_Func = (SetProcessWorkingSetSizeExFn) GetProcAddress(kernel, "SetProcessWorkingSetSizeEx");
    }
    // fallback if any of these APIs are not available.
    if (GetNativeSystemInfo_Func        == NULL) GetNativeSystemInfo_Func = GetNativeSystemInfo_Fallback;
    if (SetProcessWorkingSetSizeEx_Func == NULL) SetProcessWorkingSetSizeEx_Func = SetProcessWorkingSetSizeEx_Fallback;
}

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

/// @summary Initializes the high-resolution timer.
static inline void inittime(void)
{
    QueryPerformanceFrequency(&CLOCK_FREQUENCY);
}

/// @summary Reads the current tick count for use as a timestamp.
/// @return The current timestamp value, in nanoseconds.
static inline uint64_t nanotime(void)
{
    LARGE_INTEGER tsc = {0};
    LARGE_INTEGER tsf = CLOCK_FREQUENCY;
    QueryPerformanceCounter(&tsc);
    return (SEC_TO_NANOSEC * uint64_t(tsc.QuadPart) / uint64_t(tsf.QuadPart));
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
    InitializeCriticalSectionAndSpinCount(&list.Lock, SPIN_COUNT_Q);
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
        DeleteCriticalSection(&list.Lock);
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

    // pop the node at the front of the list.
    EnterCriticalSection(&list.Lock);
    node = list.Head->Next;
    if (list.Head->Next != NULL)
    {   // after popping node, the list is not empty.
        list.Head->Next  = node->Next;
    }
    LeaveCriticalSection(&list.Lock);

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
    // push the node at the front of the free list.
    EnterCriticalSection(&list.Lock);
    node->Next        = list.Head->Next;
    list.Head->Next   = node;
    LeaveCriticalSection(&list.Lock);
}

/// @summary Allocate storage for a new unbounded SRMW FIFO with the specified capacity.
/// @param fifo The FIFO to initialize.
/// @param capacity The initial capacity of the FIFO.
/// @return true if the FIFO is initialized.
template <typename T>
static bool create_srmw_fifo(srmw_fifo_t<T> *fifo, size_t capacity)
{
    InitializeCriticalSectionAndSpinCount(&fifo->HeadLock, 0);
    InitializeCriticalSectionAndSpinCount(&fifo->TailLock, 0);
    create_srmw_freelist(fifo->FreeList, capacity);
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
    DeleteCriticalSection(&fifo->TailLock);
    DeleteCriticalSection(&fifo->HeadLock);
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

    EnterCriticalSection(&fifo->HeadLock);
    old_head = fifo->Head;       // never NULL (will point to XXXX)
    new_head = fifo->Head->Next; // NULL if the queue is now empty
    if (new_head  != NULL)
    {   // the queue was not empty (we popped an item)
        result     = true;
        fifo->Head = new_head;
    }
    LeaveCriticalSection(&fifo->HeadLock);

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
    EnterCriticalSection(&fifo->TailLock);
    fifo->Tail->Next = node;
    fifo->Tail = node;
    LeaveCriticalSection(&fifo->TailLock);
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
    SYSTEM_INFO sysinfo = {0};
    GetNativeSystemInfo_Func(&sysinfo);

    // round the allocation size up to an even multiple of the page size.
    // round the total size up to an even multiple of the allocation size.
    // note that VirtualAlloc will further round up the total size of the
    // allocation to the nearest sysinfo.dwAllocationGranularity (64K)
    // boundary, but this extra padding will be 'lost' to us.
    size_t page_size = sysinfo.dwPageSize;
    alloc_size       = align_up(alloc_size, page_size);
    total_size       = align_up(total_size, alloc_size);
    size_t nallocs   = total_size / alloc_size;

    // in order to lock the entire allocated region in physical memory, we
    // might need to increase the size of the process' working set. this is
    // Vista and Windows Server 2003+ only, and requires that the process
    // be running as (at least) a Power User or Administrator.
    HANDLE process   = GetCurrentProcess();
    SIZE_T min_wss   = 0;
    SIZE_T max_wss   = 0;
    DWORD  wss_flags = QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE;
    GetProcessWorkingSetSize(process, &min_wss, &max_wss);
    min_wss += total_size;
    max_wss += total_size;
    if (!SetProcessWorkingSetSizeEx_Func(process, min_wss, max_wss, wss_flags))
    {   // the minimum working set size could not be set.
        return false;
    }

    // reserve and commit the entire region, and then pin it in physical memory.
    // this prevents the buffers from being paged out during normal execution.
    void  *baseaddr = VirtualAlloc(NULL, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (baseaddr == NULL)
    {   // the requested amount of memory could not be allocated.
        return false;
    }
    if (!VirtualLock(baseaddr, total_size))
    {   // the pages could not be pinned in physical memory.
        // it's still possible to run in this case; don't fail.
    }

    void **freelist = (void**) malloc(nallocs * sizeof(void*));
    if (freelist == NULL)
    {   // the requested memory could not be allocated.
        VirtualUnlock(baseaddr , total_size);
        VirtualFree(baseaddr, 0, MEM_RELEASE);
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
    if (alloc.FreeList != NULL)
    {
        free(alloc.FreeList);
    }
    if (alloc.BaseAddress != NULL)
    {
        VirtualUnlock(alloc.BaseAddress , alloc.TotalSize);
        VirtualFree(alloc.BaseAddress, 0, MEM_RELEASE);
    }
    alloc.BaseAddress = NULL;
    alloc.FreeCount   = 0;
    alloc.FreeList    = NULL;
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

/// @summary Checks a file type value to make sure it is known.
/// @param file_type One of the values of the file_type_e enumeration.
/// @return true if the file type is known.
static bool check_file_type(int32_t file_type)
{
    size_t  const  ntypes   = sizeof(FILE_TYPE_LIST) / sizeof(FILE_TYPE_LIST[0]);
    int32_t const *typelist = (int32_t const*) FILE_TYPE_LIST;
    for (size_t i = 0; i  < ntypes; ++i)
    {
        if (typelist[i] == file_type)
            return true;
    }
    return false;
}


/*////////////////////////
//   Public Functions   //
////////////////////////*/
/// @summary Entry point of the application.
/// @param argc The number of command-line arguments.
/// @param argv An array of NULL-terminated strings specifying command-line arguments.
/// @return Either EXIT_SUCCESS or EXIT_FAILURE.
int main(int argc, char **argv)
{
    int exit_code = EXIT_SUCCESS;

    if (argc > 1)
    {
        /* USAGE */
    }

    // initialize the high-resolution timer on the system.
    inittime();
    
    // resolve entry points in dynamic libraries.
    // TODO: this will probably need to return success/failure.
    resolve_kernel_apis();

    exit(exit_code);
}

