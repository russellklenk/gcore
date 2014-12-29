/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the entry point of the application.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////////
//   Preprocessor   //
////////////////////*/
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
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/param.h>
#include <sys/eventfd.h>
#include <sys/vfs.h>

#include "bridge.h"
#include "decode.cc"
#include "datain.cc"

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

/// @summary Define the maximum number of concurrent stream-in files.
#ifndef LINUX_MAX_STREAMS_IN
#define LINUX_MAX_STREAMS_IN    16
#endif
static size_t   const MAX_STREAMS_IN = LINUX_MAX_STREAMS_IN;

/// @summary Define the maximum number of concurrently active decoder streams.
/// This needs to be larger than the maximum number of active stream-in files,
/// as decode will lag behind I/O slightly in most cases.
static size_t   const MAX_DECODER_IN = MAX_STREAMS_IN * 2;

/// @summary Define the maximum number of concurrently active AIO operations.
/// We set this based on what the maximum number of AIO operations we want to
/// poll during each tick, and the maximum number the underlying OS can handle.
/// To see what the value for this should be on your system:
/// cat /sys/block/sda/queue/nr_requests
#ifndef LINUX_AIO_MAX_ACTIVE    // can override at compile time
#define LINUX_AIO_MAX_ACTIVE    128
#endif
static size_t   const AIO_MAX_ACTIVE = LINUX_AIO_MAX_ACTIVE;

/// @summary Define the size of the I/O buffer. This is calculated based on an
/// maximum I/O transfer rate of 960MB/s, and an I/O system tick rate of 60Hz;
/// 960MB/sec divided across 60 ticks/sec gives 16MB/tick maximum transfer rate.
#ifndef LINUX_VFS_IOBUF_SIZE    // can override at compile time
#define LINUX_VFS_IOBUF_SIZE   (16 * 1024 * 1024)
#endif
static size_t   const VFS_IOBUF_SIZE = LINUX_VFS_IOBUF_SIZE;

/// @summary Define the size of the write buffer for output streams. This must
/// be at least the size of a single page, as buffers are allocated using mmap.
#ifndef LINUX_VFS_WRITE_SIZE    // can override at compile time
#define LINUX_VFS_WRITE_SIZE   (64 * 1024)
#endif
static size_t   const VFS_WRITE_SIZE = LINUX_VFS_WRITE_SIZE;

/// @summary Define the size of the buffer allocated for each I/O request.
static size_t   const VFS_ALLOC_SIZE = VFS_IOBUF_SIZE / AIO_MAX_ACTIVE;

/// @summary Define the file size limit for preferring buffered I/O. Large
/// files can pollute the page cache and will reduce overall I/O throughput.
#ifndef LINUX_VFS_DIRECT_IO_THRESHOLD
#define LINUX_VFS_DIRECT_IO_THRESHOLD (16 * 1024 * 1024)
#endif
static int64_t  const VFS_DIRECT_IO_THRESHOLD = LINUX_VFS_DIRECT_IO_THRESHOLD;

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
    AIO_COMMAND_FINAL = 4, /// The file should be closed and renamed.
};

/// @summary Defines the supported VFS stream-in status flags.
enum vfs_stream_in_status_e
{
    VFS_STATUS_NONE   = (0 << 0), /// No special status bits are set.
    VFS_STATUS_PAUSE  = (1 << 0), /// The stream is currently paused.
    VFS_STATUS_CLOSE  = (1 << 0), /// The stream is marked as having a close pending.
};

/// @summary Defines the supported VFS stream-in control operations.
enum vfs_stream_in_operation_e
{
    STREAM_IN_PAUSE   = 0, /// Stream loading should be paused.
    STREAM_IN_RESUME  = 1, /// Stream loading should be resumed from the current position.
    STREAM_IN_REWIND  = 2, /// Restart stream loading from the beginning of the stream.
    STREAM_IN_SEEK    = 3, /// Seek to a position within the stream and start loading.
    STREAM_IN_STOP    = 4, /// Stop stream loading and close the stream.
};

