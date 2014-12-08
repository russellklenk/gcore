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

/*/////////////////
//   Constants   //
/////////////////*/

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
    uint32_t          PushedCount; /// Number of push operations performed.
    uint32_t          PoppedCount; /// Number of pop operations performed.
    uint32_t          Capacity;    /// The queue capacity. Always a power-of-two.
};

/// @summary Defines the data associated with a fixed-size queue safe for
/// concurrent access by a single reader and a single writer. Depends on the
/// srsw_flq_t above, so the same restrictions and caveats apply here.
template <typename T>
struct srsw_fifo_t
{
    srsw_flq_t         Queue;      /// Maintains queue state and capacity.
    T                 *Store;      /// Storage for the queue items.
};

/// @summary A waitable queue safe for concurrent access by a single reader and
/// writer. The implementation of the wait mechanism is platform-dependent; on
/// all platforms, condition variables are used. DO NOT DIRECTLY ACCESS THE
/// FIELDS OF THIS STRUCTURE.
template <typename T>
struct srsw_waitable_fifo_t
{
    srsw_flq_t         Queue;      /// Maintains queue state and capacity.
    T                 *Store;      /// Storage for the queue items.
    CRITICAL_SECTION   MtNotEmpty; /// Mutex guarding the 'not empty' condition.
    CONDITION_VARIABLE CvNotEmpty; /// Used to wait on or signal a not empty condition.
    CRITICAL_SECTION   MtNotFull;  /// Mutex guarding the 'not full' condition.
    CONDITION_VARIABLE CvNotFull;  /// Used to wait on or signal a not full condition.
};

