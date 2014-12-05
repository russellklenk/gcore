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

/*///////////////////////
//   Local Functions   //
///////////////////////*/
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

    exit(exit_code);
}