/// @summary Defines the supported file open hints.
enum vfs_file_hint_e
{
    FILE_HINT_NONE    = (0 << 0), /// No special hints are provided.
    FILE_HINT_DIRECT  = (1 << 0), /// Prefer unbuffered I/O.
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

/// @summary Defines the data associated with a stream in creation request passed
/// to the VFS driver. This structure is intended for storage in a srmw_fifo_t.
struct vfs_sics_t
{
    vfs_sics_t        *Next;         /// Pointer to the next node in the queue.
    int                Fildes;       /// The file descriptor of the opened file.
    int                Eventfd;      /// The eventfd descriptor of the opened file.
    int64_t            DataSize;     /// The logical size of the file, in bytes.
    int64_t            FileSize;     /// The physical size of the file, in bytes.
    int64_t            FileOffset;   /// The byte offset of the start of the file data.
    intptr_t           ASID;         /// The application-defined ID for the file.
    int32_t            Type;         /// The file type, one of file_type_e.
    int32_t            Behavior;     /// The behavior at end-of-stream, one of stream_in_mode_e.
    uint32_t           Priority;     /// The file access priority (0 = highest).
    size_t             SectorSize;   /// The physical sector size of the disk.
    stream_decoder_t  *Decoder;      /// The initialized stream decoder state.
};

/// @summary Defines the data associated with a stream-in control operation,
/// which may include pause, resume, rewind or stop. This structure is intended
/// for storage in an srmw_fifo_t.
struct vfs_siop_t
{
    vfs_siop_t        *Next;         /// Pointer to the next node in the queue.
    intptr_t           ASID;         /// The application-defined ID for the stream.
    int32_t            OpId;         /// The operation ID, one of vfs_stream_in_op_e.
    int64_t            Argument;     /// Optional data associated with the command.
};

/// @summary Defines the data associated with a completed stream-in read operation.
struct vfs_sird_t
{
    intptr_t           ASID;         /// The application-defined ID for the stream.
    void              *DataBuffer;   /// The buffer containing the data that was read.
    int64_t            FileOffset;   /// The absolute byte offset of the start of the operation.
    uint32_t           DataAmount;   /// The amount of data transferred.
    int                OSError;      /// The error code returned by the operation, or 0.
    stream_decoder_t  *Decoder;      /// The stream decoder state.
};

/// @summary Defines the data associated with an end-of-stream notification.
struct vfs_sies_t
{
    intptr_t           ASID;         /// The application-defined ID for the stream.
    int32_t            Behavior;     /// The configured behavior of the stream.
};

/// @summary Defines the data associated with a stream-out write request passed
/// to the VFS driver. This structure is intended for storage in a srmw_fifo_t.
struct vfs_sowr_t
{
    vfs_sowr_t        *Next;         /// Pointer to the next node in the queue.
    int                Fildes;       /// The file descriptor of the opened file.
    int                Eventfd;      /// The eventfd descriptor of the opened file.
    int64_t            FileOffset;   /// The byte offset at which the write begins.
    void              *DataBuffer;   /// The buffer containing the data to write.
    uint32_t           DataSize;     /// The number of bytes to write (constant).
    uint32_t           Priority;     /// The stream priority (0 = highest).
};

/// @summary Defines the data associated with a stream-out close request passed
/// to the VFS driver. This structure is intended for storage in a srmw_fifo_t.
struct vfs_socs_t
{
    vfs_socs_t        *Next;         /// Pointer to the next node in the queue.
    int                Fildes;       /// The file descriptor of the opened file.
    int                Eventfd;      /// The eventfd descriptor of the opened file.
    uint32_t           Priority;     /// The stream priority (0 = highest).
    char              *FilePath;     /// The target path, allocated with strdup(), or NULL.
    int64_t            FileSize;     /// The logical size of the file, in bytes.
};

#define QC  (AIO_MAX_ACTIVE * 2)
#define MS  (MAX_STREAMS_IN)
typedef srmw_fifo_t<vfs_sowr_t>      vfs_sowriteq_t;   /// A stream-out write queue.
typedef srmw_fifo_t<vfs_socs_t>      vfs_socloseq_t;   /// A stream-out close queue.
typedef srmw_fifo_t<vfs_siop_t>      vfs_sicommandq_t; /// A stream-in command queue.
typedef srmw_fifo_t<vfs_sics_t>      vfs_sicreateq_t;  /// A stream-in create queue.
typedef srsw_fifo_t<vfs_sird_t, QC>  vfs_siresultq_t;  /// A stream-in result queue.
typedef srsw_fifo_t<void*     , QC>  vfs_sireturnq_t;  /// A stream-in return queue.
typedef srsw_fifo_t<vfs_sies_t, MS>  vfs_siendq_t;     /// A stream-in end-of-stream queue.
#undef  MS
#undef  QC

/// @summary Information that remains constant from the point that a file is opened for stream-in.
struct vfs_siinfo_t
{
    int                Fildes;       /// The file descriptor for the file.
    int                Eventfd;      /// The eventfd descriptor for the file, or -1.
    int64_t            FileSize;     /// The physical file size, in bytes.
    int64_t            DataSize;     /// The file size after any size-changing transforms.
    int64_t            FileOffset;   /// The absolute byte offset of the start of the file data.
    size_t             SectorSize;   /// The disk physical sector size, in bytes.
    int32_t            EndBehavior;  /// The end-of-stream behavior, one of stream_in_mode_e.
};

/// @summary Status information associated with an active stream-in. This information
/// is required to properly process (for example) close operations, where there may be
/// one or more in-progress read operations against the file; in which case the close
/// must be deferred until all in-progress read operations have completed.
struct vfs_sistat_t
{
    uint64_t           NPendingAIO;  /// The number of pending AIO operations on the file.
    uint32_t           StatusFlags;  /// A combination of vfs_stream_in_status_e.
};

/// @summary Defines the data associated with a priority queue of pending AIO operations.
struct vfs_io_opq_t
{
    #define MO         AIO_MAX_ACTIVE
    int32_t            Count;        /// The number of items in the queue.
    uint64_t           InsertionId;  /// The counter for tagging each AIO request.
    uint32_t           Priority[MO]; /// The priority value for each item.
    uint64_t           InsertId[MO]; /// The inserion order value for each item.
    aio_req_t          Request [MO]; /// The populated AIO request for each item.
    #undef MO
};

/// @summary Defines the data associated with a priority queue of files. This queue
/// is used to determine which files get a chance to submit I/O operations.
struct vfs_io_fpq_t
{
    #define MS         MAX_STREAMS_IN
    int32_t            Count;        /// The number of items in the queue.
    uint32_t           Priority[MS]; /// The priority value for each file.
    uint16_t           RecIndex[MS]; /// The index of the file record.
    #undef MS
};

/// @summary Defines the state data maintained by a VFS driver instance.
struct vfs_state_t
{
    #define MD         MAX_DECODER_IN
    #define MS         MAX_STREAMS_IN
    #define NT         FILE_TYPE_COUNT
    vfs_sowriteq_t     StOutWriteQ;  /// Stream-out write operation queue.
    vfs_socloseq_t     StOutCloseQ;  /// Stream-out close operation queue.
    vfs_sicommandq_t   StInCommandQ; /// Stream-in playback command queue.
    vfs_sicreateq_t    StInCreateQ;  /// Stream-in create operation queue.
    iobuf_alloc_t      IoAllocator;  /// The I/O buffer allocator.
    size_t             ActiveCount;  /// The number of active stream-in.
    intptr_t           StInASID[MS]; /// An application-defined ID for each active stream-in.
    int32_t            StInType[MS]; /// One of file_type_e for each active stream-in.
    uint32_t           Priority[MS]; /// The access priority for each active stream-in.
    int64_t            RdOffset[MS]; /// The current read offset for each active stream-in.
    vfs_siinfo_t       StInInfo[MS]; /// The constant data for each active stream-in.
    vfs_sistat_t       StInStat[MS]; /// Pending AIO status for each active stream-in.
    vfs_io_opq_t       IoOperations; /// The priority queue of all pending I/O operations.
    vfs_siresultq_t    SiResult[NT]; /// The per-file type queue for stream-in I/O results.
    vfs_sireturnq_t    SiReturn[NT]; /// The per-file type queue for stream-in I/O buffer returns.
    vfs_siendq_t       SiEndOfS[NT]; /// The per-file type queue for stream-in end-of-stream events.
    size_t             SiDecCount;   /// The number of stream-in with active decode state.
    intptr_t           SiDecASID[MD];/// The stream ID for each active-decode stream-in.
    stream_decoder_t  *SiDecInfo[MD];/// The decode state for each active-decode stream-in.
    #undef NT       // per-file type
    #undef MS       // per-active stream
    #undef MD       // per-active decode
};

/// @summary Statistics tracked by the platform I/O system.
struct io_stats_t
{
    #define NT         FILE_TYPE_COUNT
    uint64_t           NStallsAIOQ;  /// Stalls due to full AIO operation queue.
    uint64_t           NStallsVFSQ;  /// Stalls due to full VFS operation queue.
    uint64_t           NStallsIOBuf; /// Stalls due to exhausted I/O buffer space.
    uint64_t           NStallsFT[NT];/// Stalls due to slow file data processing.
    #undef NT
};

/// @summary Defines the data associated with a file opened for buffered, synchronous I/O.
struct file_t
{
    int                Fildes;       /// The file descriptor for the file.
    int                OpenFlags;    /// The flags passed to open(2).
    struct stat        OpenStats;    /// The data returned by fstat() when the file was opened.
};

/// @summary Defines the state information associated with a writable file. This
/// information is modified only by the thread that calls platform_create_file().
struct stream_writer_t
{
    int                Fildes;       /// The file descriptor for the file.
    int                Eventfd;      /// The eventfd descriptor for the file, or -1.
    uint8_t           *BaseAddress;  /// The base address of the current buffer.
    size_t             DataOffset;   /// The write pointer offset from the start of the buffer.
    int64_t            FileOffset;   /// The absolute offset of the write pointer within the file.
    uint32_t           Priority;     /// The stream priority (0 = highest).
};

/*///////////////
//   Globals   //
///////////////*/
/// @summary A list of all of the file type identifiers we consider to be valid.
global_variable file_type_e FILE_TYPE_LIST[FILE_TYPE_COUNT] = {
    FILE_TYPE_DDS,
    FILE_TYPE_TGA,
    FILE_TYPE_WAV,
    FILE_TYPE_JSON
};

/// @summary A list of printable names for each valid file type identifier.
global_variable char const *FILE_TYPE_NAME[FILE_TYPE_COUNT] = {
    "DDS" , /* FILE_TYPE_DDS  */
    "TGA" , /* FILE_TYPE_TGA  */
    "WAV" , /* FILE_TYPE_WAV  */
    "JSON"  /* FILE_TYPE_JSON */
};

global_variable vfs_state_t VFS_STATE;
global_variable aio_state_t AIO_STATE;
global_variable io_stats_t  IO_STATS;

/*///////////////////////
//   Local Functions   //
///////////////////////*/
/// @summary Rounds a size up to the nearest even multiple of a given power-of-two.
/// @param size The size value to round up.
/// @param pow2 The power-of-two alignment.
/// @return The input size, rounded up to the nearest even multiple of pow2.
internal_function inline size_t align_up(size_t size, size_t pow2)
{
    assert((pow2 & (pow2-1)) == 0);
    return (size == 0) ? pow2 : ((size + (pow2-1)) & ~(pow2-1));
}

/// @summary Rounds a size up to the nearest even multiple of a given power-of-two.
/// @param size The size value to round up.
/// @param pow2 The power-of-two alignment.
/// @return The input size, rounded up to the nearest even multiple of pow2.
internal_function inline int64_t align_up(int64_t size, size_t pow2)
{
    assert((pow2 & (pow2-1)) == 0);
    return (size == 0) ? int64_t(pow2) : ((size + int64_t(pow2-1)) & ~int64_t(pow2-1));
}

/// @summary Clamps a value to a given maximum.
/// @param size The size value to clamp.
/// @param limit The upper-bound to clamp to.
/// @return The smaller of size and limit.
internal_function inline size_t clamp_to(size_t size, size_t limit)
{
    return (size > limit) ? limit : size;
}

/// @summary Reads the current tick count for use as a timestamp.
/// @return The current timestamp value, in nanoseconds.
internal_function inline uint64_t nanotime(void)
{
    struct timespec tsc;
    clock_gettime(CLOCK_MONOTONIC, &tsc);
    return (SEC_TO_NANOSEC * uint64_t(tsc.tv_sec) + uint64_t(tsc.tv_nsec));
}

/// @summary Atomically writes a 32-bit unsigned integer value to a given address.
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be 32-bit aligned.
/// @param value The value to write to address.
internal_function never_inline void atomic_write_uint32_aligned(uintptr_t address, uint32_t value)
{
    assert((address & 0x03) == 0);                  // assert address is 32-bit aligned
    uint32_t *p  = (uint32_t*) address;
    *p = value;
}

/// @summary Atomically writes a pointer-sized value to a given address.
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be aligned to the pointer size.
/// @param value The value to write to address.
internal_function never_inline void atomic_write_pointer_aligned(uintptr_t address, uintptr_t value)
{
    assert((address & (sizeof(uintptr_t)-1)) == 0); // assert address is pointer-size aligned
    uintptr_t *p = (uintptr_t*) address;
    *p = value;
}

/// @summary Atomically reads a 32-bit unsigned integer value from a given address.
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be 32-bit aligned.
/// @return The value read from the specified address.
internal_function never_inline uint32_t atomic_read_uint32_aligned(uintptr_t address)
{
    assert((address & 0x03) == 0);
    volatile uint32_t *p = (uint32_t*) address;
    return (*p);
}

/// @summary Clears or initializes a SRSW fixed lookaside queue to empty.
/// @param srswq The queue to initialize.
/// @param capacity The queue capacity. This must be a power-of-two.
internal_function inline void srsw_flq_clear(srsw_flq_t &srswq, uint32_t capacity)
{
    assert((capacity & (capacity-1)) == 0); // capacity is a power-of-two.
    srswq.PushedCount = 0;
    srswq.PoppedCount = 0;
    srswq.Capacity    = capacity;
}

/// @summary Retrieves the number of items currently available in a SRSW fixed
/// lookaside queue. Do not pop more than the number of items returned by this call.
/// @param srswq The queue to query.
internal_function inline uint32_t srsw_flq_count(srsw_flq_t &srswq)
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
internal_function inline bool srsw_flq_full(srsw_flq_t &srswq)
{
    return (srsw_flq_count(srswq) == srswq.Capacity);
}

/// @summary Checks whether a SRSW fixed lookaside queue is empty. Check this
/// before popping an item from the queue.
/// @param srswq The queue to query.
/// @return true if the queue is empty.
internal_function inline bool srsw_flq_empty(srsw_flq_t &srswq)
{
    return (srsw_flq_count(srswq) == 0);
}

/// @summary Gets the index the next push operation will write to. This must be
/// called only by the producer prior to calling srsw_flq_push().
internal_function inline uint32_t srsw_flq_next_push(srsw_flq_t &srswq)
{
    uintptr_t pushed_cnt_addr = (uintptr_t) &srswq.PushedCount;
    uint32_t  pushed_cnt      = atomic_read_uint32_aligned(pushed_cnt_addr);
    return (pushed_cnt & (srswq.Capacity - 1));
}

/// @summary Implements a push operation in a SRSW fixed lookaside queue. This
/// must be called only from the producer.
/// @param srswq The queue to update.
internal_function inline void srsw_flq_push(srsw_flq_t &srswq)
{
    uintptr_t pushed_cnt_addr = (uintptr_t) &srswq.PushedCount;
    uint32_t  pushed_cnt      = atomic_read_uint32_aligned(pushed_cnt_addr) + 1;
    atomic_write_uint32_aligned(pushed_cnt_addr, pushed_cnt);
}

/// @summary Gets the index the next pop operation will read from. This must be
/// called only by the consumer prior to popping an item from the queue.
internal_function inline uint32_t srsw_flq_next_pop(srsw_flq_t &srswq)
{
    uintptr_t popped_cnt_addr = (uintptr_t) &srswq.PoppedCount;
    uint32_t  popped_cnt      = atomic_read_uint32_aligned(popped_cnt_addr);
    return (popped_cnt & (srswq.Capacity - 1));
}

/// @summary Implements a pop operation in a SRSW fixed lookaside queue. This must
/// be called only from the consumer against a non-empty queue.
/// @param srswq The queue to update.
internal_function inline void srsw_flq_pop(srsw_flq_t &srswq)
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
internal_function inline void flush_srsw_fifo(srsw_fifo_t<T, N> *fifo)
{
    srsw_flq_clear(fifo->Queue, N);
}

/// @summary Retrieves the number of items 'currently' in the queue.
/// @param fifo The queue to query.
/// @return The number of items in the queue at the instant of the call.
template <typename T, uint32_t N>
internal_function inline size_t srsw_fifo_count(srsw_fifo_t<T, N> *fifo)
{
    return srsw_flq_count(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' empty.
/// @param fifo The queue to query.
/// @return true if the queue contains zero items at the instant of the call.
template <typename T, uint32_t N>
internal_function inline bool srsw_fifo_is_empty(srsw_fifo_t<T, N> *fifo)
{
    return srsw_flq_empty(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' full.
/// @param fifo The queue to query.
/// @return true if the queue is full at the instant of the call.
template <typename T, uint32_t N>
internal_function inline bool srsw_fifo_is_full(srsw_fifo_t<T, N> *fifo)
{
    return srsw_flq_full(fifo->Queue);
}

/// @summary Enqueues an item.
/// @param fifo The destination queue.
/// @param item The item to enqueue. This must be a POD type.
/// @return true if the item was enqueued, or false if the queue is at capacity.
template <typename T, uint32_t N>
internal_function inline bool srsw_fifo_put(srsw_fifo_t<T, N> *fifo, T const &item)
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
internal_function inline bool srsw_fifo_get(srsw_fifo_t<T, N> *fifo, T &item)
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
internal_function bool create_srmw_freelist(srmw_freelist_t<T> &list, size_t capacity)
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
internal_function void delete_srmw_freelist(srmw_freelist_t<T> &list)
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
internal_function inline T* srmw_freelist_get(srmw_freelist_t<T> &list)
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
internal_function inline void srmw_freelist_put(srmw_freelist_t<T> &list, T *node)
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
internal_function bool create_srmw_fifo(srmw_fifo_t<T> *fifo, size_t capacity)
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
internal_function void delete_srmw_fifo(srmw_fifo_t<T> *fifo)
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
internal_function bool srmw_fifo_get(srmw_fifo_t<T> *fifo, T &item)
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
internal_function bool srmw_fifo_put(srmw_fifo_t<T> *fifo, T const &item)
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
internal_function bool create_iobuf_allocator(iobuf_alloc_t &alloc, size_t total_size, size_t alloc_size)
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
internal_function void delete_iobuf_allocator(iobuf_alloc_t &alloc)
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
internal_function void flush_iobuf_allocator(iobuf_alloc_t &alloc)
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
internal_function inline void* iobuf_get(iobuf_alloc_t &alloc)
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
internal_function inline void iobuf_put(iobuf_alloc_t &alloc, void *iobuf)
{
    assert(iobuf != NULL);
    alloc.FreeList[alloc.FreeCount++] = iobuf;
}

/// @summary Calaculate the number of bytes currently unused.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of bytes currently available for use by the application.
internal_function inline size_t iobuf_bytes_free(iobuf_alloc_t const &alloc)
{
    return (alloc.AllocSize * alloc.FreeCount);
}

/// @summary Calaculate the number of bytes currently allocated.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of bytes currently in-use by the application.
internal_function inline size_t iobuf_bytes_used(iobuf_alloc_t const &alloc)
{
    return  alloc.TotalSize - (alloc.AllocSize * alloc.FreeCount);
}

/// @summary Calculate the number of buffers currently allocated.
/// @param alloc The I/O buffer allocator to query.
/// @return The number of buffers currently in-use by the application.
internal_function inline size_t iobuf_buffers_used(iobuf_alloc_t const &alloc)
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
internal_function inline int io_opq_cmp_put(vfs_io_opq_t const *pq, uint32_t priority, int32_t idx)
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
internal_function inline int io_opq_cmp_get(vfs_io_opq_t const *pq, int32_t a, int32_t b)
{   // first order by priority. if priority is equal, the operations should
    // appear in the order they were inserted into the queue.
    uint32_t const p_a  = pq->Priority[a];
    uint32_t const p_b  = pq->Priority[b];
    uint64_t const i_a  = pq->InsertId[a];
    uint64_t const i_b  = pq->InsertId[b];
    if (p_a < p_b) return -1;
    if (p_a > p_b) return +1;
    if (i_a < i_b) return -1;
    else           return +1; // i_a > i_b; i_a can never equal i_b.
}

/// @summary Resets a I/O operation priority queue to empty.
/// @param pq The priority queue to clear.
internal_function void io_opq_clear(vfs_io_opq_t *pq)
{
    pq->Count = 0;
}

/// @summary Attempts to insert an I/O operation in the priority queue.
/// @param pq The I/O operation priority queue to update.
/// @param priority The priority value associated with the item being inserted.
/// @return The AIO request to populate, or NULL if the queue is full.
internal_function aio_req_t* io_opq_put(vfs_io_opq_t *pq, uint32_t priority)
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
internal_function inline bool io_opq_top(vfs_io_opq_t *pq, aio_req_t &request)
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
internal_function bool io_opq_get(vfs_io_opq_t *pq, aio_req_t &request)
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
            uint64_t  temp_i     = pq->InsertId[pos];
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
internal_function void io_fpq_clear(vfs_io_fpq_t *pq)
{
    pq->Count = 0;
}

/// @summary Attempts to insert a file into the file priority queue.
/// @param pq The priority queue to update.
/// @param priority The priority value associated with the item being inserted.
/// @param index The zero-based index of the file record being inserted.
/// @return true if the item was inserted in the queue, or false if the queue is full.
internal_function bool io_fpq_put(vfs_io_fpq_t *pq, uint32_t priority, uint16_t index)
{
    if (pq->Count < MAX_STREAMS_IN)
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
internal_function bool io_fpq_get(vfs_io_fpq_t *pq, uint16_t &index, uint32_t &priority)
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
            if (pq->Priority[pos] < pq->Priority[m])
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

/// @summary Find the end of a volume and directory information portion of a path.
/// @param path The path string to search.
/// @param out_pathlen On return, indicates the number of bytes in the volume and
/// directory information of the path string. If the input path has no volume or
/// directory information, this value will be set to zero.
/// @param out_strlen On return, indicates the number of bytes in the input path,
/// not including the trailing zero byte.
/// @return A pointer to one past the last volume or directory separator, if present;
/// otherwise, the input pointer path.
internal_function char const* pathend(char const *path, size_t &out_pathlen, size_t &out_strlen)
{
    if (path == NULL)
    {
        out_pathlen = 0;
        out_strlen  = 0;
        return path;
    }

    char        ch   = 0;
    char const *last = path;
    char const *iter = path;
    while ((ch = *iter++) != 0)
    {
        if (ch == ':' || ch == '\\' || ch == '/')
            last = iter;
    }
    out_strlen  = size_t(iter - path - 1);
    out_pathlen = size_t(last - path);
    return last;
}

/// @summary Find the extension part of a filename or path string.
/// @param path The path string to search; ideally just the filename portion.
/// @param out_extlen On return, indicates the number of bytes of extension information.
/// @return A pointer to the first character of the extension. Check the value of
/// out_extlen to be sure that there is extension information.
internal_function char const* extpart(char const *path, size_t &out_extlen)
{
    if (path == NULL)
    {
        out_extlen = 0;
        return path;
    }

    char        ch    = 0;
    char const *last  = path;
    char const *iter  = path;
    while ((ch = *iter++) != 0)
    {
        if (ch == '.')
            last = iter;
    }
    if (last != path)
    {   // we found an extension separator somewhere in the input path.
        // @note: this also filters out the case of ex. path = '.gitignore'.
        out_extlen = size_t(iter - last - 1);
    }
    else
    {
        // no extension part is present in the input path.
        out_extlen = 0;
    }
    return last;
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
internal_function bool open_file_raw(char const *path, int flags, int &fd, int &efd, int64_t &file_size, size_t &sector_size)
{
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
internal_function void close_file_raw(int &fd, int &efd)
{
    if (efd != -1) close(efd);
    if (fd  != -1) close(fd);
    fd  = -1;
    efd = -1;
}

/// @summary Resets the platform I/O statistics to zero.
/// @param stats The counters to reset.
internal_function void init_io_stats(io_stats_t *stats)
{
    if (stats != NULL)
    {
        stats->NStallsAIOQ   = 0;
        stats->NStallsVFSQ   = 0;
        stats->NStallsIOBuf  = 0;
        for (size_t i = 0; i < FILE_TYPE_COUNT; ++i)
        {
            stats->NStallsFT[i] = 0;
        }
    }
}

/// @summary Allocates an iocb instance from the free list.
/// @param aio The AIO driver state managing the free list.
/// @return The next available iocb structure.
internal_function inline struct iocb* iocb_get(aio_state_t *aio)
{
    assert(aio->IOCBFreeCount > 0);
    return aio->IOCBFree[--aio->IOCBFreeCount];
}

/// @summary Returns an iocb instance to the free list.
/// @param aio The AIO driver state managing the free list.
/// @param iocb The IOCB to return to the free list.
internal_function inline void iocb_put(aio_state_t *aio, struct iocb *iocb)
{
    assert(aio->IOCBFreeCount < AIO_MAX_ACTIVE);
    aio->IOCBFree[aio->IOCBFreeCount++] = iocb;
}

/// @summary Helper function to build an AIO result packet.
/// @param error The error code to return.
/// @param amount The amount of data returned.
/// @param req The request associated with the result.
/// @return The populated AIO result packet.
internal_function inline aio_res_t aio_result(int error, uint32_t amount, aio_req_t const &req)
{
    aio_res_t res = {
        req.Fildes,      /* Fildes     */
        req.Eventfd,     /* Eventfd    */
        error,           /* OSError    */
        amount,          /* DataAmount */
        req.FileOffset,  /* FileOffset */ /* the relative offset */
        req.DataBuffer,  /* DataBuffer */
        req.QTimeNanos,  /* QTimeNanos */
        nanotime(),      /* CTimeNanos */
        req.AFID,        /* AFID       */
        req.Type,        /* Type       */
        0                /* Reserved   */
    };
    return res;
}

/// @summary Builds a read operation IOCB and submits it to kernel AIO.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the read operation.
/// @param error On return, this location stores the error return value.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
internal_function int aio_submit_read(aio_state_t *aio, aio_req_t const &req, int &error)
{
    int64_t absolute_ofs = req.BaseOffset + req.FileOffset; // relative->absolute
    struct iocb    *iocb = iocb_get(aio);    // allocate from the free list
    iocb->data           = (void*)req.AFID;  // for sanity checking on completion
    iocb->aio_lio_opcode = IO_CMD_PREAD;     // we're reading from the file
    iocb->aio_fildes     = req.Fildes;       // the file descriptor to read from
    iocb->u.c.buf        = req.DataBuffer;   // the buffer to read into
    iocb->u.c.nbytes     = req.DataAmount;   // the maximum number of bytes to read
    iocb->u.c.offset     = absolute_ofs;     // the absolute byte offset of the write location
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
        aio_res_t r =  aio_result(error, 0, req);
        srsw_fifo_put(&aio->ReadResults, r);
        return (-1);
    }
}

/// @summary Builds a write operation IOCB and submits it to kernel AIO.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the write operation.
/// @param error On return, this location stores the error return value.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
internal_function int aio_submit_write(aio_state_t *aio, aio_req_t const &req, int &error)
{
    int64_t absolute_ofs = req.BaseOffset + req.FileOffset; // relative->absolute
    struct iocb    *iocb = iocb_get(aio);    // allocate from the free list
    iocb->data           = (void*)req.AFID;  // for sanity checking on completion
    iocb->aio_lio_opcode = IO_CMD_PWRITE;    // we're writing to the the file
    iocb->aio_fildes     = req.Fildes;       // the file descriptor to write to
    iocb->u.c.buf        = req.DataBuffer;   // the buffer to read from
    iocb->u.c.nbytes     = req.DataAmount;   // the number of bytes to write
    iocb->u.c.offset     = absolute_ofs;     // the absolute byte offset of the write location
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
        aio_res_t r =  aio_result(error , 0, req);
        srsw_fifo_put(&aio->WriteResults, r);
        return (-1);
    }
}

/// @summary Builds an fsync operation IOCB and submits it to kernel AIO.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the flush operation.
/// @param error On return, this location stores the error return value.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
internal_function int aio_submit_fsync(aio_state_t *aio, aio_req_t const &req, int &error)
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
        aio_res_t r =  aio_result(error , 0, req);
        srsw_fifo_put(&aio->WriteResults, r);
        return (-1);
    }
}

/// @summary Builds an fdatasync operation IOCB and submits it to kernel AIO.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the flush operation.
/// @param error On return, this location stores the error return value.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
internal_function int aio_submit_fdsync(aio_state_t *aio, aio_req_t const &req, int &error)
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
        aio_res_t r =  aio_result(error , 0, req);
        srsw_fifo_put(&aio->WriteResults, r);
        return (-1);
    }
}

/// @summary Synchronously processes a file close operation.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the close operation.
/// @return Zero if the result was successfully submitted, or -1 if the result queue is full.
internal_function int aio_process_close(aio_state_t *aio, aio_req_t const &req)
{   // close the file descriptors associated with the file.
    if (req.Eventfd != -1) close(req.Eventfd);
    if (req.Fildes  != -1) close(req.Fildes);

    // generate the completion result and push it to the queue.
    aio_res_t res = aio_result(0, 0, req);
    return srsw_fifo_put(&aio->CloseResults, res) ? 0 : -1;
}

/// @summary Synchronously processes a finalize request, which closes the temp
/// file and safely moves it to the destination file path.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the finalize operation.
/// @return Zero if the result was successfully submitted, or -1 if the result queue is full.
internal_function int aio_process_finalize(aio_state_t *aio, aio_req_t const &req)
{
    struct stat st;                        // results of lstat; st_size gives the path length.
    ssize_t nsrc =  0;                     // the number of bytes read by readlink().
    char *target = (char*) req.DataBuffer; // either NULL, or allocated by strdup().
    char *source =  NULL;                  // allocated with malloc().
    char fdpath[32];                       // the path /proc/self/fd/########.

    // all we have is a file descriptor. in order to use rename() or unlink(),
    // the file descriptor must be translated into a source path.
    snprintf (fdpath, 32, "/proc/self/fd/%d", req.Fildes);
    if (lstat(fdpath,&st) < 0)
    {   // couldn't retrieve the length of the source path. check errno.
        goto error_cleanup;
    }
    // allocate storage for the source path.
    if ((source = (char*) malloc(st.st_size + 1)) == NULL)
    {   // unable to malloc() enough space. check errno.
        goto error_cleanup;
    }
    // zero the buffer, and read the source path from the symlink.
    memset(source, 0, st.st_size + 1);
    if ((nsrc = readlink(fdpath, source, st.st_size + 1)) == -1)
    {   // unable to read the source path from the symlink.
        goto error_cleanup;
    }
    source[st.st_size] = 0;

    // handle the simple case of deleting the temp file.
    if (target == NULL)
    {   // use unlink() to delete the temporary file.
        if (unlink(source) == -1)
        {   // the file could not be deleted. check errno.
            goto error_cleanup;
        }
        srsw_fifo_put(&aio->CloseResults, aio_result(0, 0, req));
        if (req.Eventfd != -1) close(req.Eventfd);
        if (req.Fildes != -1) close(req.Fildes);
        if (source != NULL) free(source);
        return 0;
    }

    // we're saving the file, so save off the current EOF position.
    // we'll use this to set the 'real' size of the file.
    if (req.Eventfd != -1) close(req.Eventfd);
    if (req.Fildes != -1) close(req.Fildes);
    if (truncate(source, req.FileOffset) == -1)
    {   // can't just goto error_cleanup; we've already closed the fd's.
        srsw_fifo_put(&aio->CloseResults, aio_result(errno, 0, req));
        free(source);  free(target);
        return -1;
    }

    // use rename() to move the temp file to the target path.
    if (rename(source, target) == -1)
    {   // can't just goto error_cleanup; we've already closed the fd's.
        srsw_fifo_put(&aio->CloseResults, aio_result(errno, 0, req));
        free(source);  free(target);
        return -1;
    }

    // finally, we're done. complete the operation successfully.
    free(source); free(target);
    return srsw_fifo_put(&aio->CloseResults, aio_result(0, 0, req)) ? 0 : -1;

error_cleanup:
    srsw_fifo_put(&aio->CloseResults, aio_result(errno, 0, req));
    if (req.Eventfd != -1) close(req.Eventfd);
    if (req.Fildes != -1) close(req.Fildes);
    if (source != NULL) free(source);
    if (target != NULL) free(target);
    return -1;
}

/// @summary Implements the main loop of the AIO driver using a polling mechanism.
/// @param aio The AIO driver state to update.
/// @param timeout The timeout value indicating the amount of time to wait, or
/// NULL to block indefinitely. Note that aio_poll() just calls aio_tick() with
/// a timeout of zero, which will return immediately if no events are available.
/// @return Zero to continue with the next tick, 1 if the shutdown signal was received, -1 if an error occurred.
internal_function int aio_tick(aio_state_t *aio, struct timespec *timeout)
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
                int       err = evt.res <  0 ? -evt.res : 0;
                uint32_t  amt = evt.res >= 0 ?  evt.res : 0;
                aio_req_t req = aio->AAIOList[idx]; // make a copy of the request
                aio_res_t res = aio_result(err, amt, req);

                // swap the last active request into this slot.
                aio->AAIOList[idx] = aio->AAIOList[nlive-1];
                aio->IOCBList[idx] = aio->IOCBList[nlive-1];
                aio->ActiveCount--;

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
            else
            {   // TODO: should track this statistic somewhere.
                // this should not happen.
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
                // fdsync() flushes file data only, not usually metadata.
                result = aio_submit_fsync(aio, req, error);
                break;
            case AIO_COMMAND_CLOSE:
                result = aio_process_close(aio, req);
                break;
            case AIO_COMMAND_FINAL:
                result = aio_process_finalize(aio, req);
                break;
            default:
                error  = EINVAL;
                break;
        }
    }
    return 0;
}