/// @summary Defines the state associated with a direct I/O buffer manager.
/// This object allocates a single large chunk of memory aligned to a multiple
/// of the physical disk sector size, and then allows the caller to allocate
/// fixed-size chunks from within that buffer. The allocator can only be used
/// from a single thread. This allocator can also be used for cached I/O.
struct iobuf_allocator_t
{
    size_t             TotalSize;   /// The total number of bytes allocated.
    size_t             PageSize;    /// The size of a single page, in bytes.
    size_t             AllocSize;   /// The number of pages per-allocation.
    void              *BaseAddress; /// The base address of the committed range.
    size_t             FreeCount;   /// The number of unallocated AllocSize blocks.
    void             **FreeList;    /// Pointers to the start of each unallocated block.
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
// The following functions are not available under MinGW, so kernel32.dll is
// loaded and these functions will be resolved manually.
typedef void (WINAPI *GetNativeSystemInfoFn)(SYSTEM_INFO*);
typedef BOOL (WINAPI *SetProcessWorkingSetSizeExFn)(HANDLE, SIZE_T, SIZE_T, DWORD);

static GetNativeSystemInfoFn        GetNativeSystemInfo_Func        = NULL;
static SetProcessWorkingSetSizeExFn SetProcessWorkingSetSizeEx_Func = NULL;
static bool                         ResolveKernelAPIs               = true;

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
{
    if (ResolveKernelAPIs)
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
        ResolveKernelAPIs = false;
    }
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

/// @summary Create a new SRSW concurrent queue with the specified capacity.
/// @param fifo The queue to initialize.
/// @param capacity The queue capacity. This must be a non-zero power-of-two.
/// @return true if the queue was created.
template <typename T>
static inline bool create_srsw_fifo(srsw_fifo_t<T> *fifo, uint32_t capacity)
{
    if ((fifo != NULL) && (capacity > 0) && ((capacity & (capacity-1)) == 0))
    {
        srsw_flq_clear(fifo->Queue, capacity);
        fifo->Store = (T*) malloc(capacity * sizeof(T));
        return true;
    }
    else return false;
}

/// @summary Frees resources associated with a SRSW concurrent queue.
/// @param fifo The queue to delete.
template <typename T>
static inline void delete_srsw_fifo(srsw_fifo_t<T> *fifo)
{
    if (fifo != NULL)
    {
        if (fifo->Store != NULL)
        {
            free(fifo->Store);
            fifo->Store = NULL;
        }
        srsw_flq_clear(fifo->Queue, fifo->Queue.Capacity);
    }
}

/// @summary Flushes a SRSW concurrent queue. This operation should only be
/// performed after coordination between the producer and the consumer; only
/// one should be accessing the queue at the time.
/// @param fifo The queue to flush.
template <typename T>
static inline void flush_srsw_fifo(srsw_fifo_t<T> *fifo)
{
    srsw_flq_clear(fifo->Queue, fifo->Queue.Capacity);
}

/// @summary Retrieves the number of items 'currently' in the queue.
/// @param fifo The queue to query.
/// @return The number of items in the queue at the instant of the call.
template <typename T>
static inline size_t srsw_fifo_count(srsw_fifo_t<T> *fifo)
{
    return srsw_flq_count(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' empty.
/// @param fifo The queue to query.
/// @return true if the queue contains zero items at the instant of the call.
template <typename T>
static inline bool srsw_fifo_is_empty(srsw_fifo_t<T> *fifo)
{
    return srsw_flq_empty(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' full.
/// @param fifo The queue to query.
/// @return true if the queue is full at the instant of the call.
template <typename T>
static inline bool srsw_fifo_is_full(srsw_fifo_t<T> *fifo)
{
    return srsw_flq_full(fifo->Queue);
}

/// @summary Enqueues an item.
/// @param fifo The destination queue.
/// @param item The item to enqueue. This must be a POD type.
/// @return true if the item was enqueued, or false if the queue is at capacity.
template <typename T>
static inline bool srsw_fifo_put(srsw_fifo_t<T> *fifo, T const &item)
{
    uint32_t count = srsw_flq_count(fifo->Queue) + 1;
    if (count <= fifo->Queue.Capacity)
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
template <typename T>
static inline bool srsw_fifo_get(srsw_fifo_t<T> *fifo, T &item)
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

/// @summary Create a new waitable SRSW concurrent queue with the specified capacity.
/// @param fifo The queue to initialize.
/// @param capacity The queue capacity. This must be a non-zero power-of-two.
/// @return true if the queue was created.
template <typename T>
static bool create_srsw_waitable_fifo(srsw_waitable_fifo_t<T> *fifo, uint32_t capacity)
{
    // ensure we have a valid fifo and that the capacity is a power-of-two.
    // the capacity being a non-zero power-of-two is a requirement for correct
    // functioning of the queue.
    if ((fifo != NULL) && (capacity > 0) && ((capacity & (capacity-1)) == 0))
    {
        srsw_flq_clear(fifo->Queue, capacity);
        fifo->Store = (T*) malloc(capacity * sizeof(T));
        InitializeCriticalSection(&fifo->MtNotEmpty);
        InitializeConditionVariable(&fifo->CvNotEmpty);
        InitializeCriticalSection(&fifo->MtNotFull);
        InitializeConditionVariable(&fifo->CvNotFull);
        return true;
    }
    else return false;
}

/// @summary Frees resources associated with a waitable SRSW concurrent queue.
/// @param fifo The queue to delete.
template <typename T>
static void delete_srsw_fifo(srsw_waitable_fifo_t<T> *fifo)
{
    if (fifo != NULL)
    {
        if (fifo->Store != NULL)
        {
            free(fifo->Store);
            fifo->Store = NULL;
        }
        // force the queue into a not-empty, not-full state.
        srsw_flq_clear(fifo->Queue, fifo->Queue.Capacity);
        srsw_flq_push (fifo->Queue);

        // wake up any waiters on the not-empty condition.
        EnterCriticalSection(&fifo->MtNotEmpty);
        WakeAllConditionVariable(&fifo->CvNotEmpty);
        LeaveCriticalSection(&fifo->MtNotEmpty);

        // wake up any waiters on the not-full condition.
        EnterCriticalSection(&fifo->MtNotFull);
        WakeAllConditionVariable(&fifo->CvNotFull);
        LeaveCriticalSection(&fifo->MtNotFull);

        // destroy the mutexes guarding the conditions.
        DeleteCriticalSection(&fifo->MtNotEmpty);
        DeleteCriticalSection(&fifo->MtNotFull );

        // return the queue to an empty state.
        srsw_flq_clear(fifo->Queue, fifo->Queue.Capacity);
    }
}

/// @summary Flushes a SRSW concurrent queue. This operation should only be
/// performed after coordination between the producer and the consumer; only
/// one should be accessing the queue at the time.
/// @param fifo The queue to flush.
template <typename T>
static void flush_srsw_fifo(srsw_waitable_fifo_t<T> *fifo)
{
    EnterCriticalSection (&fifo->MtNotFull);
    srsw_flq_clear       ( fifo->Queue, fifo->Queue.Capacity);
    WakeConditionVariable(&fifo->CvNotFull);
    LeaveCriticalSection (&fifo->MtNotFull);
}

/// @summary Retrieves the number of items 'currently' in the queue.
/// @param fifo The queue to query.
/// @return The number of items in the queue at the instant of the call.
template <typename T>
static inline size_t srsw_fifo_count(srsw_waitable_fifo_t<T> *fifo)
{
    return srsw_flq_count(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' empty.
/// @param fifo The queue to query.
/// @return true if the queue contains zero items at the instant of the call.
template <typename T>
static inline bool srsw_fifo_is_empty(srsw_waitable_fifo_t<T> *fifo)
{
    return srsw_flq_empty(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' full.
/// @param fifo The queue to query.
/// @return true if the queue is full at the instant of the call.
template <typename T>
static inline bool srsw_fifo_is_full(srsw_waitable_fifo_t<T> *fifo)
{
    return srsw_flq_full(fifo->Queue);
}

/// @summary Blocks the calling thread until the queue reaches a non-empty
/// state, or the specified timeout interval has elapsed. The caller must check
/// the current state of the queue using srsw_fifo_is_empty(fifo) after being
/// woken up, as the queue may no longer be non-empty.
/// @param fifo The queue to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if the queue has reached a non-empty state, or false if the
/// timeout interval has elapsed or an error has occurred.
template <typename T>
static bool srsw_fifo_wait_not_empty(srsw_waitable_fifo_t<T> *fifo, uint32_t timeout_ms)
{
    EnterCriticalSection(&fifo->MtNotEmpty);
    while (srsw_flq_count(fifo->Queue) == 0)
    {   // the queue is currently empty, so wait for a not-empty signal.
        BOOL ok = SleepConditionVariableCS(&fifo->CvNotEmpty, &fifo->MtNotEmpty, timeout_ms);
        if (!ok)
        {   // GetLastError() returns ERROR_TIMEOUT if a timeout occurred.
            LeaveCriticalSection(&fifo->MtNotEmpty);
            return false;
        }
    }
    LeaveCriticalSection(&fifo->MtNotEmpty);
    return true;
}

/// @summary Blocks the calling thread until the queue reaches a non-full
/// state, or the specified timeout interval has elapsed. The caller must check
/// the current state of the queue using srsw_fifo_is_full(fifo) after being
/// woken up, as the queue may no longer be non-full.
/// @param fifo The queue to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if the queue has reached a non-full state, or false if the
/// timeout interval has elapsed or an error has occurred.
template <typename T>
static bool srsw_fifo_wait_not_full(srsw_waitable_fifo_t<T> *fifo, uint32_t timeout_ms)
{
    uint32_t capacity   = fifo->Queue.Capacity;
    EnterCriticalSection(&fifo->MtNotFull);
    while (srsw_flq_count(fifo->Queue) == capacity)
    {   // the queue is currently full, so wait for a not-full signal.
        BOOL ok = SleepConditionVariableCS(&fifo->CvNotFull, &fifo->MtNotFull, timeout_ms);
        if (!ok)
        {   // GetLastError() returns ERROR_TIMEOUT if a timeout occurred.
            LeaveCriticalSection(&fifo->MtNotFull);
            return false;
        }
    }
    LeaveCriticalSection(&fifo->MtNotFull);
    return true;
}

/// @summary Enqueues an item, if the queue is not full, and signals the
/// not-empty and possibly the not-full events.
/// @param fifo The destination queue.
/// @param item The item to enqueue. This must be a POD type.
/// @return true if the item was enqueued, or false if the queue is at capacity.
template <typename T>
static bool srsw_fifo_put(srsw_waitable_fifo_t<T> *fifo, T const &item)
{
    uint32_t capacity = fifo->Queue.Capacity;
    uint32_t count    = srsw_flq_count(fifo->Queue) + 1;
    if (count <= capacity) // <= because 'count' is the incremented count.
    {
        uint32_t    index  = srsw_flq_next_push(fifo->Queue);
        fifo->Store[index] = item;
        COMPILER_MFENCE_WRITE;
        srsw_flq_push(fifo->Queue);
        WakeConditionVariable(&fifo->CvNotEmpty);
        if (count < capacity)
        {   // the queue is also not full.
            WakeConditionVariable(&fifo->CvNotFull);
        }
        return true;
    }
    return false;
}

/// @summary Dequeues an item, if the queue is not empty, and signals the
/// not-full and possibly the not-empty events.
/// @param fifo The source queue.
/// @param item On return, the dequeued item is copied here.
/// @return true if an item was dequeued, or false if the queue is empty.
template <typename T>
static bool srsw_fifo_get(srsw_waitable_fifo_t<T> *fifo, T &item)
{
    uint32_t count = srsw_flq_count(fifo->Queue);
    if (count > 0)
    {
        uint32_t index = srsw_flq_next_pop(fifo->Queue);
        item = fifo->Store[index];
        COMPILER_MFENCE_READ;
        srsw_flq_pop(fifo->Queue);
        WakeConditionVariable(&fifo->CvNotFull);
        if (count > 1)
        {   // the queue is also not empty.
            WakeConditionVariable(&fifo->CvNotEmpty);
        }
        return true;
    }
    return false;
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
/// @return true if the allocator was initialized. Check alloc->TotalSize and
/// alloc->AllocSize to determine the values selected by the system.
static bool create_iobuf_allocator(iobuf_allocator_t *alloc, size_t total_size, size_t alloc_size)
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
    alloc->TotalSize   = total_size;
    alloc->PageSize    = page_size;
    alloc->AllocSize   = alloc_size;
    alloc->BaseAddress = baseaddr;
    alloc->FreeCount   = nallocs;
    alloc->FreeList    = freelist;
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
static void delete_iobuf_allocator(iobuf_allocator_t *alloc)
{
    if (alloc->FreeList != NULL)
    {
        free(alloc->FreeList);
    }
    if (alloc->BaseAddress != NULL)
    {
        VirtualUnlock(alloc->BaseAddress , alloc->TotalSize);
        VirtualFree(alloc->BaseAddress, 0, MEM_RELEASE);
    }
    alloc->BaseAddress = NULL;
    alloc->FreeCount   = 0;
    alloc->FreeList    = NULL;
}

/// @summary Returns all I/O buffers to the free list of the allocator, regardless
/// of whether any I/O buffers are in use by the application.
/// @param alloc The I/O buffer allocator to flush.
static void flush_iobuf_allocator(iobuf_allocator_t *alloc)
{
    size_t const nallocs = alloc->TotalSize / alloc->AllocSize;
    size_t const allocsz = alloc->AllocSize;
    uint8_t       *bufit = (uint8_t*) alloc->BaseAddress;
    void         **freel = alloc->FreeList;
    for (size_t i = 0; i < nallocs; ++i)
    {
        freel[i]  = bufit;
        bufit    += allocsz;
    }
    alloc->FreeCount = nallocs;
}

/// @summary Retrieves an I/O buffer from the pool.
/// @param alloc The I/O buffer allocator to query.
/// @return A pointer to the I/O buffer, or NULL if no buffers are available.
static inline void* iobuf_get(iobuf_allocator_t *alloc)
{
    if (alloc->FreeCount > 0)
    {   // return the next buffer from the free list,
        // which is typically the most recently used buffer.
        return alloc->FreeList[--alloc->FreeCount];
    }
    else return NULL; // no buffers available for use.
}

/// @summary Returns an I/O buffer to the pool.
/// @param alloc The I/O buffer allocator that owns the buffer.
/// @param iobuf The address of the buffer returned by iobuf_get().
static inline void iobuf_put(iobuf_allocator_t *alloc, void *iobuf)
{
    assert(iobuf != NULL);
    alloc->FreeList[alloc->FreeCount++] = iobuf;
}

/// @summary Calaculate the number of bytes currently unused.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of bytes currently available for use by the application.
static inline size_t iobuf_allocator_bytes_free(iobuf_allocator_t const *alloc)
{
    return (alloc->AllocSize *  alloc->FreeCount);
}

/// @summary Calaculate the number of bytes currently allocated.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of bytes currently in-use by the application.
static inline size_t iobuf_allocator_bytes_used(iobuf_allocator_t const *alloc)
{
    return  alloc->TotalSize - (alloc->AllocSize * alloc->FreeCount);
}

/// @summary Calculate the number of buffers currently allocated.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of buffers currently in-use by the application.
static inline size_t iobuf_allocator_buffers_used(iobuf_allocator_t const *alloc)
{
    size_t const nallocs = alloc->TotalSize / alloc->AllocSize;
    size_t const nunused = alloc->FreeCount;
    return (nallocs - nunused);
}

/// @summary Perform any required setup of the platform-specific virtual file
/// system and I/O system. Calls to open files are routed through the VFS and
/// eventually are translated into filesystem operations.
static bool platform_setup_io(void)
{
    // TODO: Initialize the application virtual file system on this platform.
    // This will typically involve things like directory enumeration and
    // the reading and parsing of packages. Once that is done, initialize the
    // platform I/O subsystem.
    return true;
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

/// @summary attempts to open a file and read it from beginning to end.
/// @param path the location of the file to load.
/// @param file_type One of the values of the file_type_e enumeration.
/// @param app_id the application-defined identifier associated with the file.
bool platform_read_file(char const *path, int32_t file_type, uint32_t app_id)
{
#if DEBUG
    if (!check_file_type(file_type))
    {
        IoCallback->IoError(app_id, file_type, EINVAL, "Invalid file type");
        return false;
    }
#endif
    // TODO: route mounting of the file through the VFS.
    return false;
}

/// @summary Closes a file previously opened with platform_read_file. This
/// should be called when the application has finished processing the file
/// data, or when the platform has reported an error while reading the file.
/// @param file_type One of the values of the file_type_e enumeration.
/// @param app_id The application-defined identifier of the file to close.
void platform_close_file(int32_t file_type, uint32_t app_id)
{
    // TODO: route closing of the file through the VFS.
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
    if (!resolve_kernel_apis())
    {
        /* ERROR */
    }
    if (!platform_setup_io())
    {
        fprintf(stderr, "FATAL: Unable to setup the platform Virtual File System.\n");
        exit(EXIT_FAILURE);
    }

    exit(exit_code);
}