/// @summary Implements the main loop of the AIO driver.
/// @param aio The AIO driver state to update.
internal_function inline void aio_poll(aio_state_t *aio)
{   // configure a zero timeout so we won't block.
    struct timespec timeout;
    timeout.tv_sec  = 0;
    timeout.tv_nsec = 0;
    aio_tick(aio , &timeout);
}

/// @summary Allocates a new AIO context and initializes the AIO state.
/// @param aio The AIO state to allocate and initialize.
/// @return 0 if the operation completed successfully; otherwise, the errno value.
internal_function int create_aio_state(aio_state_t *aio)
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
internal_function void delete_aio_state(aio_state_t *aio)
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

/// @summary Searches the VFS driver state to determine the current index of an
/// active stream-in given the stream application-defined ID.
/// @param vfs The VFS driver state to search.
/// @param asid The application-defined stream ID to locate.
/// @param index On return, this value is set to the zero-based index of the
/// current slot in the active stream list associated with the input ASID.
/// @return true if the ASID was located in the list.
internal_function inline bool vfs_find_by_asid(vfs_state_t const *vfs, intptr_t asid, size_t &index)
{
    intptr_t const  ASID      = asid;
    intptr_t const *ASIDList  = vfs->StInASID;
    size_t   const  ASIDCount = vfs->ActiveCount;
    for (size_t i = 0; i < ASIDCount; ++i)
    {
        if (ASIDList[i] == ASID)
        {
            index = i;
            return true;
        }
    }
    return false;
}

/// @summary Searches the VFS driver state to determine the current index of an
/// active stream-in decoder given the stream application-defined ID.
/// @param vfs The VFS driver state to search.
/// @param asid The application-defined stream ID to locate.
/// @param index On return, this value is set to the zero-based index of the
/// current slot in the active stream list associated with the input ASID.
/// @return true if the ASID was located in the list.
internal_function inline bool vfs_decoder_by_asid(vfs_state_t const *vfs, intptr_t asid, size_t &index)
{
    intptr_t const  ASID      = asid;
    intptr_t const *ASIDList  = vfs->SiDecASID;
    size_t   const  ASIDCount = vfs->SiDecCount;
    for (size_t i = 0; i < ASIDCount; ++i)
    {
        if (ASIDList[i] == ASID)
        {
            index = i;
            return true;
        }
    }
    return false;
}

/// @summary Check whether a give file descriptor references a file on a remote mount point.
/// @param fd The file descriptor to check.
/// @return true if the file exists on a remote mount point.
internal_function bool is_remote(int fd)
{
    struct statfs    st;
    if (fstatfs(fd, &st) < 0)
    {   // unable to stat the file.
        return false;
    }
    switch (st.f_type)
    {
        case 0xFF534D42: // CIFS_MAGIC_NUMBER: /* 0xFF534D42 */
        case 0x00006969: // NFS_MAGIC_NUMBER:  /* 0x00006969 */
            return true;
        default:
            break;
    }
    return false;
}

/// @summary Determine whether a path references a file within an archive, (and
/// if so, which one) or whether it references a native file. Open the file if
/// necessary, and return basic file information to the caller. This function
/// should only be used for read-only files, files cannot be written in an archive.
/// @param path The NULL-terminated UTF-8 path of the file to resolve.
/// @param hints A combination of vfs_file_hint_e to control how the file is opened.
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
/// @param decoder On return, points to the initialized decoder instance used to decode
/// the data. The specific decoder type depends on the source file type.
/// @return true if the file could be resolved.
internal_function bool vfs_resolve_file_read(char const *path, int hints, int &fd, int &efd, int64_t &lsize, int64_t &psize, int64_t &offset, size_t &sector_size, stream_decoder_t *&decoder)
{   // TODO: determine whether this path references a file contained within an archive.
    // for now, we only handle native file paths, which may be absolute or relative.
    bool native_path = true;
    if  (native_path)
    {
        int flags = O_RDONLY | O_LARGEFILE;
        if (open_file_raw(path, flags, fd, efd, psize, sector_size))
        {   // native files always begin at the first byte.
            // logical and physical size are the same.
            if (((hints & FILE_HINT_DIRECT) != 0 || (psize > VFS_DIRECT_IO_THRESHOLD)) && is_remote(fd) == false)
            {   // fcntl O_DIRECT if buffer_hint is false or the file exceeds a certain size.
                // if this fails, we will fall back to buffered I/O; it is not a fatal error.
                // never use O_DIRECT for files mounted with NFS or CIFS; these don't read correctly.
                fcntl(fd, F_SETFL, O_RDONLY | O_LARGEFILE | O_DIRECT);
            }
            decoder = new stream_decoder_t();
            lsize   = psize;
            offset  = 0;
            return true;
        }
        else
        {   // unable to open the file, so fail immediately.
            fd = efd = -1; lsize = psize = offset = sector_size = 0;
            decoder = NULL;
            return false;
        }
    }
    return false;
}

/// @summary Processes queued commands for creating a new stream-in.
/// @param vfs The VFS driver state.
internal_function void vfs_process_sicreate(vfs_state_t *vfs)
{
    while (vfs->ActiveCount < MAX_STREAMS_IN)
    {
        vfs_sics_t    req;
        if (srmw_fifo_get(&vfs->StInCreateQ, req) == false)
        {   // there are no pending create requests, so we're done.
            break;
        }

        // the file is already open; it was opened during platform_open_stream().
        // all we need to do is update our internal active file list.
        size_t indexa = vfs->ActiveCount++;
        vfs->StInASID[indexa]             = req.ASID;
        vfs->StInType[indexa]             = req.Type;
        vfs->Priority[indexa]             = req.Priority;
        vfs->RdOffset[indexa]             = 0;
        vfs->StInInfo[indexa].Fildes      = req.Fildes;
        vfs->StInInfo[indexa].Eventfd     = req.Eventfd;
        vfs->StInInfo[indexa].FileSize    = req.FileSize;
        vfs->StInInfo[indexa].DataSize    = req.DataSize;
        vfs->StInInfo[indexa].FileOffset  = req.FileOffset;
        vfs->StInInfo[indexa].SectorSize  = req.SectorSize;
        vfs->StInInfo[indexa].EndBehavior = req.Behavior;
        vfs->StInStat[indexa].NPendingAIO = 0;
        vfs->StInStat[indexa].StatusFlags = VFS_STATUS_NONE;

        // save the stream decoder state, used when processing read data.
        size_t indexd = vfs->SiDecCount++;
        vfs->SiDecASID[indexd] = req.ASID;
        vfs->SiDecInfo[indexd] = req.Decoder;
    }
}

/// @summary Attempts to queue a file close request for the AIO driver.
/// @param vfs The VFS driver state to update.
/// @param i The zero-based index of the active file to close.
/// @return The zero-based index of the next record to check.
internal_function size_t vfs_queue_close(vfs_state_t *vfs, size_t i)
{
    if (vfs->StInStat[i].NPendingAIO > 0)
    {   // there are pending AIO operations against this file.
        // the file cannot be closed until all operations have completed.
        // the file close will be queued after the last operation completes.
        vfs->StInStat[i].StatusFlags |= VFS_STATUS_CLOSE;
        return (i + 1);
    }

    // queue the file close operation for the AIO driver.
    aio_req_t *aio_req = io_opq_put(&vfs->IoOperations, vfs->Priority[i]);
    if (aio_req != NULL)
    {   // fill out the request. it will be processed at a later time.
        aio_req->Command    = AIO_COMMAND_CLOSE;
        aio_req->Fildes     = vfs->StInInfo[i].Fildes;
        aio_req->Eventfd    = vfs->StInInfo[i].Eventfd;
        aio_req->DataAmount = 0;
        aio_req->BaseOffset = vfs->StInInfo[i].FileOffset;
        aio_req->FileOffset = 0;
        aio_req->DataBuffer = NULL;
        aio_req->QTimeNanos = nanotime();
        aio_req->ATimeNanos = 0;
        aio_req->AFID       = vfs->StInASID[i];
        aio_req->Type       = vfs->StInType[i];
        aio_req->Reserved   = 0;

        // delete the decoder state for the file immediately.
        // it is possible that we will get completed reads for the
        // stream after the close command is received, but if the
        // caller has closed the stream, it is assumed that any
        // outstanding data is not wanted and will be discarded.
        size_t      indexd  = 0;
        size_t const lastd  = vfs->SiDecCount - 1;
        if (vfs_decoder_by_asid(vfs, vfs->StInASID[i], indexd))
        {   // free resources associated with the decoder state.
            delete vfs->SiDecInfo[indexd];
            vfs->SiDecInfo[indexd] = NULL;
            // swap the last decoder info into slot 'index'.
            vfs->SiDecASID[indexd] = vfs->SiDecASID[lastd];
            vfs->SiDecInfo[indexd] = vfs->SiDecInfo[lastd];
            vfs->SiDecCount = lastd;
        }

        // delete the file from our internal state immediately.
        size_t const lasta  = vfs->ActiveCount - 1;
        vfs->StInASID[i]    = vfs->StInASID[lasta];
        vfs->StInType[i]    = vfs->StInType[lasta];
        vfs->Priority[i]    = vfs->Priority[lasta];
        vfs->RdOffset[i]    = vfs->RdOffset[lasta];
        vfs->StInInfo[i]    = vfs->StInInfo[lasta];
        vfs->ActiveCount    = lasta;
        return i;
    }
    else
    {   // there's no more space in the pending I/O operation queue.
        // we'll try closing the file again when there's space.
        vfs->StInStat[i].StatusFlags |= VFS_STATUS_CLOSE;
        return (i + 1);
    }
}

/// @summary Processes any pending file close requests.
/// @param vfs The VFS driver state.
internal_function void vfs_process_closes(vfs_state_t *vfs)
{
    for (size_t i = 0; i < vfs->ActiveCount; )
    {
        if (vfs->StInStat[i].StatusFlags & VFS_STATUS_CLOSE)
        {   // there's a pending close operation against this file.
            // vfs_queue_close() returns the next index to check;
            // either i (if a close was queued), or i + 1.
            // note that VFS_STATUS_CLOSE is never explicitly cleared,
            // as when the close is queued, the record is overwritten.
            i = vfs_queue_close(vfs, i);
        }
        else i++; // no pending close; check the next file.
    }
}

/// @summary Processes all completed file close notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
internal_function void vfs_process_completed_closes(vfs_state_t *vfs, aio_state_t *aio)
{
    aio_res_t   res;
    while (srsw_fifo_get(&aio->CloseResults, res))
    {
        /* empty */
    }
}

/// @summary Processes all completed file read notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
internal_function void vfs_process_completed_reads(vfs_state_t *vfs, aio_state_t *aio)
{
    aio_res_t res;
    size_t    index = 0;
    while (srsw_fifo_get(&aio->ReadResults, res))
    {   // locate the stream decoder state so we can pass it on to the platform layer.
        if (vfs_decoder_by_asid(vfs, res.AFID, index) == false)
        {   // the stream has been stopped; don't report the data.
            if (vfs_find_by_asid(vfs, res.AFID, index))
            {   // decrement the number of pending I/O operations.
                vfs->StInStat[index].NPendingAIO--;
            }
            continue;
        }

        vfs_sird_t read;
        // convert the AIO result into something useful for the platform layer.
        read.ASID        = res.AFID;
        read.DataBuffer  = res.DataBuffer;
        read.FileOffset  = res.FileOffset; // this is the relative offset
        read.DataAmount  = res.DataAmount;
        read.OSError     = res.OSError;
        read.Decoder     = vfs->SiDecInfo[index];
        if (srsw_fifo_put(&vfs->SiResult[res.Type], read))
        {   // a single read operation has completed.
            if (vfs_find_by_asid(vfs, res.AFID, index))
            {   // decrement the number of pending I/O operations.
                vfs->StInStat[index].NPendingAIO--;
            }
        }
        else
        {   // TODO: track this statistic somewhere.
            // This should not be happening.
        }
    }
}

/// @summary Processes all completed file write notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
internal_function void vfs_process_completed_writes(vfs_state_t *vfs, aio_state_t *aio)
{
    aio_res_t res;
    while (srsw_fifo_get(&aio->WriteResults, res))
    {   // no need to return anything to the platform layer.
        if (res.DataBuffer != NULL)
        {   // free the fixed-size write buffer.
            munmap(res.DataBuffer, VFS_WRITE_SIZE);
        }
    }
}

/// @summary Processes all completed file flush notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
internal_function void vfs_process_completed_flushes(vfs_state_t *vfs, aio_state_t *aio)
{
    aio_res_t res;
    // there's nothing that the VFS driver needs to do here for the application.
    while (srsw_fifo_get(&aio->FlushResults, res))
    {
        /* empty */
    }
}

/// @summary Processes all pending buffer returns and releases memory back to the pool.
/// @param vfs The VFS driver state.
internal_function void vfs_process_buffer_returns(vfs_state_t *vfs)
{
    for (size_t i = 0; i < FILE_TYPE_COUNT; ++i)
    {
        void            *buffer;
        vfs_sireturnq_t *returnq   = &vfs->SiReturn[i];
        iobuf_alloc_t   &allocator =  vfs->IoAllocator;
        while (srsw_fifo_get(returnq, buffer))
        {
            iobuf_put(allocator, buffer);
        }
    }
}

/// @summary Updates the status of all active input streams, and submits I/O operations.
/// @param vfs The VFS driver state.
/// @return true if the tick should continue submitting I/O operations, or false if
/// either buffer space is full or the I/O operation queue is full.
internal_function bool vfs_update_stream_in(vfs_state_t *vfs)
{
    iobuf_alloc_t &allocator = vfs->IoAllocator;
    size_t const read_amount = allocator.AllocSize;
    size_t const file_count  = vfs->ActiveCount;
    uint32_t     priority    = 0;
    uint16_t     index       = 0;
    vfs_io_fpq_t file_queue;
    vfs_siop_t   op;

    // process any pending stream-in control operations.
    while (srmw_fifo_get(&vfs->StInCommandQ, op))
    {
        size_t i = 0;
        if (vfs_find_by_asid(vfs, op.ASID, i))
        {   // the stream is currently in the active list, so process the control.
            switch (op.OpId)
            {
                case STREAM_IN_PAUSE:
                    vfs->StInStat[i].StatusFlags |=  VFS_STATUS_PAUSE;
                    vfs->StInStat[i].StatusFlags &=~(VFS_STATUS_CLOSE);
                    break;
                case STREAM_IN_RESUME:
                    vfs->StInStat[i].StatusFlags &=~(VFS_STATUS_PAUSE | VFS_STATUS_CLOSE);
                    break;
                case STREAM_IN_REWIND:
                    vfs->StInStat[i].StatusFlags &=~(VFS_STATUS_PAUSE | VFS_STATUS_CLOSE);
                    vfs->RdOffset[i] = 0;
                    break;
                case STREAM_IN_SEEK:
                    if ((op.Argument & (vfs->StInInfo[i].SectorSize-1)) != 0)
                    {   // round up to the nearest sector size multiple.
                        // then, subtract the sector size to get the next lowest multiple.
                        op.Argument  = align_up(op.Argument, vfs->StInInfo[i].SectorSize);
                        op.Argument -= vfs->StInInfo[i].SectorSize;
                    }
                    vfs->StInStat[i].StatusFlags &=~(VFS_STATUS_PAUSE | VFS_STATUS_CLOSE);
                    vfs->RdOffset[i] = op.Argument;
                    break;
                case STREAM_IN_STOP:
                    vfs->StInStat[i].StatusFlags |= VFS_STATUS_CLOSE;
                    break;
            }
        }
    }

    // build a priority queue of files, and then process them one at a time
    // starting with the highest-priority file. the goal here is to fill up
    // the queue of pending I/O operations and stay maximally busy.
    io_fpq_clear(&file_queue);
    for (size_t i = 0; i < file_count; ++i)
    {
        if (vfs->StInStat[i].StatusFlags == VFS_STATUS_NONE)
        {   // only update streams that are active (not paused, not closed.)
            io_fpq_put(&file_queue, vfs->Priority[i], uint16_t(i));
        }
    }
    while(io_fpq_get(&file_queue, index, priority))
    {   // we want to submit as many sequential reads against the file as
        // possible for maximum efficiency. these operations will be
        // processed in-order, so this minimizes seeking as much as possible.
        // stop submitting operations for this file under these conditions:
        // 1. we've reached the end of the file data. continue with the next file.
        // 2. we've run out of pending queue space. stop processing for the tick.
        // 3. we've run out of I/O buffer space. stop processing for the tick.
        size_t nqueued = 0;
        while (iobuf_bytes_free(allocator) > 0)
        {   // allocate a new request in our internal operation queue.
            aio_req_t *req  = io_opq_put(&vfs->IoOperations, priority);
            if (req != NULL)
            {   // populate the (already queued) request.
                req->Command    = AIO_COMMAND_READ;
                req->Fildes     = vfs->StInInfo[index].Fildes;
                req->Eventfd    = vfs->StInInfo[index].Eventfd;
                req->DataAmount = uint32_t(read_amount);
                req->BaseOffset = vfs->StInInfo[index].FileOffset;
                req->FileOffset = vfs->RdOffset[index];
                req->DataBuffer = iobuf_get(allocator);
                req->QTimeNanos = nanotime();
                req->ATimeNanos = 0;
                req->AFID       = vfs->StInASID[index];
                req->Type       = vfs->StInType[index];
                req->Reserved   = 0;
                nqueued++;

                // update the byte offset to the next read.
                int64_t newofs  = vfs->RdOffset[index] + read_amount;
                vfs->RdOffset[index] = newofs;
                if (newofs >= vfs->StInInfo[index].FileSize)
                {   // reached or passed end-of-file.
                    // continue processing the next file.
                    break;
                }
            }
            // we ran out of I/O queue space; no point in continuing.
            // TODO: track this statistic somewhere, we want to know
            // how often this happens.
            else return false;
        }

        // update the number of pending AIO operations against the file.
        // this value is decremented as operations are completed.
        vfs->StInStat[index].NPendingAIO += nqueued;

        if (vfs->RdOffset[index] >= vfs->StInInfo[index].FileSize)
        {   // the end-of-stream notification gives the listener a chance to
            // take some appropriate action. we could handle it automatically,
            // but for some usages we might do the wrong thing - for example,
            // some data is at the end of the stream, but after reading it,
            // the application wants to seek to some other offset and resume
            // instead of closing - neither STREAM_IN_ONCE and STREAM_IN_LOOP fit.
            // the stream remains paused until a STOP, SEEK or REWIND is received.
            if ((vfs->StInStat[index].StatusFlags & VFS_STATUS_PAUSE) == 0)
            {
                vfs_sies_t eos = {
                    vfs->StInASID[index],
                    vfs->StInInfo[index].EndBehavior
                };
                srsw_fifo_put(&vfs->SiEndOfS[vfs->StInType[index]], eos);
                vfs->StInStat[index].StatusFlags |= VFS_STATUS_PAUSE;
            }
        }
        else if (nqueued == 0)
        {   // we ran out of I/O buffer space; there's no point in continuing.
            // TODO: track this statistic somewhere.
            return false;
        }
    }
    return true;
}

/// @summary Processes as many pending stream-out operations as possible.
/// @param vfs The VFS driver state to update.
/// @return true if the tick should continue submitting I/O operations, or false if
/// the I/O operation queue is full.
internal_function bool vfs_update_stream_out(vfs_state_t *vfs)
{
    vfs_sowr_t write;
    while (srmw_fifo_get(&vfs->StOutWriteQ, write))
    {   // allocate a new request in our internal operation queue.
        aio_req_t *req  = io_opq_put(&vfs->IoOperations, write.Priority);
        if (req != NULL)
        {   // populate the (already queued) request.
            req->Command    = AIO_COMMAND_WRITE;
            req->Fildes     = write.Fildes;
            req->Eventfd    = write.Eventfd;
            req->DataAmount = write.DataSize;
            req->BaseOffset = 0;
            req->FileOffset = write.FileOffset;
            req->DataBuffer = write.DataBuffer;
            req->QTimeNanos = nanotime();
            req->ATimeNanos = 0;
            req->AFID       = write.Fildes;
            req->Type       = 0;
            req->Reserved   = 0;
        }
        else
        {   // the internal operation queue is full.
            // put the write back in the queue, and return.
            srmw_fifo_put(&vfs->StOutWriteQ, write);
            return false;
        }
    }

    vfs_socs_t close;
    while (srmw_fifo_get(&vfs->StOutCloseQ, close))
    {   // allocate a new request in our internal operation queue.
        aio_req_t *req  = io_opq_put(&vfs->IoOperations, write.Priority);
        if (req != NULL)
        {   // populate the (already queued) request.
            req->Command    = AIO_COMMAND_FINAL;
            req->Fildes     = close.Fildes;
            req->Eventfd    = close.Eventfd;
            req->DataAmount = 0;
            req->BaseOffset = 0;
            req->FileOffset = close.FileSize;
            req->DataBuffer = NULL;
            req->QTimeNanos = nanotime();
            req->ATimeNanos = 0;
            req->AFID       = close.Fildes;
            req->Type       = 0;
            req->Reserved   = 0;
        }
        else
        {   // the internal operation queue is full.
            // put the close back in the queue, and return.
            srmw_fifo_put(&vfs->StOutCloseQ, close);
            return false;
        }
    }
    return true;
}

/// @summary Implements the main body of the VFS update loop, which processes
/// requests from the application layer, submits I/O requests to the AIO driver,
/// and dispatches completion notifications from the AIO layer back to the application.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
/// @param stats Optional VFS and AIO counters. May be NULL.
internal_function void vfs_tick(vfs_state_t *vfs, aio_state_t *aio, io_stats_t *stats)
{
    io_stats_t null_stats;
    if (stats == NULL)
    {   // prevent everything from constantly having to NULL-check this.
        init_io_stats(&null_stats);
        stats = &null_stats;
    }

    // free up as much buffer state as possible.
    vfs_process_buffer_returns(vfs);

    // generate read and write I/O operations. this increments the number of
    // pending I/O operations across the set of active files.
    vfs_update_stream_in(vfs);
    vfs_update_stream_out(vfs);

    // we're done generating operations, so push as much as possible to AIO.
    aio_req_t request;
    while (io_opq_top(&vfs->IoOperations, request))
    {   // we were able to retrieve an operation from our internal queue.
        if (srsw_fifo_put(&aio->RequestQueue, request))
        {   // we were able to push it to AIO, so remove it from our queue.
            io_opq_get(&vfs->IoOperations, request);
        }
    }

    // dispatch any completed I/O operations to the per-type queues for
    // processing by the platform layer and dispatching to the application.
    // this decrements the number of pending I/O operations across the file set.
    vfs_process_completed_reads  (vfs, aio);
    vfs_process_completed_writes (vfs, aio);
    vfs_process_completed_flushes(vfs, aio);
    vfs_process_completed_closes (vfs, aio);

    // close file requests should be processed after all read and write requests.
    // this ensures that all I/O has been submitted before closing the file.
    // files with pending I/O will not be closed until the I/O completes.
    vfs_process_closes(vfs);

    // open file requests should be processed after all close requests.
    // this increases the likelyhood that we'll have open file slots.
    vfs_process_sicreate(vfs);
}

/// @summary Returns an I/O buffer to the pool. This function should be called
/// for every read result that the platform layer dequeues.
/// @param vfs The VFS state that posted the I/O result.
/// @param type One of file_type_e indicating the type of file being processed.
/// @param buffer The buffer to return. This value may be NULL.
internal_function void vfs_return_buffer(vfs_state_t *vfs, int32_t type, void *buffer)
{
    void const *iobeg = (uint8_t const *)  vfs->IoAllocator.BaseAddress;
    void const *ioend = (uint8_t const *)  vfs->IoAllocator.BaseAddress + vfs->IoAllocator.TotalSize;
    if (buffer >= iobeg && buffer < ioend)
    {   // only return the buffer if it's within the address range handed out
        // by the I/O buffer allocator. this excludes user-allocated buffers.
        srsw_fifo_put(&vfs->SiReturn[type], buffer);
    }
}

/// @summary Initialize a VFS driver state object and allocate any I/O resources.
/// @param vfs The VFS driver state to initialize.
/// @return true if the VFS driver state is initialized.
internal_function bool create_vfs_state(vfs_state_t *vfs)
{   // TODO: some error handling would be nice.
    create_iobuf_allocator(vfs->IoAllocator, VFS_IOBUF_SIZE, VFS_ALLOC_SIZE);
    create_srmw_fifo(&vfs->StOutWriteQ     , AIO_MAX_ACTIVE);
    create_srmw_fifo(&vfs->StOutCloseQ     , MAX_STREAMS_IN);
    create_srmw_fifo(&vfs->StInCommandQ    , MAX_STREAMS_IN);
    create_srmw_fifo(&vfs->StInCreateQ     , MAX_STREAMS_IN);
    for (size_t i = 0; i < FILE_TYPE_COUNT; ++i)
    {
        flush_srsw_fifo(&vfs->SiResult[i]);
        flush_srsw_fifo(&vfs->SiReturn[i]);
        flush_srsw_fifo(&vfs->SiEndOfS[i]);
    }
    io_opq_clear(&vfs->IoOperations);
    vfs->ActiveCount = 0;
    vfs->SiDecCount  = 0;
    return true;
}

/// @summary Free resources associated with a VFS driver state.
/// @param vfs The VFS driver state to delete.
internal_function void delete_vfs_state(vfs_state_t *vfs)
{
    vfs->ActiveCount = 0;
    io_opq_clear(&vfs->IoOperations);
    delete_srmw_fifo(&vfs->StInCreateQ);
    delete_srmw_fifo(&vfs->StInCommandQ);
    delete_srmw_fifo(&vfs->StOutCloseQ);
    delete_srmw_fifo(&vfs->StOutWriteQ);
    delete_iobuf_allocator(vfs->IoAllocator);
    for (size_t i = 0; i < FILE_TYPE_COUNT; ++i)
    {
        flush_srsw_fifo(&vfs->SiResult[i]);
        flush_srsw_fifo(&vfs->SiReturn[i]);
        flush_srsw_fifo(&vfs->SiEndOfS[i]);
    }
    for (size_t i = 0; i < vfs->SiDecCount; ++i)
    {
        if (vfs->SiDecInfo[i] != NULL)
        {
            delete vfs->SiDecInfo[i];
            vfs->SiDecInfo[i] = NULL;
        }
    }
    vfs->SiDecCount = 0;
}

/// @summary Checks a file type value to make sure it is known.
/// @param file_type One of the values of the file_type_e enumeration.
/// @return true if the file type is known.
internal_function bool check_file_type(int32_t file_type)
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

/// @summary No-op callback function invoked when the platform I/O system has
/// some data available for processing by the application.
/// @param app_id The application-defined identifier of the source file.
/// @param type One of the values of the file_type_e enumeration.
/// @param data Pointer to the data buffer. The data to read starts at offset 0.
/// @param offset The starting offset of the buffered data within the file.
/// @param size The number of valid bytes in the buffer.
internal_function void null_read_func(intptr_t app_id, int32_t type, void const *data, int64_t offset, uint32_t size)
{   // all parameters are unused. suppress compiler warnings.
    (void) sizeof(app_id);
    (void) sizeof(type);
    (void) sizeof(data);
    (void) sizeof(offset);
    (void) sizeof(size);
}

/// @summary No-op callback function invoked when the platform I/O system has
/// completed writing some data to a file.
/// @param app_id The application-defined identifier of the target file.
/// @param type One of the values of the file_type_e enumeration.
/// @param data Pointer to the data buffer. The data written starts at offset 0.
/// @param offset The byte offset of the start of the write operation within the file.
/// @param size The number of bytes written to the file.
internal_function void null_write_func(intptr_t app_id, int32_t type, void const *data, int64_t offset, uint32_t size)
{   // all parameters are unused. suppress compiler warnings.
    (void) sizeof(app_id);
    (void) sizeof(type);
    (void) sizeof(data);
    (void) sizeof(offset);
    (void) sizeof(size);
}

/// @summary No-op callback function invoked when an error occurs while the
/// platform I/O system encounters an error during a file operation.
/// @param app_id The application-defined identifier of the source file.
/// @param type One of the values of the file_type_e enumeration.
/// @param error_code The system error code value.
/// @param error_message An optional string description of the error.
internal_function void null_error_func(intptr_t app_id, int32_t type, uint32_t error_code, char const *error_message)
{
#ifdef DEBUG
    fprintf(stderr, "I/O ERROR: %p(%s): %u(0x%08X): %s\n", (void*) app_id, FILE_TYPE_NAME[type], error_code, error_code, strerror(error_code));
#else
    // in release mode, all parameters are unused. suppress compiler warnings.
    (void) sizeof(app_id);
    (void) sizeof(type);
    (void) sizeof(error_code);
    (void) sizeof(error_message);
#endif
}

/// @summary Formats and writes an I/O error description to stderr.
/// @param app_id The application-defined identifier of the file being accessed when the error occurred.
/// @param type The of the values of the file_type_e enumeration.
/// @param error_code The system error code value.
/// @param error_message An optional string description of the error.
internal_function void platform_print_ioerror(intptr_t app_id, int32_t type, uint32_t error_code, char const *error_message)
{
    int errn = int(error_code);
    fprintf(stderr, "I/O ERROR: %p(%s): %u(0x%08X): %s\n", (void*) app_id, FILE_TYPE_NAME[type], error_code, error_code, strerror(errn));
}

/// @summary Opens a file for streaming data to the application. The file will be
/// read from beginning to end and data returned to the application on the thread
/// appropriate for the given type.
/// @param path The NULL-terminated UTF-8 path of the file to load.
/// @param id The application-defined identifier for the stream.
/// @param type One of file_type_e indicating the type of file being loaded. This allows
/// the platform to decide the thread on which data should be returned to the application.
/// @param mode One of stream_in_mode_e indicating the stream behavior at end-of-stream.
/// @param start Specify true to start streaming in the file data immediately.
/// @param stream_size On return, this location is updated with the logical size of the stream.
/// @return true if the file was successfully opened.
internal_function bool platform_open_stream(char const *path, intptr_t id, int32_t type, uint32_t priority, int32_t mode, bool start, int64_t &stream_size)
{
    int     hint   = FILE_HINT_NONE;
    int     fd     = -1;
    int     efd    = -1;
    size_t  ssize  =  0;
    int64_t lsize  =  0;
    int64_t psize  =  0;
    int64_t offset =  0;
    stream_decoder_t *decoder = NULL;
    if (mode == STREAM_IN_ONCE)
    {   // for files that will be streamed in only once, prefer unbuffered I/O.
        // this avoids polluting the page cache with their data.
        hint  = FILE_HINT_DIRECT;
    }
    if (vfs_resolve_file_read(path, hint, fd, efd, lsize, psize, offset, ssize, decoder))
    {   // queue a load file request to be processed by the VFS driver.
        vfs_sics_t req;
        req.Next       = NULL;
        req.Fildes     = fd;
        req.Eventfd    = efd;
        req.DataSize   = lsize;  // size of the file after decompression
        req.FileSize   = psize;  // number of bytes to read from the file
        req.FileOffset = offset; // offset of first byte relative to fd 0, SEEK_SET
        req.ASID       = id;
        req.Type       = type;
        req.Behavior   = mode;
        req.Priority   = priority;
        req.SectorSize = ssize;
        req.Decoder    = decoder;
        stream_size    = lsize;  // return the logical size to the caller
        return srmw_fifo_put(&VFS_STATE.StInCreateQ, req);
    }
    else
    {   // unable to open the file, so fail immediately.
        stream_size = 0;
        return false;
    }
}

/// @summary Queues a file for loading. The file is read from beginning to end and
/// data is returned to the application on the thread appropriate for the given type.
/// The file is closed automatically when all data has been read, or an error occurs.
/// Equivalent to open_stream(path, id, type, priority, STREAM_IN_ONCE, true, stream_size).
/// @param path The NULL-terminated UTF-8 path of the file to load.
/// @param id The application-defined identifier for the load request.
/// @param type One of file_type_e indicating the type of file being loaded. This allows
/// the platform to decide the thread on which data should be returned to the application.
/// @param priority The file loading priority, with 0 indicating the highest possible priority.
/// @param stream_size On return, this location is updated with the logical size of the stream.
/// @return true if the file was successfully opened and the load was queued.
internal_function bool platform_stream_in(char const *path, intptr_t id, int32_t type, uint32_t priority, int64_t &stream_size)
{   // just redirect to open stream, which will queue the appropriate requests.
    return platform_open_stream(path, id, type, priority, STREAM_IN_ONCE, true, stream_size);
}

/// @summary Pauses a stream without closing the underlying file.
/// @param id The application-defined identifier of the stream.
/// @return true if the stream pause was queued.
internal_function bool platform_pause_stream(intptr_t id)
{
    vfs_siop_t   req;
    req.Next     = NULL;
    req.ASID     = id;
    req.OpId     = STREAM_IN_PAUSE;
    req.Argument = 0;
    return srmw_fifo_put(&VFS_STATE.StInCommandQ, req);
}

/// @summary Resumes streaming a paused stream.
/// @param id The application-defined identifier of the stream.
/// @return true if the stream start was queued.
internal_function bool platform_resume_stream(intptr_t id)
{
    vfs_siop_t   req;
    req.Next     = NULL;
    req.ASID     = id;
    req.OpId     = STREAM_IN_RESUME;
    req.Argument = 0;
    return srmw_fifo_put(&VFS_STATE.StInCommandQ, req);
}

/// @summary Starts streaming data from the beginning of the stream.
/// @param id The application-defined identifier of the stream.
/// @return true if the stream rewind was queued.
internal_function bool platform_rewind_stream(intptr_t id)
{
    vfs_siop_t   req;
    req.Next     = NULL;
    req.ASID     = id;
    req.OpId     = STREAM_IN_REWIND;
    req.Argument = 0;
    return srmw_fifo_put(&VFS_STATE.StInCommandQ, req);
}

/// @summary Positions the read cursor for the stream near a given location and resumes playback of the stream.
/// @param id The application-defined identifier of the stream.
/// @param absolute_offset The byte offset of the new playback position from the
/// start of the stream. The stream loading will resume from at or before this position.
/// @return true if the stream seek was queued.
internal_function bool platform_seek_stream(intptr_t id, int64_t absolute_offset)
{
    vfs_siop_t   req;
    req.Next     = NULL;
    req.ASID     = id;
    req.OpId     = STREAM_IN_SEEK;
    req.Argument = absolute_offset;
    return srmw_fifo_put(&VFS_STATE.StInCommandQ, req);
}

/// @summary Stops loading a stream and closes the underlying file.
/// @param id The application-defined identifier of the stream.
/// @return true if the stream close was queued.
internal_function bool platform_stop_stream(intptr_t id)
{
    vfs_siop_t   req;
    req.Next     = NULL;
    req.ASID     = id;
    req.OpId     = STREAM_IN_STOP;
    req.Argument = 0;
    return srmw_fifo_put(&VFS_STATE.StInCommandQ, req);
}

/// @summary Implements the default end-of-stream behavior for basic stream types.
/// @param id The application-defined identifier of the stream.
/// @param type One of file_type_e indicating the type of data being streamed in.
/// @param default_behavior One of stream_in_mode_e indicating the configured default behavior of the stream.
/// @return true if the behavior was processed.
internal_function bool platform_default_eos(intptr_t id, int32_t type, int32_t default_behavior)
{
    switch (default_behavior)
    {
        case STREAM_IN_ONCE: return platform_stop_stream(id);
        case STREAM_IN_LOOP: return platform_rewind_stream(id);
        default: break;
    }
    return false;
}

/// @summary Opens a file for reading or writing. The file is opened in buffered
/// mode, and all operations will block the calling thread. If the file exists,
/// it is opened and any existing data is preserved. If the file does not exist,
/// it is created and initialized to empty.
/// @param path The path of the file to open.
/// @param read_only Specify true to open the file in read-only mode.
/// @param file_size On return this value indicates the current size of the file, in bytes.
/// @param file On return, this value is set to the file record used for subsequent operations.
/// @return true if the file was opened.
internal_function bool platform_open_file(char const *path, bool read_only, int64_t &file_size, file_t **file)
{
    struct stat st;
    file_t *f = NULL;
    int flags = read_only ? (O_RDONLY | O_LARGEFILE) : (O_RDWR | O_LARGEFILE);
    int fd    = open(path, flags);
    if (fd == -1)
    {   // the file could not be opened; check errno.
        goto error_cleanup;
    }
    if (fstat(fd, &st) == -1)
    {   // the file could not be statted; check errno.
        goto error_cleanup;
    }
    f =  (file_t*) malloc(sizeof(file_t));
    if (f == NULL) goto error_cleanup;

    file_size    = st.st_size;
    f->Fildes    = fd;
    f->OpenFlags = flags;
    f->OpenStats = st;
    *file  = f;
    return true;

error_cleanup:
    if (fd != -1) close(fd);
    file_size = 0;
    *file = NULL;
    return false;
}

/// @summary Synchronously reads data from a file.
/// @param file The file state returned from open_file().
/// @param offset The absolute byte offset at which to start reading data.
/// @param buffer The buffer into which data will be written.
/// @param size The maximum number of bytes to read.
/// @param bytes_read On return, this value is set to the number of bytes actually
/// read. This may be less than the number of bytes requested, or 0 at end-of-file.
/// @return true if the read operation was successful.
internal_function bool platform_read_file(file_t *file, int64_t offset, void *buffer, size_t size, size_t &bytes_read)
{
    if (lseek(file->Fildes, 0, SEEK_SET) < 0)
    {   // unable to seek to the specified offset.
        bytes_read = 0;
        return false;
    }
    // safely read the requested number of bytes. for very large reads,
    // size > SSIZE_MAX, the result is undefined, so possibly split the
    // read up into several sub-reads (though this case is unlikely...)
    uint8_t *b  =(uint8_t*) buffer;
    bytes_read  = 0;
    while (size > 0)
    {
        ssize_t nread  =  read(file->Fildes, &b[bytes_read], size);
        if (nread > 0)
        {   // the read has completed successfully and returned data.
            bytes_read += size_t(nread);
            size       -= size_t(nread);
        }
        else if (nread == 0)
        {   // end-of-file was encountered.
            break;
        }
        else
        {   // an error occurred; check errno.
            return false;
        }
    }
    return true;
}

/// @summary Synchronously writes data to a file.
/// @param file The file state returned from open_file().
/// @param offset The absolute byte offset at which to start writing data.
/// @param buffer The data to be written to the file.
/// @param size The number of bytes to write to the file.
/// @param bytes_written On return, this value is set to the number of bytes
/// actually written to the file.
/// @return true if the write operation was successful.
internal_function bool platform_write_file(file_t *file, int64_t offset, void const *buffer, size_t size, size_t &bytes_written)
{
    if (lseek(file->Fildes, 0, SEEK_SET) < 0)
    {   // unable to seek to the specified offset.
        bytes_written = 0;
        return false;
    }
    uint8_t const *b  = (uint8_t const*) buffer;
    bytes_written     = 0;
    while (size > 0)
    {
        ssize_t nwrite = write(file->Fildes, &b[bytes_written], size);
        if (nwrite > 0)
        {   // the write has completed successfully.
            bytes_written += size_t(nwrite);
            size          -= size_t(nwrite);
        }
        else
        {   // an error occurred; check errno.
            return false;
        }
    }
    return true;
}

/// @summary Flushes any buffered writes to the file, and updates file metadata.
/// @param file The file state returned from open_file().
/// @return true if the flush operation was successful.
internal_function bool platform_flush_file(file_t *file)
{
    return (fsync(file->Fildes) == 0);
}

/// @summary Closes a file.
/// @param file The file state returned from open_file().
/// @return true if the file is closed.
internal_function bool platform_close_file(file_t **file)
{   // TODO: consider extending this to support archive files?
    file_t *f = *file;
    *file  = NULL;
    if (f != NULL)
    {
        if (f->Fildes != -1) close(f->Fildes);
        free(f);
    }
    return true;
}

/// @summary Saves a file to disk. If the file exists, it is overwritten. This
/// operation is performed entirely synchronously and will block the calling
/// thread until the file is written. The file is guaranteed to have been either
/// written successfully, or not at all.
/// @param path The path of the file to write.
/// @param data The contents of the file.
/// @param size The number of bytes to read from data and write to the file.
/// @return true if the operation was successful.
internal_function bool platform_write_out(char const *path, void const *data, int64_t size)
{
    uint8_t const *buffer        = (uint8_t const*) data;          // the input buffer
    size_t  const  page_size     = (size_t) sysconf(_SC_PAGESIZE); // the system page size, in bytes
    int     const  prot          = PROT_READ   | PROT_WRITE;       // we want to be able to read and write
    int     const  flags         = MAP_PRIVATE | MAP_ANONYMOUS;    // mapping is private and may begin anywhere
    struct  stat   st            = {0};                            // for retrieving the physical sector size
    int64_t        fsize         =  0;                             // the file size, rounded up to the nearest sector
    uint8_t       *sector_buffer = NULL;                           // a one-page buffer for padding, aligned
    size_t         sector_count  =  0;                             // the number of whole sectors in 'data'
    size_t         sector_bytes  =  0;                             // the nearest sector-size multiple <= 'size'
    size_t         sector_over   =  0;                             // how much over a sector boundary 'size' is
    size_t         pathlen       =  0;                             // the total length of 'path', in bytes
    size_t         dirlen        =  0;                             // the length of the path part of 'path', in bytes
    size_t         ssize         =  0;                             // the physical disk sector size, in bytes
    char          *temp_path     = NULL;                           // buffer for the path of the temporary file
    int            fd            = -1;                             // file descriptor of the temporary file

    // we need to create this file in the same directory as the output path.
    // this avoids problems with rename() not being able to work across partitions.
    // so, generate a unique filename within that same directory.
    pathend(path, dirlen, pathlen);
    temp_path  = (char*) malloc(dirlen + 17); // + '/' + 'writeout-XXXXXX'
    memset(temp_path, 0, dirlen + 17);
    if (dirlen > 0)
    {   // the output path has volume and/or directory information specified.
        // copy it to the temporary path buffer, and append a path separator.
        memcpy(temp_path, path, dirlen);
        strcat(temp_path, "/");
    }
    // now append the template filename to the temp_path buffer.
    strcat(temp_path, "writeout-XXXXXX");

    // allocate enough storage for a single block/sector. to support
    // direct I/O, the buffer must be allocated on an address that is
    // an even multiple of the disk sector size. allocating a single
    // page from the VMM will satisfy this requirement, and as an
    // added bonus, the page contents is zeroed, so we don't have to.
    if ((sector_buffer = (uint8_t*) mmap(NULL, page_size, prot, flags, -1, 0)) == NULL)
    {   // unable to allocate the single-sector buffer. check errno.
        goto error_cleanup;
    }

    // use mkstemp to generate a temporary filename and open the file.
    if ((fd = mkstemp(temp_path)) == -1)
    {   // the file could not be opened at all. check errno.
        goto error_cleanup;
    }

    // mkstemp doesn't allow specification of additional flags, so add O_DIRECT.
    // if this fails, it's not a fatal; we'll still be able to write the file.
    fcntl(fd, F_SETFL, O_DIRECT);

    // retrieve the physical block size for the disk containing the file.
    if (fstat(fd, &st) < 0)
    {   // unable to retrieve file stats; fail.
        goto error_cleanup;
    }

    // copy the data extending into the tail sector into our temporary buffer.
    sector_count  = size_t(size / st.st_blksize);
    sector_bytes  = size_t(st.st_blksize * sector_count);
    sector_over   = size_t(size - sector_bytes);
    if (sector_over > 0)
    {   // buffer the overlap amount into our temporary buffer.
        memcpy(sector_buffer, &buffer[sector_bytes], sector_over);
    }

    // align the file size up to an even multiple of the physical sector size.
    // pre-allocate space for the file, without changing the file size.
    ssize = st.st_blksize;
    fsize = align_up(size, ssize);
    if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, off_t(fsize)) != 0)
    {   // unable to pre-allocate space for the file. check errno.
        goto error_cleanup;
    }

    // finally, we can write the data to the file.
    if (sector_bytes > 0)
    {   // write the bulk of the data, if the data is > 1 sector.
        write(fd, buffer, sector_bytes);
    }
    if (sector_over  > 0)
    {   // write the remainder of the data.
        write(fd, sector_buffer, st.st_blksize);
    }

    // close the file, and then truncate it to the correct size.
    if (close(fd) == -1 || truncate(temp_path, size) == -1)
    {   // unable to set the correct file size. check errno.
        fd = -1; goto error_cleanup;
    }

    // all of that was successful, to move the temp file to the destination path.
    if (rename(temp_path, path) == -1)
    {   // the temp file could not be renamed. check errno.
        fd = -1; goto error_cleanup;
    }

    // all done; clean up temporary memory.
    munmap(sector_buffer, page_size);
    free(temp_path);
    return true;

error_cleanup:
    if (sector_buffer != NULL) munmap(sector_buffer, page_size);
    if (fd != -1) close(fd);
    if (temp_path != NULL)
    {   // be sure to delete the temporary file.
        unlink(temp_path);
        free(temp_path);
    }
    return false;
}

/// @summary Opens a new temporary file for writing. The file is initially empty.
/// Data may be written to the file using append_stream(). When finished, call
/// close_stream() to close the file and move it to its final destination.
/// @param where The directory path where the file will be created, or NULL to use the CWD.
/// @param priority The file operation priority, with 0 indicating the highest possible priority.
/// @param reserve_size The size, in bytes, to preallocate for the file. This makes write
/// operations more efficient. Specify zero if unknown.
/// @param writer On return, this value will point to the file writer state.
/// @return true if the file is opened and ready for write operations.
internal_function bool platform_create_stream(char const *where, uint32_t priority, int64_t reserve_size, stream_writer_t **writer)
{
    stream_writer_t *fw = NULL; // the file writer we return
    struct  stat     st = {0};  // for retrieving the physical sector size
    int64_t fsize       =  0;   // the file size, rounded up to the nearest sector
    size_t  ssize       =  0;   // the physical disk sector size, in bytes
    size_t  pathlen     =  0;   // the total length of 'path', in bytes
    size_t  dirlen      =  0;   // the length of the path part of 'path', in bytes
    char   *temp_path   = NULL; // buffer for the path of the temporary file
    void   *buffer      = NULL; // the write buffer for the stream
    int     fd          = -1;   // file descriptor of the temporary file
    int     prot        = PROT_READ   | PROT_WRITE;
    int     flags       = MAP_PRIVATE | MAP_ANONYMOUS;

    // allocate storage for the file writer up front.
    fw = (stream_writer_t*) malloc(sizeof(stream_writer_t));
    if (fw == NULL) goto error_cleanup;

    // we need to create this file in the same directory as the output path.
    // this avoids problems with rename() not being able to work across partitions.
    // so, generate a unique filename within that same directory.
    pathend(where, dirlen, pathlen);
    temp_path  = (char*) malloc(dirlen + 17); // + '/' + 'tempfile-XXXXXX'
    memset(temp_path, 0, dirlen + 17);
    if (dirlen > 0)
    {   // the output path has volume and/or directory information specified.
        // copy it to the temporary path buffer, and append a path separator.
        memcpy(temp_path, where, dirlen);
        strcat(temp_path, "/");
    }
    // now append the template filename to the temp_path buffer.
    strcat(temp_path, "tempfile-XXXXXX");

    // use mkstemp to generate a temporary filename and open the file.
    if ((fd = mkstemp(temp_path)) == -1)
    {   // the file could not be opened at all. check errno.
        goto error_cleanup;
    }

    // mkstemp doesn't allow specification of additional flags, so add O_DIRECT.
    // if this fails, it's not a fatal; we'll still be able to write the file.
    // mkostemp does not mention allowing O_DIRECT, so avoid it.
    fcntl(fd, F_SETFL, O_APPEND | O_DIRECT);

    // retrieve the physical block size for the disk containing the file.
    if (fstat(fd, &st) < 0)
    {   // unable to retrieve file stats; fail.
        goto error_cleanup;
    }

    // align the file size up to an even multiple of the physical sector size.
    // pre-allocate space for the file, without changing the file size.
    if (reserve_size > 0)
    {   // this is best-effort; if it fails, that's okay.
        ssize = st.st_blksize;
        fsize = align_up(reserve_size, ssize);
        fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, off_t(fsize));
    }

    // now use mmap to allocate a buffer for combining small writes.
    // use mmap because it is guaranteed to return addresses with the
    // correct alignment, and ranges rounded up to the correct size
    // for use with direct block I/O files.
    if ((buffer = mmap(NULL, VFS_WRITE_SIZE, prot, flags, -1, 0)) == NULL)
    {   // the mmap failed; check errno.
        goto error_cleanup;
    }

    // populate the file writer; we're done.
    fw->Fildes      = fd;
    fw->Eventfd     = eventfd(0, 0);
    fw->BaseAddress = (uint8_t*) buffer;
    fw->DataOffset  = 0;
    fw->FileOffset  = 0;
    fw->Priority    = priority;
    *writer = fw;
    return true;

error_cleanup:
    if (fd != -1)
    {   // delete the temporary file.
        close(fd);
        unlink(temp_path);
    }
    if (temp_path != NULL) free(temp_path);
    if (fw != NULL) free(fw);
    *writer = NULL;
    return false;
}

/// @summary Queues a write operation against an open file. The file should have
/// previously been opened using create_stream(). The data is always appended to
/// the end of the file; writes to arbitrary locations are not supported.
/// @param writer The file writer state returned from the create file call.
/// @param data The data to write. Do not modify the contents of this buffer
/// until the write completion notification is received.
/// @param size The number of bytes to write.
/// @param bytes_written On return, this value is updated with the number of bytes written.
/// @return true if the write operation was successful.
internal_function bool platform_append_stream(stream_writer_t *writer, void const *data, uint32_t size, size_t &bytes_written)
{
    int const flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int const prot  = PROT_READ   | PROT_WRITE;

    bytes_written = 0;
    while (size   > 0)
    {
        uint8_t const  *srcbuf = (uint8_t const*) data + bytes_written;
        void  *newbuf = writer->BaseAddress;                         /* assume no new buffer is needed  */
        size_t nwrite = writer->DataOffset + size > VFS_WRITE_SIZE ? /* does size exceed buffer space?  */
                        VFS_WRITE_SIZE - writer->DataOffset        : /* yes, so fill the current buffer */
                        size;                                        /* no, so write the remaining data */

        if (writer->DataOffset + nwrite == VFS_WRITE_SIZE)
        {   // allocate a new buffer for the next write operation(s).
            // we do this first because if it fails, we want to fail
            // but still have a 'live' buffer.
            if ((newbuf = mmap(NULL, VFS_WRITE_SIZE, prot, flags, -1, 0)) == NULL)
            {   // this is a serious error - check errno.
                return false;
            }

            // fill up the active buffer, and queue a write to the VFS.
            // if queueing the write fails, we won't update DataOffset,
            // and the caller can attempt to resume the write later.
            memcpy(&writer->BaseAddress[writer->DataOffset], srcbuf, nwrite);

            vfs_sowr_t write;
            write.Next       = NULL;
            write.Fildes     = writer->Fildes;
            write.Eventfd    = writer->Eventfd;
            write.FileOffset = writer->FileOffset;
            write.DataBuffer = writer->BaseAddress;
            write.DataSize   = VFS_WRITE_SIZE;
            write.Priority   = writer->Priority;
            if (srmw_fifo_put(&VFS_STATE.StOutWriteQ, write))
            {
                size -= uint32_t(nwrite);
                bytes_written += nwrite;
                writer->BaseAddress = (uint8_t*) newbuf;
                writer->FileOffset += VFS_WRITE_SIZE;
                writer->DataOffset  = 0;
            }
            else return false;
        }
        else
        {   // this write only partially fills up the buffer.
            size -= uint32_t(nwrite);
            bytes_written += nwrite;
            memcpy(&writer->BaseAddress[writer->DataOffset], srcbuf, nwrite);
            writer->DataOffset += nwrite;
        }
    }
    return true;
}

/// @summary Closes a file previously opened using create_stream(), and atomically
/// renames that file to move it to the specified path.
/// @param writer The stream writer state returned from the create stream call.
/// @param path The target path and filename of the file, or NULL to delete the file.
/// @return true if the finalize operation was successfully queued.
internal_function bool platform_close_stream(stream_writer_t **writer, char const *path)
{
    stream_writer_t  *sw = *writer;
    void           *addr =  sw->BaseAddress;
    size_t            nb =  sw->DataOffset;
    int64_t           fs =  sw->FileOffset + sw->DataOffset;
    uint32_t    priority =  sw->Priority;
    int               fd =  sw->Fildes;
    int              efd =  sw->Eventfd;

    if (nb > 0)
    {   // queue a write with the remaining data.
        vfs_sowr_t       write;
        write.Next       = NULL;
        write.Fildes     = sw->Fildes;
        write.Eventfd    = sw->Eventfd;
        write.FileOffset = sw->FileOffset;
        write.DataBuffer = sw->BaseAddress;
        write.DataSize   = VFS_WRITE_SIZE;
        write.Priority   = sw->Priority;
        if (srmw_fifo_put(&VFS_STATE.StOutWriteQ, write))
        {   // the write was submitted; update the offset in case the close fails.
            sw->FileOffset += VFS_WRITE_SIZE;
            sw->DataOffset  = 0;
        }
        else
        {   // unable to queue the final write; don't proceed with the close.
            return false;
        }
    }
    else if (addr != NULL)
    {   // there's an empty buffer hanging around; free it now.
        munmap(addr, VFS_WRITE_SIZE);
    }

    // submit the stream close request to the VFS.
    vfs_socs_t     close;
    close.Next     = NULL;
    close.Fildes   = fd;
    close.Eventfd  = efd;
    close.Priority = priority;
    close.FilePath = NULL;
    close.FileSize = fs;
    if (path != NULL)
    {   // copy the path string. this will be freed by AIO.
        close.FilePath = strdup(path);
    }
    if (srmw_fifo_put(&VFS_STATE.StOutCloseQ, close))
    {   // the close was successfully submitted.
        *writer = NULL;
        free(sw);
        return true;
    }
    else
    {   // free the file path to avoid leaking memory.
        if (close.FilePath != NULL) free(close.FilePath);
        return false;
    }
}

/*////////////////////////
//   Public Functions   //
////////////////////////*/
static int test_stream_in(int argc, char **argv, platform_layer_t *p)
{
    int64_t ss = 0;
    int result = EXIT_SUCCESS;
    bool  done = false;

    init_io_stats(&IO_STATS);
    create_aio_state(&AIO_STATE);
    create_vfs_state(&VFS_STATE);

    if (argc < 2)
    {
        fprintf(stderr, "ERROR: Missing required argument infile.\n");
        fprintf(stdout, "USAGE: a.out infile\n");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    // start a stream-in.
    if (p->open_stream(argv[1], 1234, 0, 0, STREAM_IN_LOOP, true, ss))
    {
        fprintf(stdout, "Successfully loaded \'%s\'; %" PRId64 " bytes.\n", argv[1], ss);
        result = EXIT_SUCCESS;
    }
    else
    {
        fprintf(stderr, "ERROR: Unable to load file \'%s\'.\n", argv[1]);
        fprintf(stdout, "USAGE: a.out infile\n");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    // run everything on the same thread, which isn't required.
    // we could run VFS on one thread, AIO on another, or both on the same.
    while (!done)
    {   // update the VFS and AIO drivers. this generates AIO operations,
        // updates stream-in status, and pushes data to the application.
        vfs_tick(&VFS_STATE, &AIO_STATE, &IO_STATS);
        aio_poll(&AIO_STATE);

        if (VFS_STATE.ActiveCount == 0)
        {   // we could have closed due to an error in this test case.
            // normally, you wouldn't have this check.
            done = true;
        }

        // process data received from the I/O system. normally, different
        // threads would handle one or more file types, depending on what
        // needs to be done with the data and who needs access to it.
        for (size_t i = 0; i < FILE_TYPE_COUNT; ++i)
        {
            int32_t    type = int32_t(i); // the file_type_e.
            vfs_sird_t read;
            while (srsw_fifo_get(&VFS_STATE.SiResult[i], read))
            {
                if (read.OSError == 0)
                {   // echo the data to stdout.
                    // normally, you'd push the result to a callback.
                    // note that all of the reading of the data is performed
                    // through the stream decoder, which transparently performs
                    // any decompression and/or decryption that might be necessary.
                    read.Decoder->push(read.DataBuffer, read.DataAmount);
                    do
                    {
                        size_t amount = read.Decoder->amount();
                        void  *buffer = read.Decoder->Cursor;
                        fwrite(buffer , 1, amount, stdout);
                        read.Decoder->Cursor += amount;
                    }
                    while (read.Decoder->refill(read.Decoder) == DECODE_RESULT_START);
                    // after the application has had a chance to process
                    // the data, return the buffer so it can be used again.
                    vfs_return_buffer(&VFS_STATE, type, read.DataBuffer);
                }
                else
                {   // an error occurred, so display it and then exit.
                    platform_print_ioerror(read.ASID, type, read.OSError, strerror(read.OSError));
                    p->stop_stream(read.ASID);
                    result = EXIT_FAILURE;
                }
            }

            vfs_sies_t eos;
            while (srsw_fifo_get(&VFS_STATE.SiEndOfS[i], eos))
            {
                fprintf(stdout, "Reached end-of-stream for ASID %p.\n", (void*) eos.ASID);
                p->stop_stream(eos.ASID);//p->rewind_stream(eos.ASID);
            }
        }
    }

cleanup:
    delete_vfs_state(&VFS_STATE);
    delete_aio_state(&AIO_STATE);
    return result;
}

static int test_fileio_in(int argc, char **argv, platform_layer_t *p)
{
    size_t  nr = 0;
    int64_t fo = 0;
    int64_t ss = 0;
    int result = EXIT_SUCCESS;
    bool  done = false;
    file_t *fp = NULL;
    void  *buf = malloc(VFS_ALLOC_SIZE);

    if (argc < 2)
    {
        fprintf(stderr, "ERROR: Missing required argument infile.\n");
        fprintf(stdout, "USAGE: a.out infile\n");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    // open the file.
    if (p->open_file(argv[1], true, ss, &fp))
    {
        fprintf(stdout, "Successfully loaded \'%s\'; %" PRId64 " bytes.\n", argv[1], ss);
        result = EXIT_SUCCESS;
    }
    else
    {
        fprintf(stderr, "ERROR: Unable to load file \'%s\'.\n", argv[1]);
        fprintf(stdout, "USAGE: a.out infile\n");
        result = EXIT_FAILURE;
        goto cleanup;
    }

    // read as much data as possible, write to stdout.
    while (p->read_file(fp, fo, buf, VFS_ALLOC_SIZE, nr))
    {
        if (nr == 0)
        {
            fo  = 0;
            continue;
        }
        else
        {
            fwrite(buf, 1, nr, stdout);
            fo += nr;
        }
    }

cleanup:
    if (fp != NULL) p->close_file(&fp);
    return result;
}

/// @summary Entry point of the application.
/// @param argc The number of command-line arguments.
/// @param argv An array of NULL-terminated strings specifying command-line arguments.
/// @return Either EXIT_SUCCESS or EXIT_FAILURE.
export_function int main(int argc, char **argv)
{
    platform_layer_t platform_layer;
    int exit_code = EXIT_SUCCESS;

    // set up the platform layer function pointers:
    platform_layer.stream_in     = platform_stream_in;
    platform_layer.open_stream   = platform_open_stream;
    platform_layer.pause_stream  = platform_pause_stream;
    platform_layer.resume_stream = platform_resume_stream;
    platform_layer.rewind_stream = platform_rewind_stream;
    platform_layer.seek_stream   = platform_seek_stream;
    platform_layer.stop_stream   = platform_stop_stream;
    platform_layer.open_file     = platform_open_file;
    platform_layer.read_file     = platform_read_file;
    platform_layer.write_file    = platform_write_file;
    platform_layer.flush_file    = platform_flush_file;
    platform_layer.close_file    = platform_close_file;
    platform_layer.write_out     = platform_write_out;
    platform_layer.create_stream = platform_create_stream;
    platform_layer.append_stream = platform_append_stream;
    platform_layer.close_stream  = platform_close_stream;

    // TODO: other platform init code here.
    //
    // TODO: dynamically load the application code.

    exit_code = test_stream_in(argc, argv, &platform_layer);
    //exit_code = test_fileio_in(argc, argv, &platform_layer);

    exit(exit_code);
}

