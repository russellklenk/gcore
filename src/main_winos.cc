/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the entry point of the application, along with all
/// platform-specific functionality for Windows-based platforms.
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
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include <windows.h>
#include <inttypes.h>

#include "bridge.h"
#include "decode.cc"
#include "datain.cc"

#ifdef __GNUC__
#ifndef QUOTA_LIMITS_HARDWS_MIN_ENABLE
    #define QUOTA_LIMITS_HARDWS_MIN_ENABLE     0x00000001
#endif

#ifndef QUOTA_LIMITS_HARDWS_MAX_DISABLE
    #define QUOTA_LIMITS_HARDWS_MAX_DISABLE    0x00000008
#endif
#ifndef METHOD_BUFFERED
    #define METHOD_BUFFERED                    0
#endif
#ifndef FILE_ANY_ACCESS
    #define FILE_ANY_ACCESS                    0
#endif

#ifndef FILE_DEVICE_MASS_STORAGE
    #define FILE_DEVICE_MASS_STORAGE           0x0000002d
#endif

#ifndef CTL_CODE
    #define CTL_CODE(DeviceType, Function, Method, Access )                      \
        (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#ifndef IOCTL_STORAGE_BASE
    #define IOCTL_STORAGE_BASE                 FILE_DEVICE_MASS_STORAGE
#endif

#ifndef IOCTL_STORAGE_QUERY_PROPERTY
    #define IOCTL_STORAGE_QUERY_PROPERTY CTL_CODE(IOCTL_STORAGE_BASE, 0x0500, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#if   (__GNUC__ < 4) || (__GNUC__ == 4 && __GNUC_MINOR__ <= 8)
typedef enum _STORAGE_PROPERTY_ID {
    StorageDeviceProperty = 0,
    StorageAdapterProperty,
    StorageDeviceIdProperty,
    StorageDeviceUniqueIdProperty,
    StorageDeviceWriteCacheProperty,
    StorageMiniportProperty,
    StorageAccessAlignmentProperty,
    StorageDeviceSeekPenaltyProperty,
    StorageDeviceTrimProperty,
    StorageDeviceWriteAggregationProperty,
    StorageDeviceTelemetryProperty,
    StorageDeviceLBProvisioningProperty,
    StorageDevicePowerProperty,
    StorageDeviceCopyOffloadProperty,
    StorageDeviceResiliencyPropery
} STORAGE_PROPERTY_ID, *PSTORAGE_PROPERTY_ID;

typedef enum _STORAGE_QUERY_TYPE {
    PropertyStandardQuery = 0,
    propertyExistsQuery,
    PropertyMaskQuery,
    PropertyQueryMaxDefined
} STORAGE_QUERY_TYPE, *PSTORAGE_QUERY_TYPE;

typedef struct _STORAGE_PROPERTY_QUERY {
    STORAGE_PROPERTY_ID PropertyId;
    STORAGE_QUERY_TYPE  QueryType;
    UCHAR               AdditionalParameters[1];
} STORAGE_PROPERTY_QUERY, *PSTORAGE_PROPERTY_QUERY;
#endif /* __GNUC__ < 4.9 */

#if (__GNUC__ < 4) || (__GNUC__ == 4 && __GNUC_MINOR__ <= 9)
typedef enum _FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo = 0,
    FileStandardInfo = 1,
    FileNameInfo = 2,
    FileRenameInfo = 3,
    FileDispositionInfo = 4,
    FileAllocationInfo = 5,
    FileEndOfFileInfo = 6,
    FileStreamInfo = 7,
    FileCompressionInfo = 8,
    FileAttributeTagInfo = 9,
    FileIdBothDirectoryInfo = 10,
    FileIdBothDirectoryRestartInfo = 11,
    FileIoPriorityHintInfo = 12,
    FileRemoteProtocolInfo = 13,
    FileFullDirectoryInfo = 14,
    FileFullDirectoryRestartInfo = 15,
    FileStorageInfo = 16,
    FileAlignmentInfo = 17,
    FileIdInfo = 18,
    FileIdExtdDirectoryInfo = 19,
    FileIdExtdDirectoryRestartInfo = 20,
    MaximumFileInfoByHandlesClass
} FILE_INFO_BY_HANDLE_CLASS, *PFILE_INFO_BY_HANDLE_CLASS;

typedef struct _STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR {
    DWORD Version;
    DWORD Size;
    DWORD BytesPerCacheLine;
    DWORD BytesOffsetForCacheAlignment;
    DWORD BytesPerLogicalSector;
    DWORD BytesPerPhysicalSector;
    DWORD BytesOffsetForSectorAlignment;
} STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR, *PSTORAGE_ACCESS_ALIGNMENT_DESCRIPTOR;

typedef struct _FILE_ALLOCATION_INFO {
    LARGE_INTEGER AllocationSize;
} FILE_ALLOCATION_INFO, *PFILE_ALLOCATION_INFO;

typedef struct _FILE_END_OF_FILE_INFO {
    LARGE_INTEGER EndOfFile;
} FILE_END_OF_FILE_INFO, *PFILE_END_OF_FILE_INFO;

typedef struct _FILE_NAME_INFO {
    DWORD FileNameLength;
    WCHAR FileName[1];
} FILE_NAME_INFO, *PFILE_NAME_INFO;
#endif /* __GNUC__ <= 4.9 - MinGW */
#endif /* __GNUC__ comple - MinGW */

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
#ifndef WINOS_MAX_STREAMS_IN
#define WINOS_MAX_STREAMS_IN    16
#endif
static size_t   const MAX_STREAMS_IN = WINOS_MAX_STREAMS_IN;

/// @summary Define the maximum number of concurrently active decoder streams.
/// This needs to be larger than the maximum number of active stream-in files,
/// as decode will lag behind raw I/O slightly in most cases.
static size_t   const MAX_STREAMS_LIVE = MAX_STREAMS_IN * 2;

/// @summary Define the maximum number of concurrently active AIO operations.
/// We set this based on what the maximum number of AIO operations we want to
/// poll during each tick, and the maximum number the underlying OS can handle.
#ifndef WINOS_AIO_MAX_ACTIVE    // can override at compile time
#define WINOS_AIO_MAX_ACTIVE    128
#endif
static size_t    const AIO_MAX_ACTIVE = WINOS_AIO_MAX_ACTIVE;

/// @summary Define the size of the I/O buffer. This is calculated based on an
/// maximum I/O transfer rate of 960MB/s, and an I/O system tick rate of 60Hz;
/// 960MB/sec divided across 60 ticks/sec gives 16MB/tick maximum transfer rate.
#ifndef WINOS_VFS_IOBUF_SIZE    // can override at compile time
#define WINOS_VFS_IOBUF_SIZE   (16 * 1024 * 1024)
#endif
static size_t    const VFS_IOBUF_SIZE = WINOS_VFS_IOBUF_SIZE;

/// @summary Define the size of the write buffer for output streams. This must
/// be at least the size of a single page, as buffers are allocated using mmap.
#ifndef WINOS_VFS_WRITE_SIZE    // can override at compile time
#define WINOS_VFS_WRITE_SIZE   (64 * 1024)
#endif
static size_t   const VFS_WRITE_SIZE = WINOS_VFS_WRITE_SIZE;

/// @summary Define the size of the buffer allocated for each I/O request.
static size_t   const VFS_ALLOC_SIZE = VFS_IOBUF_SIZE / AIO_MAX_ACTIVE;

/// @summary Define the file size limit for preferring buffered I/O. Large
/// files can pollute the page cache and will reduce overall I/O throughput.
#ifndef WINOS_VFS_DIRECT_IO_THRESHOLD
#define WINOS_VFS_DIRECT_IO_THRESHOLD (16 * 1024 * 1024)
#endif
static int64_t  const VFS_DIRECT_IO_THRESHOLD = WINOS_VFS_DIRECT_IO_THRESHOLD;

/// @summary The spin count used on critical sections protecting shared resources
/// of srmw_freelist_t and srmw_fifo_t.
static DWORD     const SPIN_COUNT_Q   = 4096;

/// @summary The special I/O completion port completion key used to notify the
/// AIO driver to begin its shutdown process.
static ULONG_PTR const AIO_SHUTDOWN   = ULONG_PTR(-1);

/*///////////////////
//   Local Types   //
///////////////////*/
/// @summary Define the set of application thread identifiers. The platform layer
/// defines a function mapping file type to the thread ID on which I/O for that
/// file type is processed. IDs must start at zero and increase monotonically.
enum thread_id_e
{
    THREAD_ID_ANY     = 0, /// A special thread identifier representing all threads.
    THREAD_ID_COUNT        /// The number of defined application thread identifiers.
};

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
    VFS_STATUS_CLOSE  = (1 << 1), /// The stream is marked as having a close pending.
    VFS_STATUS_CLOSED = (1 << 2), /// The stream is fully closed.
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

/// @summary Define the various statistic counters maintained by the I/O system.
/// These values should start from zero and increase monotonically.
enum io_count_e
{
    IO_COUNT_STREAM_IN_OPEN = 0,
    IO_COUNT_STREAM_IN_OPEN_ONCE,
    IO_COUNT_STREAM_IN_OPEN_LOOP,
    IO_COUNT_STREAM_IN_EOS,
    IO_COUNT_STREAM_IN_REWIND,
    IO_COUNT_STREAM_IN_SEEK,
    IO_COUNT_STREAM_IN_STOP,
    IO_COUNT_STREAM_IN_PAUSE,
    IO_COUNT_STREAM_IN_RESUME,
    IO_COUNT_READS_STARTED,
    IO_COUNT_READS_COMPLETE_SUCCESS,
    IO_COUNT_READS_COMPLETE_ERROR,
    IO_COUNT_WRITES_STARTED,
    IO_COUNT_WRITES_COMPLETE_SUCCESS,
    IO_COUNT_WRITES_COMPLETE_ERROR,
    IO_COUNT_FLUSHES_STARTED,
    IO_COUNT_FLUSHES_COMPLETE_SUCCESS,
    IO_COUNT_FLUSHES_COMPLETE_ERROR,
    IO_COUNT_CLOSES_STARTED,
    IO_COUNT_CLOSES_COMPLETE_SUCCESS,
    IO_COUNT_CLOSES_COMPLETE_ERROR,
    IO_COUNT_BYTES_READ_REQUEST,
    IO_COUNT_BYTES_READ_ACTUAL,
    IO_COUNT_BYTES_WRITE_REQUEST,
    IO_COUNT_BYTES_WRITE_ACTUAL,
    IO_COUNT_NANOS_ELAPSED_AIO,
    IO_COUNT_NANOS_ELAPSED_VFS,
    IO_COUNT_TICKS_ELAPSED_AIO,
    IO_COUNT_TICKS_ELAPSED_VFS,
    IO_COUNT_MAX_OPS_QUEUED,
    IO_COUNT_MAX_STREAM_IN_BYTES_USED,
    IO_COUNT_STREAM_IN_BYTES_USED,
    IO_COUNT_MAX_TICK_DURATION_AIO,
    IO_COUNT_MAX_TICK_DURATION_VFS,
    IO_COUNT_MIN_TICK_DURATION_AIO,
    IO_COUNT_MIN_TICK_DURATION_VFS,
    IO_COUNT_COUNT
};

/// @summary Define the various error counters maintained by the I/O system.
/// These values should start from zero and increase monotonically.
enum io_error_e
{
    IO_ERROR_FULL_READRESULTS = 0,
    IO_ERROR_FULL_WRITERESULTS,
    IO_ERROR_FULL_CLOSERESULTS,
    IO_ERROR_FULL_RESULTQUEUE,
    IO_ERROR_INVALID_AIO_CMD,
    IO_ERROR_ORPHANED_IOCB,
    IO_ERROR_COUNT
};

/// @summary Define the various stall counters maintained by the I/O system.
/// These values should start from zero and increase monotonically.
enum io_stall_e
{
    IO_STALL_FULL_AIO_QUEUE = 0,
    IO_STALL_FULL_VFS_QUEUE,
    IO_STALL_OUT_OF_IOBUFS,
    IO_STALL_COUNT
};

/// @summary Define the various rate counters maintained by the I/O system.
/// These values should start from zero and increase monotonically.
enum io_rate_e
{
    IO_RATE_BYTES_PER_SEC_IN = 0,
    IO_RATE_BYTES_PER_SEC_OUT,
    IO_RATE_COUNT
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
    HANDLE             Fildes;       /// The file descriptor of the file. Required.
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
    HANDLE             Fildes;       /// The file descriptor of the file.
    DWORD              OSError;      /// The error code returned by the operation, or 0.
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
typedef srsw_fifo_t<aio_res_t, MAX_STREAMS_LIVE> aio_cresultq_t; /// Queue for close operation results.
typedef srsw_fifo_t<aio_res_t, MAX_STREAMS_LIVE> aio_fresultq_t; /// Queue for flush operation results.
typedef srsw_fifo_t<aio_res_t, AIO_MAX_ACTIVE>   aio_rresultq_t; /// Queue for read operation results.
typedef srsw_fifo_t<aio_res_t, AIO_MAX_ACTIVE>   aio_wresultq_t; /// Queue for write operation results.
typedef srsw_fifo_t<aio_req_t, AIO_MAX_ACTIVE>   aio_requestq_t; /// Queue for all operation requests.

/// @summary Define the state associated with the AIO driver. The AIO driver
/// receives requests to read, write, flush and close files, and then queues
/// kernel AIO operations to perform them.
struct aio_state_t
{
    aio_requestq_t     RequestQueue;             /// The queue for all pending AIO requests.
    OVERLAPPED         ASIOPool[AIO_MAX_ACTIVE]; /// The static pool of OVERLAPPED structures.
    HANDLE             ASIOContext;              /// The I/O completion port handle.
    size_t             ActiveCount;              /// The number of in-flight AIO requests.
    aio_req_t          AAIOList[AIO_MAX_ACTIVE]; /// The set of active AIO requests [ActiveCount valid].
    OVERLAPPED        *ASIOList[AIO_MAX_ACTIVE]; /// The dynamic list of active OVERLAPPED [ActiveCount valid].
    size_t             ASIOFreeCount;            /// The number of available OVERLAPPED.
    OVERLAPPED        *ASIOFree[AIO_MAX_ACTIVE]; /// The list of available OVERLAPPED [ASIOFreeCount valid].
    aio_rresultq_t     ReadResults;              /// Queue for completed read  operations.
    aio_wresultq_t     WriteResults;             /// Queue for completed write operations.
    aio_fresultq_t     FlushResults;             /// Queue for completed flush operations.
    aio_cresultq_t     CloseResults;             /// Queue for completed close operations.
};

/// @summary Defines the data associated with a stream in creation request passed
/// to the VFS driver. This structure is intended for storage in a srmw_fifo_t.
struct vfs_sics_t
{
    vfs_sics_t        *Next;         /// Pointer to the next node in the queue.
    HANDLE             Fildes;       /// The file descriptor of the opened file.
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
    int32_t            FileType;     /// One of file_type_e indicating the file type.
    DWORD              OSError;      /// The error code returned by the operation, or 0.
    stream_decoder_t  *Decoder;      /// The stream decoder state.
};

/// @summary Defines the data associated with an end-of-stream notification.
struct vfs_sies_t
{
    intptr_t           ASID;         /// The application-defined ID for the stream.
    int32_t            Behavior;     /// The configured behavior of the stream.
};

/// @summary Defines the data associated with a stream-in buffer return notification.
struct vfs_sibr_t
{
    intptr_t           ASID;         /// The application-defined ID for the stream.
    void              *DataBuffer;   /// The data buffer being returned.
};

/// @summary Defines the data associated with a stream-out write request passed
/// to the VFS driver. This structure is intended for storage in a srmw_fifo_t.
struct vfs_sowr_t
{
    vfs_sowr_t        *Next;         /// Pointer to the next node in the queue.
    HANDLE             Fildes;       /// The file descriptor of the opened file.
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
    HANDLE             Fildes;       /// The file descriptor of the opened file.
    uint32_t           Priority;     /// The stream priority (0 = highest).
    WCHAR             *FilePath;     /// The target path, allocated with malloc(), or NULL.
    int64_t            FileSize;     /// The logical size of the file, in bytes.
};

typedef srmw_fifo_t<vfs_sowr_t>                   vfs_sowriteq_t;   /// A stream-out write queue.
typedef srmw_fifo_t<vfs_socs_t>                   vfs_socloseq_t;   /// A stream-out close queue.
typedef srmw_fifo_t<vfs_siop_t>                   vfs_sicommandq_t; /// A stream-in command queue.
typedef srmw_fifo_t<vfs_sics_t>                   vfs_sicreateq_t;  /// A stream-in create queue.
typedef srsw_fifo_t<vfs_sird_t, AIO_MAX_ACTIVE*2> vfs_siresultq_t;  /// A stream-in result queue.
typedef srsw_fifo_t<vfs_sibr_t, AIO_MAX_ACTIVE*2> vfs_sireturnq_t;  /// A stream-in return queue.
typedef srsw_fifo_t<vfs_sies_t, MAX_STREAMS_IN>   vfs_siendq_t;     /// A stream-in end-of-stream queue.

/// @summary Information that remains constant from the point that a file is opened for stream-in.
struct vfs_siinfo_t
{
    HANDLE             Fildes;       /// The file descriptor for the file.
    int64_t            FileSize;     /// The physical file size, in bytes.
    int64_t            DataSize;     /// The file size after any size-changing transforms.
    int64_t            FileOffset;   /// The absolute byte offset of the start of the file data.
    size_t             SectorSize;   /// The disk physical sector size, in bytes.
    int32_t            EndBehavior;  /// The end-of-stream behavior, one of stream_in_mode_e.
    int32_t            FileType;     /// The stream-in file type, one of file_type_e.
};

/// @summary Status information associated with a 'live' stream-in. This information
/// is required to properly process (for example) close operations, where there may be
/// one or more in-progress read operations against the file; in which case the close
/// must be deferred until all in-progress read operations have completed.
struct vfs_sistat_t
{
    uint32_t           StatusFlags;  /// A combination of vfs_stream_in_status_e.
    uint32_t           NLiveIoOps;   /// The number of pending AIO operations against the stream.
    uint32_t           NLiveDecode;  /// The number of pending decode operations against the stream.
    uint32_t           Priority;     /// The priority value of the stream.
    stream_decoder_t  *Decoder;      /// The decoder state for the stream.
};

/// @summary Defines the data associated with a priority queue of pending AIO operations.
struct vfs_io_opq_t
{
    int32_t            Count;                        /// The number of items in the queue.
    uint64_t           InsertionId;                  /// The counter for tagging each AIO request.
    uint32_t           Priority[AIO_MAX_ACTIVE];     /// The priority value for each item.
    uint64_t           InsertId[AIO_MAX_ACTIVE];     /// The inserion order value for each item.
    aio_req_t          Request [AIO_MAX_ACTIVE];     /// The populated AIO request for each item.
};

/// @summary Defines the data associated with a priority queue of files. This queue
/// is used to determine which files get a chance to submit I/O operations.
struct vfs_io_fpq_t
{
    int32_t            Count;                        /// The number of items in the queue.
    uint32_t           Priority[MAX_STREAMS_IN];     /// The priority value for each file.
    uint32_t           RecIndex[MAX_STREAMS_IN];     /// The packed index of the stream record.
};

/// @summary Defines the state data maintained by a VFS driver instance.
struct vfs_state_t
{
    vfs_sowriteq_t     StOutWriteQ;                  /// Stream-out write operation queue.
    vfs_socloseq_t     StOutCloseQ;                  /// Stream-out close operation queue.
    vfs_sicommandq_t   StInCommandQ;                 /// Stream-in playback command queue.
    vfs_sicreateq_t    StInCreateQ;                  /// Stream-in create operation queue.
    iobuf_alloc_t      IoAllocator;                  /// The I/O buffer allocator.
    vfs_io_opq_t       IoOperations;                 /// The priority queue of all pending I/O operations.
    size_t             ActiveCount;                  /// The number of open files.
    intptr_t           StInASID[MAX_STREAMS_IN];     /// An application-defined ID for each active file.
    uint32_t           Priority[MAX_STREAMS_IN];     /// The access priority for each active file.
    int64_t            RdOffset[MAX_STREAMS_IN];     /// The current read offset for each active file.
    vfs_siinfo_t       StInInfo[MAX_STREAMS_IN];     /// The constant data for each active stream-in.
    vfs_siresultq_t    SiResult[THREAD_ID_COUNT];    /// The per-file type queue for stream-in I/O results.
    vfs_sireturnq_t    SiReturn[THREAD_ID_COUNT];    /// The per-file type queue for stream-in I/O buffer returns.
    vfs_siendq_t       SiEndOfS[THREAD_ID_COUNT];    /// The per-file type queue for stream-in end-of-stream events.
    size_t             LiveCount;                    /// The number of 'live' stream-in objects.
    intptr_t           LiveASID[MAX_STREAMS_LIVE];   /// The stream ID for each 'live' stream-in.
    vfs_sistat_t       LiveStat[MAX_STREAMS_LIVE];   /// The current status for each 'live' stream-in.
};

/// @summary Statistics tracked by the platform I/O system.
struct io_stats_t
{
    uint64_t           Counts[IO_COUNT_COUNT];       /// I/O driver event counters.
    uint64_t           Errors[IO_ERROR_COUNT];       /// I/O driver error counters.
    uint64_t           Stalls[IO_STALL_COUNT];       /// I/O driver stall counters.
    double             Rates [IO_RATE_COUNT];        /// I/O driver rate buckets.
    uint64_t           StartTimeNanos;               /// Nanosecond timestamp at which stats were initialized.
};

/// @summary Defines the data associated with a file opened for buffered, synchronous I/O.
struct file_t
{
    HANDLE             Fildes;       /// The file descriptor for the file.
};

/// @summary Defines the state information associated with a writable file. This
/// information is modified only by the thread that calls platform_create_file().
struct stream_writer_t
{
    HANDLE             Fildes;       /// The file descriptor for the file.
    uint8_t           *BaseAddress;  /// The base address of the current buffer.
    size_t             DataOffset;   /// The write pointer offset from the start of the buffer.
    int64_t            FileOffset;   /// The absolute offset of the write pointer within the file.
    uint32_t           Priority;     /// The stream priority (0 = highest).
};

/*///////////////
//   Globals   //
///////////////*/
/// @summary The frequency of the high-resolution timer on the system.
global_variable LARGE_INTEGER CLOCK_FREQUENCY  = {0};

/// @summary A list of all of the thread IDs we consider to be valid.
global_variable thread_id_e THREAD_ID_LIST[THREAD_ID_COUNT] = {
    THREAD_ID_ANY
};

/// @summary A list of printable names for each valid thread identifier.
global_variable char const *THREAD_ID_NAME[THREAD_ID_COUNT] = {
    "ANY" , /* THREAD_ID_ANY */
};

/// @summary A list of all of the file type identifiers we consider to be valid.
global_variable file_type_e   FILE_TYPE_LIST[] = {
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

/// @summary A list of all of the valid I/O statistic counters.
global_variable io_count_e  IO_COUNT_LIST[IO_COUNT_COUNT] = {
    IO_COUNT_STREAM_IN_OPEN,
    IO_COUNT_STREAM_IN_OPEN_ONCE,
    IO_COUNT_STREAM_IN_OPEN_LOOP,
    IO_COUNT_STREAM_IN_EOS,
    IO_COUNT_STREAM_IN_REWIND,
    IO_COUNT_STREAM_IN_SEEK,
    IO_COUNT_STREAM_IN_STOP,
    IO_COUNT_STREAM_IN_PAUSE,
    IO_COUNT_STREAM_IN_RESUME,
    IO_COUNT_READS_STARTED,
    IO_COUNT_READS_COMPLETE_SUCCESS,
    IO_COUNT_READS_COMPLETE_ERROR,
    IO_COUNT_WRITES_STARTED,
    IO_COUNT_WRITES_COMPLETE_SUCCESS,
    IO_COUNT_WRITES_COMPLETE_ERROR,
    IO_COUNT_FLUSHES_STARTED,
    IO_COUNT_FLUSHES_COMPLETE_SUCCESS,
    IO_COUNT_FLUSHES_COMPLETE_ERROR,
    IO_COUNT_CLOSES_STARTED,
    IO_COUNT_CLOSES_COMPLETE_SUCCESS,
    IO_COUNT_CLOSES_COMPLETE_ERROR,
    IO_COUNT_BYTES_READ_REQUEST,
    IO_COUNT_BYTES_READ_ACTUAL,
    IO_COUNT_BYTES_WRITE_REQUEST,
    IO_COUNT_BYTES_WRITE_ACTUAL,
    IO_COUNT_NANOS_ELAPSED_AIO,
    IO_COUNT_NANOS_ELAPSED_VFS,
    IO_COUNT_TICKS_ELAPSED_AIO,
    IO_COUNT_TICKS_ELAPSED_VFS,
    IO_COUNT_MAX_OPS_QUEUED,
    IO_COUNT_MAX_STREAM_IN_BYTES_USED,
    IO_COUNT_STREAM_IN_BYTES_USED,
    IO_COUNT_MAX_TICK_DURATION_AIO,
    IO_COUNT_MAX_TICK_DURATION_VFS,
    IO_COUNT_MIN_TICK_DURATION_AIO,
    IO_COUNT_MIN_TICK_DURATION_VFS
};

/// @summary A list of printable names for each valid I/O statistic counter.
global_variable char const *IO_COUNT_NAME[IO_COUNT_COUNT] = {
    "Stream-In Opens",
    "Stream-In Opens (ONCE)",
    "Stream-In Opens (LOOP)",
    "Stream-In End-of-Stream",
    "Stream-In Rewinds",
    "Stream-In Seeks",
    "Stream-In Stops",
    "Stream-In Pauses",
    "Stream-In Resumes",
    "I/O Reads Started",
    "I/O Reads Succeeded",
    "I/O Reads Failed",
    "I/O Writes Started",
    "I/O Writes Succeeded",
    "I/O Writes Failed",
    "I/O Flushes Started",
    "I/O Flushes Succeeded",
    "I/O Flushes Failed",
    "I/O Closes Started",
    "I/O Closes Succeeded",
    "I/O Closes Failed",
    "I/O Read Bytes Requested",
    "I/O Read Bytes Actual",
    "I/O Write Bytes Requested",
    "I/O Write Bytes Actual",
    "Nanoseconds in aio_tick()",
    "Nanoseconds in vfs_tick()",
    "Number of AIO ticks",
    "Number of VFS ticks",
    "Maximum In-Flight I/O Ops",
    "Maximum Stream-In Buffer",
    "Stream-In Bytes Used",
    "Maximum AIO tick duration",
    "Maximum VFS tick duration",
    "Minimum AIO tick duration",
    "Minimum VFS tick duration"
};

/// @summary A list of all of the valid I/O error counters.
global_variable io_error_e  IO_ERROR_LIST[IO_ERROR_COUNT] = {
    IO_ERROR_FULL_READRESULTS,
    IO_ERROR_FULL_WRITERESULTS,
    IO_ERROR_FULL_CLOSERESULTS,
    IO_ERROR_FULL_RESULTQUEUE,
    IO_ERROR_INVALID_AIO_CMD,
    IO_ERROR_ORPHANED_IOCB
};

/// @summary A list of printable names for each valid I/O error counter.
global_variable char const *IO_ERROR_NAME[IO_ERROR_COUNT] = {
    "Full ReadResults Queue",
    "Full WriteResults Queue",
    "Full CloseResults Queue",
    "Full Thread Result Queue",
    "Invalid AIO Command ID",
    "Orphaned struct iocb"
};

/// @summary A list of all of the valid I/O stall counters.
global_variable io_stall_e  IO_STALL_LIST[IO_STALL_COUNT] = {
    IO_STALL_FULL_AIO_QUEUE,
    IO_STALL_FULL_VFS_QUEUE,
    IO_STALL_OUT_OF_IOBUFS
};

/// @summary A list of printable names for each valid I/O stall counter.
global_variable char const *IO_STALL_NAME[IO_STALL_COUNT] = {
    "Full AIO Operation Queue",
    "Full VFS Operation Queue",
    "Out of Stream-In Buffers"
};

/// @summary A list of all of the valid I/O rate slots.
global_variable io_rate_e   IO_RATE_LIST [IO_RATE_COUNT]  = {
    IO_RATE_BYTES_PER_SEC_IN,
    IO_RATE_BYTES_PER_SEC_OUT
};

/// @summary A list of printable names for each valid I/O rate slot.
global_variable char const *IO_RATE_NAME [IO_RATE_COUNT]  = {
    "Stream-In Bytes/Second",
    "Stream-Out Bytes/Second"
};

/// @summary The state of our VFS process (part of the I/O system).
global_variable vfs_state_t VFS_STATE;

/// @summary The state of our AIO process (part of the I/O system).
global_variable aio_state_t AIO_STATE;

/// @summary Profiling and statistic information for the I/O system.
global_variable io_stats_t  IO_STATS;

// The following functions are not available under MinGW, so kernel32.dll is
// loaded and these functions will be resolved manually.
typedef void (WINAPI *GetNativeSystemInfoFn)(SYSTEM_INFO*);
typedef BOOL (WINAPI *SetProcessWorkingSetSizeExFn)(HANDLE, SIZE_T, SIZE_T, DWORD);
typedef BOOL (WINAPI *SetFileInformationByHandleFn)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
typedef BOOL (WINAPI *GetQueuedCompletionStatusExFn)(HANDLE, LPOVERLAPPED_ENTRY, ULONG, PULONG, DWORD, BOOL);
typedef DWORD(WINAPI *GetFinalPathNameByHandleFn)(HANDLE, LPWSTR, DWORD, DWORD);

global_variable GetNativeSystemInfoFn         GetNativeSystemInfo_Func         = NULL;
global_variable GetFinalPathNameByHandleFn    GetFinalPathNameByHandle_Func    = NULL;
global_variable SetProcessWorkingSetSizeExFn  SetProcessWorkingSetSizeEx_Func  = NULL;
global_variable SetFileInformationByHandleFn  SetFileInformationByHandle_Func  = NULL;
global_variable GetQueuedCompletionStatusExFn GetQueuedCompletionStatusEx_Func = NULL;

/*///////////////////////
//   Local Functions   //
///////////////////////*/
/// @summary Redirect a call to GetNativeSystemInfo to GetSystemInfo.
/// @param sys_info The SYSTEM_INFO structure to populate.
internal_function void WINAPI GetNativeSystemInfo_Fallback(SYSTEM_INFO *sys_info)
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
internal_function BOOL WINAPI SetProcessWorkingSetSizeEx_Fallback(HANDLE process, SIZE_T minimum, SIZE_T maximum, DWORD /*flags*/)
{
    return SetProcessWorkingSetSize(process, minimum, maximum);
}

/// @summary Loads function entry points that may not be available at compile
/// time with some build environments.
internal_function void resolve_kernel_apis(void)
{   // it's a safe assumption that kernel32.dll is mapped into our process
    // address space already, and will remain mapped for the duration of execution.
    // note that some of these APIs are Vista/WS2008+ only, so make sure that we
    // have an acceptable fallback in each case to something available earlier.
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (kernel != NULL)
    {
        GetNativeSystemInfo_Func         = (GetNativeSystemInfoFn)          GetProcAddress(kernel, "GetNativeSystemInfo");
        GetFinalPathNameByHandle_Func    = (GetFinalPathNameByHandleFn)     GetProcAddress(kernel, "GetFinalPathNameByHandleW");
        SetProcessWorkingSetSizeEx_Func  = (SetProcessWorkingSetSizeExFn)   GetProcAddress(kernel, "SetProcessWorkingSetSizeEx");
        SetFileInformationByHandle_Func  = (SetFileInformationByHandleFn)   GetProcAddress(kernel, "SetFileInformationByHandle");
        GetQueuedCompletionStatusEx_Func = (GetQueuedCompletionStatusExFn)  GetProcAddress(kernel, "GetQueuedCompletionStatusEx");
    }
    // fallback if any of these APIs are not available.
    if (GetNativeSystemInfo_Func        == NULL) GetNativeSystemInfo_Func = GetNativeSystemInfo_Fallback;
    if (SetProcessWorkingSetSizeEx_Func == NULL) SetProcessWorkingSetSizeEx_Func = SetProcessWorkingSetSizeEx_Fallback;
    // TODO: should fail if Vista+ APIs are not available. Or not be lazy and implement fallbacks.
    // for GetFinalPathNameByHandle, see:
    // cbloomrants.blogspot.com/2012/12/12-21-12-file-handle-to-file-name-on.html
    // GetQueuedCompletionStatusEx is Vista+ only also.
}

/// @summary Elevates the privileges for the process to include the privilege
/// SE_MANAGE_VOLUME_NAME, so that SetFileValidData() can be used to initialize
/// a file without having to zero-fill the underlying sectors. This is optional,
/// and we don't want it failing to prevent the application from launching.
internal_function void elevate_process_privileges(void)
{
    TOKEN_PRIVILEGES tp;
    HANDLE        token;
    LUID           luid;

    OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token);
    if (LookupPrivilegeValue(NULL, SE_MANAGE_VOLUME_NAME, &luid))
    {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL))
        {   // Privilege adjustment succeeded.
            CloseHandle(token);
        }
        else
        {   // Privilege adjustment failed.
            CloseHandle(token);
        }
    }
    else
    {   // lookup of SE_MANAGE_VOLUME_NAME failed.
        CloseHandle(token);
    }
}

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

/// @summary Initializes the high-resolution timer.
internal_function inline void inittime(void)
{
    QueryPerformanceFrequency(&CLOCK_FREQUENCY);
}

/// @summary Reads the current tick count for use as a timestamp.
/// @return The current timestamp value, in nanoseconds.
internal_function inline uint64_t nanotime(void)
{
    LARGE_INTEGER tsc = {0};
    LARGE_INTEGER tsf = CLOCK_FREQUENCY;
    QueryPerformanceCounter(&tsc);
    return (SEC_TO_NANOSEC * uint64_t(tsc.QuadPart) / uint64_t(tsf.QuadPart));
}

/// @summary Convert a value specified in nanoseconds to a value specified in seconds.
/// @param nanos The nanoseconds value.
/// @return The input value, converted to seconds.
internal_function inline double seconds(uint64_t nanos)
{
    return double(nanos) / double(SEC_TO_NANOSEC);
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
        DeleteCriticalSection(&list.Lock);
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
internal_function inline void srmw_freelist_put(srmw_freelist_t<T> &list, T *node)
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
internal_function bool create_srmw_fifo(srmw_fifo_t<T> *fifo, size_t capacity)
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
internal_function bool srmw_fifo_get(srmw_fifo_t<T> *fifo, T &item)
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
internal_function bool srmw_fifo_put(srmw_fifo_t<T> *fifo, T const &item)
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
internal_function bool create_iobuf_allocator(iobuf_alloc_t &alloc, size_t total_size, size_t alloc_size)
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
internal_function void delete_iobuf_allocator(iobuf_alloc_t &alloc)
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
{
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
            uint32_t  temp_p  = pq->Priority[pos];
            uint64_t  temp_i  = pq->InsertId[pos];
            aio_req_t temp_r  = pq->Request [pos];
            pq->Priority[pos] = pq->Priority[m];
            pq->InsertId[pos] = pq->InsertId[m];
            pq->Request [pos] = pq->Request [m];
            pq->Priority[m]   = temp_p;
            pq->InsertId[m]   = temp_i;
            pq->Request [m]   = temp_r;
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
/// @param index_a The zero-based index of the stream record in the active list.
/// @param index_l The zero-based index of the stream record in the live list.
/// @return true if the item was inserted in the queue, or false if the queue is full.
internal_function bool io_fpq_put(vfs_io_fpq_t *pq, uint32_t priority, size_t index_a, size_t index_l)
{
    if (pq->Count < MAX_STREAMS_IN)
    {   // there's room in the queue for this operation.
        uint32_t id = uint32_t(index_a << 16) | uint32_t(index_l);
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
        pq->RecIndex[pos] = id;
        return true;
    }
    else return false;
}

/// @summary Retrieves the highest priority active file.
/// @param pq The priority queue to update.
/// @param index_a On return, this location is updated with the stream index in the active list.
/// @param index_l On return, this location is updated with the stream index in the live list.
/// @param priority On return, this location is updated with the file priority.
/// @return true if a file was retrieved, or false if the queue is empty.
internal_function bool io_fpq_get(vfs_io_fpq_t *pq, size_t &index_a, size_t &index_l, uint32_t &priority)
{
    if (pq->Count > 0)
    {   // the highest-priority operation is located at index 0.
        priority  = pq->Priority[0];
        index_a   = pq->RecIndex[0] >> 16;
        index_l   = pq->RecIndex[0]  & 0xFFFF;

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
            uint32_t temp_i   = pq->RecIndex[pos];
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

/// @summary Retrieve the physical sector size for a block-access device.
/// @param file The handle to an open file on the device.
/// @return The size of a physical sector on the specified device.
internal_function size_t physical_sector_size(HANDLE file)
{   // http://msdn.microsoft.com/en-us/library/ff800831(v=vs.85).aspx
    // for structure STORAGE_ACCESS_ALIGNMENT
    // Vista and Server 2008+ only - XP not supported.
    size_t const DefaultPhysicalSectorSize = 4096;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR desc;
    STORAGE_PROPERTY_QUERY    query;
    memset(&desc  , 0, sizeof(desc));
    memset(&query , 0, sizeof(query));

    query.QueryType  = PropertyStandardQuery;
    query.PropertyId = StorageAccessAlignmentProperty;
    DWORD bytes = 0;
    BOOL result = DeviceIoControl(
        file,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query),
        &desc , sizeof(desc),
        &bytes, NULL);
    if (!result)
    {
        return DefaultPhysicalSectorSize;
    }
    else return desc.BytesPerPhysicalSector;
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

/// @summary Generates a unique filename for use as a temp file in a given directory.
/// @param path The NULL-terminated UTF-8 string specifying the target path.
/// @param prefix The NULL-terminated UCS-2 string specifying the filename prefix.
/// @return A string <volume and directory info from path>\<prefix>-########\0.
/// The returned string should be freed using the standard C library free() function.
internal_function WCHAR* make_temp_path(char const *path, WCHAR const *prefix)
{
    int    nbytes =-1;    // the number of bytes of volume and directory info
    int    nchars = 0;
    size_t pathlen= 0;    // the number of bytes in path, not including zero-byte
    size_t dirlen = 0;    // the number of bytes of volume and directory info
    size_t pfxlen = 0;    // the number of chars in prefix
    WCHAR *temp   = NULL; // the temporary file path we will return
    WCHAR  random[9];     // zero-terminated string of 8 random hex digits

    // ensure that a prefix is specified.
    if (prefix == NULL)
    {   // use the prefix 'tempfile'.
        prefix  = L"tempfile";
    }

    // the temp file should be created in the same directory as the output path.
    // this avoids problems with file moves across volumes or partitions, which
    // will either fail, or are implemented as a non-atomic file copy. the path
    // we return will contain the full volume and directory information from the
    // input path, and append a filename of the form prefix-######## to it.
    pathend(path, dirlen, pathlen);          // get the path portion of the input string.
    pfxlen = wcslen(prefix);                 // get the length of the prefix string.
    nbytes = dirlen > 0 ? int(dirlen)  : -1; // get the number of bytes of volume and directory info.
    nchars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, nbytes, NULL, 0);
    if (dirlen > 0 && nchars == 0)
    {   // the path cannot be converted from UTF-8 to UCS-2.
        return NULL;
    }
    // allocate storage for the path string, converted to UCS-2.
    // the output path has the form:
    // <volume and directory info from path>\<prefix>-########\0
    nchars =  int(nchars + 1 + pfxlen + 10);
    temp   = (WCHAR*) malloc(nchars * sizeof(WCHAR));
    if (temp == NULL) return NULL;
    memset(temp, 0, nchars * sizeof(WCHAR));
    if (dirlen > 0)
    {   // the output path has volume and directory information specified.
        // convert this data from UTF-8 to UCS-2 and append a directory separator.
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, int(dirlen), temp, nchars);
        wcscat(temp, L"\\");
    }
    wcscat(temp, prefix);
    wcscat(temp, L"-");

    // generate a 32-bit 'random' value and convert it to an eight-digit hex value.
    // start with the current tick count, and then mix the bits using the 4-byte
    // integer hash, full avalanche method from burtleburtle.net/bob/hash/integer.html.
    uint32_t bits = GetTickCount();
    bits  = (bits + 0x7ed55d16) + (bits << 12);
    bits  = (bits ^ 0xc761c23c) ^ (bits >> 19);
    bits  = (bits + 0x165667b1) + (bits <<  5);
    bits  = (bits + 0xd3a2646c) ^ (bits <<  9);
    bits  = (bits + 0xfd7046c5) + (bits <<  3);
    bits  = (bits ^ 0xb55a4f09) ^ (bits >> 16);
    swprintf(random, 9, L"%08x" ,  bits);
    random[8]  = 0;
    wcscat(temp, random);
    return temp;
}

/// @summary Synchronously opens a file and retrieves various information.
/// @param path The path of the file to open.
/// @param iocp The I/O completion port to associate with the file handle, or NULL.
/// @param access Usually GENERIC_READ, GENERIC_WRITE, or GENERIC_READ|GENERIC_WRITE.
/// See MSDN documentation for CreateFile, dwDesiredAccess parameter.
/// @param share Usually FILE_SHARE_READ, FILE_SHARE_WRITE, or FILE_SHARE_READ|FILE_SHARE_WRITE.
/// See MSDN documentation for CreateFile, dwShareMode parameter.
/// @param create Usually OPEN_EXISTNG or OPEN_ALWAYS. See MSDN documentation
/// for CreateFile, dwCreationDisposition parameter.
/// @param flags Usually FILE_FLAG_NO_BUFFERING|FILE_FLAG_OVERLAPPED. See MSDN
/// documentation for CreateFile, dwFlagsAndAttributes parameter.
/// @param fd On return, this location is set to the file handle, or INVALID_HANDLE_VALUE.
/// @param file_size On return, this value is set to the current size of the file, in bytes.
/// @param sector_size On return, this value is set to the size of the physical disk sector, in bytes.
/// @return true if all operations were successful.
internal_function bool open_file_raw(char const *path, HANDLE iocp, DWORD access, DWORD share, DWORD create, DWORD flags, HANDLE &fd, int64_t &file_size, size_t &sector_size)
{
    LARGE_INTEGER fsize   = {0};
    HANDLE        hFile   = INVALID_HANDLE_VALUE;
    WCHAR        *pathbuf = NULL;
    size_t        ssize   = 0;
    int           nchars  = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);

    // convert the path from UTF-8 to UCS-2, which Windows requires.
    if (nchars == 0)
    {   // the path cannot be converted from UTF-8 to UCS-2.
        goto error_cleanup;
    }

    pathbuf = (WCHAR*) malloc(nchars * sizeof(WCHAR));
    if (pathbuf != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, pathbuf, nchars) == 0)
    {   // the path cannot be converted from UTF-8 to UCS-2.
        goto error_cleanup;
    }

    // open the file as specified by the caller.
    hFile = CreateFile(pathbuf, access, share, NULL, create, flags, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {   // the file cannot be opened.
        goto error_cleanup;
    }

    // retrieve basic file attributes.
    ssize = physical_sector_size(hFile);
    if (!GetFileSizeEx(hFile, &fsize))
    {   // the file size cannot be retrieved.
        goto error_cleanup;
    }

    // associate the file handle to the I/O completion port.
    if (iocp != NULL && CreateIoCompletionPort(hFile, iocp, 0, 0) != iocp)
    {   // the file handle could not be associated with the IOCP.
        goto error_cleanup;
    }

    // set output parameters and clean up.
    fd          = hFile;
    file_size   = fsize.QuadPart;
    sector_size = ssize;
    free(pathbuf);
    return true;

error_cleanup:
    if (hFile   != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (pathbuf != NULL) free(pathbuf);
    fd = INVALID_HANDLE_VALUE;
    file_size = 0;
    sector_size = 0;
    return false;
}

/// @summary Closes the file descriptors associated with a file.
/// @param fd The raw file descriptor of the underlying file. On return, set to INVALID_HANDLE_VALUE.
internal_function void close_file_raw(HANDLE &fd)
{
    if (fd != INVALID_HANDLE_VALUE)
    {
        CloseHandle(fd);
        fd = INVALID_HANDLE_VALUE;
    }
}

/// @summary Increments an I/O statistics counter.
/// @param stats The I/O statistics to update.
/// @param count_id The counter ID, one of io_count_e.
internal_function inline void io_count(io_stats_t *stats, int count_id)
{
    stats->Counts[count_id]++;
}

/// @summary Directly assigns an I/O statistics counter.
/// @param stats The I/O statistics to update.
/// @param count_id The counter ID, one of io_count_e.
/// @param value The value to assign to the counter.
internal_function inline void io_count_assign(io_stats_t *stats, int count_id, uint64_t value)
{
    stats->Counts[count_id] = value;
}

/// @summary Assigns a counter to the specified value if the value is less than
/// the current value of the counter.
/// @param stats The I/O statistics to update.
/// @param count_id The counter ID, one of io_count_e.
/// @param value The value to test and assigned to the counter.
internal_function inline void io_count_assign_min(io_stats_t *stats, int count_id, uint64_t value)
{
    uint64_t      min_value = stats->Counts[count_id] < value ? stats->Counts[count_id] : value;
    stats->Counts[count_id] = min_value;
}

/// @summary Assigns a counter to the specified value if the value is greater
/// than the current value of the counter.
/// @param stats The I/O statistics to update.
/// @param count_id The counter ID, one of io_count_e.
/// @param value The value to test and assigned to the counter.
internal_function inline void io_count_assign_max(io_stats_t *stats, int count_id, uint64_t value)
{
    uint64_t      max_value = stats->Counts[count_id] > value ? stats->Counts[count_id] : value;
    stats->Counts[count_id] = max_value;

}

/// @summary Increments an I/O statistics counter by a given amount.
/// @param stats The I/O statistics to update.
/// @param count_id The counter ID, one of io_count_e.
/// @param amount The amount by which the counter should be incremented.
internal_function inline void io_count_increment(io_stats_t *stats, int count_id, uint64_t amount)
{
    stats->Counts[count_id] += amount;
}

/// @summary Increments an I/O error counter.
/// @param stats The I/O statistics to update.
/// @param error_id The counter ID, one of io_error_e.
internal_function inline void io_error(io_stats_t *stats, int error_id)
{
    stats->Errors[error_id]++;
}

/// @summary Increments an I/O stall counter.
/// @param stats The I/O statistics to update.
/// @param stall_id The counter ID, one of io_stall_e.
internal_function inline void io_stall(io_stats_t *stats, int stall_id)
{
    stats->Stalls[stall_id]++;
}

/// @summary Assigns an I/O rate counter.
/// @param stats The I/O statistics to update.
/// @param rate_id The counter ID, one of io_rate_e.
/// @param value The computed rate value.
internal_function inline void io_rate(io_stats_t *stats, int rate_id, double value)
{
    stats->Rates[rate_id] = value;
}

/// @summary Given a file type, return the ID of the thread on which I/O should be processed.
/// @param file_type One of the values of the file_type_e enumeration.
/// @return One of the values of the thread_id_e enumeration indicating the thread on which
/// completed I/O should be processed for the given file type.
internal_function int32_t io_thread_for_file_type(int32_t file_type)
{
    switch (file_type)
    {
        case FILE_TYPE_DDS:
        case FILE_TYPE_TGA:
        case FILE_TYPE_WAV:
        case FILE_TYPE_JSON:
            return THREAD_ID_ANY;

        default:
            break;
    }
    return THREAD_ID_ANY;
}

/// @summary Resets the platform I/O statistics to zero.
/// @param stats The counters to reset.
internal_function void init_io_stats(io_stats_t *stats)
{
    if (stats != NULL)
    {
        for (size_t i = 0; i < IO_COUNT_COUNT; ++i) stats->Counts[i] = 0;
        for (size_t i = 0; i < IO_ERROR_COUNT; ++i) stats->Errors[i] = 0;
        for (size_t i = 0; i < IO_STALL_COUNT; ++i) stats->Stalls[i] = 0;
        for (size_t i = 0; i < IO_RATE_COUNT ; ++i) stats->Rates [i] = 0.0;
        stats->StartTimeNanos = nanotime();
    }
}

/// @summary Pretty-prints a set of I/O system counters to the specified stream.
/// @param fp The output stream.
/// @param stats The set of I/O system counters to format.
internal_function void print_io_stats(FILE *fp, io_stats_t const *stats)
{
    for (size_t i = 0; i < IO_COUNT_COUNT; ++i)
    {
        fprintf(fp, "Count: %30s    %" PRIu64 "\n", IO_COUNT_NAME[i], stats->Counts[i]);
    }
    for (size_t i = 0; i < IO_ERROR_COUNT; ++i)
    {
        fprintf(fp, "Error: %30s    %" PRIu64 "\n", IO_ERROR_NAME[i], stats->Errors[i]);
    }
    for (size_t i = 0; i < IO_STALL_COUNT; ++i)
    {
        fprintf(fp, "Stall: %30s    %" PRIu64 "\n", IO_STALL_NAME[i], stats->Stalls[i]);
    }
    for (size_t i = 0; i < IO_RATE_COUNT ; ++i)
    {
        fprintf(fp, "Rate : %30s    %0.3f\n", IO_RATE_NAME[i], stats->Rates[i]);
    }
    fprintf(fp, "TOTAL SECONDS ELAPSED: %0.3f\n", seconds(nanotime() - stats->StartTimeNanos));
}

/// @summary Pretty-prints the system I/O streaming rates to the specified stream.
/// @param fp The output stream.
/// @param stats The set of I/O system counters to format.
internal_function void print_io_rates(FILE *fp, io_stats_t const *stats)
{
    fprintf(fp, "Stream-In Bytes/Sec: %0.3f    Stream-Out Bytes/Sec: %0.3f\r",
            stats->Rates[IO_RATE_BYTES_PER_SEC_IN],
            stats->Rates[IO_RATE_BYTES_PER_SEC_OUT]);
}

/// @summary Allocates an iocb instance from the free list.
/// @param aio The AIO driver state managing the free list.
/// @return The next available iocb structure.
internal_function inline OVERLAPPED* asio_get(aio_state_t *aio)
{
    assert(aio->ASIOFreeCount > 0);
    return aio->ASIOFree[--aio->ASIOFreeCount];
}

/// @summary Returns an iocb instance to the free list.
/// @param aio The AIO driver state managing the free list.
/// @param asio The OVERLAPPED instance to return to the free list.
internal_function inline void asio_put(aio_state_t *aio, OVERLAPPED *asio)
{
    assert(aio->ASIOFreeCount < AIO_MAX_ACTIVE);
    aio->ASIOFree[aio->ASIOFreeCount++] = asio;
}

/// @summary Helper function to build an AIO result packet.
/// @param error The error code to return.
/// @param amount The amount of data returned.
/// @param req The request associated with the result.
/// @return The populated AIO result packet.
internal_function inline aio_res_t aio_result(DWORD error, uint32_t amount, aio_req_t const &req)
{
    aio_res_t res = {
        req.Fildes,      /* Fildes     */
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

/// @summary Builds a read operation and submits it to the kernel.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the read operation.
/// @param error On return, this location stores the error return value.
/// @param stats The I/O driver statistics to update.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
internal_function int aio_submit_read(aio_state_t *aio, aio_req_t const &req, DWORD &error, io_stats_t *stats)
{
    int64_t absolute_ofs = req.BaseOffset + req.FileOffset; // relative->absolute
    OVERLAPPED     *asio = asio_get(aio);
    DWORD           xfer = 0;
    asio->Internal       = 0;
    asio->InternalHigh   = 0;
    asio->Offset         = DWORD((absolute_ofs & 0x00000000FFFFFFFFULL) >>  0);
    asio->OffsetHigh     = DWORD((absolute_ofs & 0xFFFFFFFFFFFFFFFFULL) >> 32);
    BOOL res = ReadFile(req.Fildes, req.DataBuffer, req.DataAmount, &xfer, asio);
    if (!res || xfer == 0)
    {   // the common case is that GetLastError() returns ERROR_IO_PENDING,
        // which means that the operation will complete asynchronously.
        DWORD err  = GetLastError();
        if   (err == ERROR_IO_PENDING)
        {   // the operation was queued by kernel AIO. append to the active list.
            size_t index = aio->ActiveCount++;
            aio->AAIOList[index] = req;
            aio->AAIOList[index].ATimeNanos = nanotime();
            aio->ASIOList[index] = asio;
            error = ERROR_SUCCESS;
            return (0);
        }
        else if (err == ERROR_HANDLE_EOF)
        {   // this is not considered to be an error. complete immediately.
            error = ERROR_SUCCESS;
            aio_res_t res = aio_result(ERROR_SUCCESS, 0, req);
            return srsw_fifo_put(&aio->ReadResults, res) ? 0 : -1;
        }
        else
        {   // an error has occurred. complete immediately with error.
            error = err;
            aio_res_t res = aio_result(err, 0, req);
            srsw_fifo_put(&aio->ReadResults, res);
            return (-1);
        }
    }
    else
    {   // the read operation has completed synchronously. complete immediately.
        error = ERROR_SUCCESS;
        aio_res_t res = aio_result(ERROR_SUCCESS, xfer, req);
        return srsw_fifo_put(&aio->ReadResults, res) ? 0 : -1;
    }
}

/// @summary Builds a write operation and submits it to the kernel.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the read operation.
/// @param error On return, this location stores the error return value.
/// @param stats The I/O driver statistics to update.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
internal_function int aio_submit_write(aio_state_t *aio, aio_req_t const &req, DWORD &error, io_stats_t *stats)
{
    int64_t absolute_ofs = req.BaseOffset + req.FileOffset; // relative->absolute
    OVERLAPPED     *asio = asio_get(aio);
    DWORD           xfer = 0;
    asio->Internal       = 0;
    asio->InternalHigh   = 0;
    asio->Offset         = DWORD((absolute_ofs & 0x00000000FFFFFFFFULL) >>  0);
    asio->OffsetHigh     = DWORD((absolute_ofs & 0xFFFFFFFFFFFFFFFFULL) >> 32);
    BOOL res = WriteFile(req.Fildes, req.DataBuffer, req.DataAmount, &xfer, asio);
    if (!res || xfer == 0)
    {   // the common case is that GetLastError() returns ERROR_IO_PENDING,
        // which means that the operation will complete asynchronously.
        DWORD err  = GetLastError();
        if   (err == ERROR_IO_PENDING)
        {   // the operation was queued by kernel AIO. append to the active list.
            size_t index = aio->ActiveCount++;
            aio->AAIOList[index] = req;
            aio->AAIOList[index].ATimeNanos = nanotime();
            aio->ASIOList[index] = asio;
            error = ERROR_SUCCESS;
            return (0);
        }
        else if (err == ERROR_HANDLE_EOF)
        {   // this is not considered to be an error. complete immediately.
            error = ERROR_SUCCESS;
            aio_res_t res = aio_result(ERROR_SUCCESS, 0, req);
            return srsw_fifo_put(&aio->WriteResults, res) ? 0 : -1;
        }
        else
        {   // an error has occurred. complete immediately.
            error = err;
            aio_res_t res = aio_result(err, 0, req);
            srsw_fifo_put(&aio->WriteResults, res);
            return (-1);
        }
    }
    else
    {   // the write operation has completed synchronously. complete immediately.
        error = ERROR_SUCCESS;
        aio_res_t res = aio_result(ERROR_SUCCESS, xfer, req);
        return srsw_fifo_put(&aio->WriteResults, res) ? 0 : -1;
    }
}

/// @summary Synchronously processes a file flush operation.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the read operation.
/// @param error On return, this location stores the error return value.
/// @param stats The I/O driver statistics to update.
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
internal_function int aio_process_flush(aio_state_t *aio, aio_req_t const &req, DWORD &error, io_stats_t *stats)
{   // synchronously flush all pending writes to the file.
    BOOL result = FlushFileBuffers(req.Fildes);
    if (!result)  error = GetLastError();
    else error = ERROR_SUCCESS;

    // generate the completion result and push it to the queue.
    aio_res_t res = aio_result(error, 0, req);
    return srsw_fifo_put(&aio->FlushResults, res) ? 0 : -1;
}

/// @summary Synchronously processes a file close operation.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the close operation.
/// @param stats The I/O driver statistics to update.
/// @return Zero if the result was successfully submitted, or -1 if the result queue is full.
internal_function int aio_process_close(aio_state_t *aio, aio_req_t const &req, io_stats_t *stats)
{   // close the file descriptors associated with the file.
    if (req.Fildes != INVALID_HANDLE_VALUE) CloseHandle(req.Fildes);

    // generate the completion result and push it to the queue.
    aio_res_t res = aio_result(ERROR_SUCCESS, 0, req);
    return srsw_fifo_put(&aio->CloseResults, res) ? 0 : -1;
}

/// @summary Synchronously processes a finalize request, which closes the temp
/// file and safely moves it to the destination file path.
/// @param aio The AIO driver state processing the AIO request.
/// @param req The AIO request corresponding to the finalize operation.
/// @param stats The I/O driver statistics to update.
/// @return Zero if the result was successfully submitted, or -1 if the result queue is full.
internal_function int aio_process_finalize(aio_state_t *aio, aio_req_t const &req, io_stats_t *stats)
{
    DWORD ncharsp = 0;    // number of characters in source path; GetFinalPathNameByHandle().
    size_t  ssize = 0;    // the disk physical sector size, in bytes.
    int64_t lsize = 0;    // the logical file size, in bytes
    int64_t psize = 0;    // the physical disk allocation size, in bytes.
    WCHAR *target = (WCHAR*) req.DataBuffer;
    WCHAR *source = NULL;
    FILE_END_OF_FILE_INFO eof;

    // get the absolute path of the temporary file (the source file).
    ncharsp = GetFinalPathNameByHandle_Func(req.Fildes, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    source  = (WCHAR*) malloc(ncharsp * sizeof(WCHAR));
    if (ncharsp == 0 || source == NULL)
    {   // couldn't allocate the temporary buffer for the source path.
        goto error_cleanup;
    }
    GetFinalPathNameByHandle_Func(req.Fildes, source, ncharsp, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);

    // is the temporary file being deleted, or is it being moved?
    if (target == NULL)
    {   // handle the simple case of deleting the temporary file.
        srsw_fifo_put(&aio->CloseResults, aio_result(0, 0, req));
        CloseHandle(req.Fildes);
        DeleteFile(source);
        free(source);
        return 0;
    }

    // the file needs to be moved into place. set the file size.
    lsize = req.FileOffset;
    eof.EndOfFile.QuadPart = lsize;
    ssize = physical_sector_size(req.Fildes);
    psize = align_up(req.FileOffset, ssize);
    SetFileInformationByHandle_Func(req.Fildes, FileEndOfFileInfo, &eof, sizeof(eof));
    SetFileValidData(req.Fildes, eof.EndOfFile.QuadPart); // requires elevate_process_privileges().

    // close the open file handle, and move the file into place.
    // note that goto error_cleanup can't be used after this point,
    // since we have already closed the file handle.
    CloseHandle(req.Fildes);
    if (!MoveFileEx(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {   // unable to move the file to the target location.
        srsw_fifo_put(&aio->CloseResults, aio_result(GetLastError(), 0, req));
        DeleteFile(source);
        free(source);
        free(target);
        return -1;
    }

    // finally, we're done. complete the operation successfully.
    free(source); free(target);
    return srsw_fifo_put(&aio->CloseResults, aio_result(0, 0, req)) ? 0 : -1;

error_cleanup:
    srsw_fifo_put(&aio->CloseResults, aio_result(GetLastError(), 0, req));
    if (req.Fildes != INVALID_HANDLE_VALUE) CloseHandle(req.Fildes);
    if (source != NULL) DeleteFile(source);
    if (source != NULL) free(source);
    if (target != NULL) free(target);
    return -1;
}

/// @summary Implements the main loop of the AIO driver using a polling mechanism.
/// @param aio The AIO driver state to update.
/// @param stats The I/O driver statistics to update.
/// @param timeout The timeout value indicating the amount of time to wait, or
/// INFINITE to block indefinitely. Note that aio_poll() just calls aio_tick() with
/// a timeout of zero, which will return immediately if no events are available.
/// @return Zero to continue with the next tick, 1 if the shutdown signal was received, -1 if an error occurred.
internal_function int aio_tick(aio_state_t *aio, io_stats_t *stats, DWORD timeout)
{   // poll kernel AIO for any completed events, and process them first.
    uint64_t s_nanos = nanotime();
    uint64_t e_nanos = 0;
    OVERLAPPED_ENTRY events[AIO_MAX_ACTIVE];  // STACK: 8-16KB depending on OS.
    ULONG nevents = 0;
    BOOL  iocpres = GetQueuedCompletionStatusEx_Func(aio->ASIOContext, events, AIO_MAX_ACTIVE, &nevents, timeout, FALSE);
    if (iocpres && nevents > 0)
    {   // kernel AIO reported one or more events are ready.
        for (ULONG i = 0; i < nevents; ++i)
        {
            OVERLAPPED_ENTRY &evt = events[i];
            OVERLAPPED      *asio = evt.lpOverlapped;
            size_t            idx = 0;
            bool            found = false;

            // search for the current index of this operation by
            // locating the OVERLAPPED pointer in the active list. this
            // is necessary because the active list gets reordered.
            size_t const nlive = aio->ActiveCount;
            OVERLAPPED  **list = aio->ASIOList;
            for (size_t op = 0 ; op < nlive; ++op)
            {
                if (list[op] == asio)
                {   // found the item we're looking for.
                    found = true;
                    idx   = op;
                    break;
                }
            }
            if (found)
            {
                aio_req_t req = aio->AAIOList[idx]; // make a copy of the request
                aio_res_t res = aio_result(HRESULT_FROM_NT(evt.Internal), evt.dwNumberOfBytesTransferred, req);

                // swap the last active request into this slot.
                aio->AAIOList[idx] = aio->AAIOList[nlive-1];
                aio->ASIOList[idx] = aio->ASIOList[nlive-1];
                aio->ActiveCount--;

                // create the result and enqueue it in the appropriate queue.
                // AIO_COMMAND_CLOSE is always processed synchronously and
                // will never be completed through GetQueuedCompletionStatusEx().
                // AIO_COMMAND_FLUSH is always processed synchronously and
                // will never be completed through GetQueuedCompletionStatusEx().
                switch (req.Command)
                {
                    case AIO_COMMAND_READ:
                        srsw_fifo_put(&aio->ReadResults , res);
                        break;
                    case AIO_COMMAND_WRITE:
                        srsw_fifo_put(&aio->WriteResults, res);
                        break;
                    default:
                        break;
                }

                // return the OVERLAPPED instance to the free list.
                asio_put(aio, asio);
            }
            else if (evt.lpCompletionKey == AIO_SHUTDOWN)
            {   // this is the shutdown signal.
                return 1;
            }
            else
            {   // this should never happen. this is a serious programming error.
                io_error(stats, IO_ERROR_ORPHANED_IOCB);
            }
        }
    }

    // now dequeue and submit as many AIO requests as we can.
    DWORD error  = ERROR_SUCCESS;
    DWORD result = 0;
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
                io_count(stats, IO_COUNT_READS_STARTED);
                result = aio_submit_read (aio, req, error, stats);
                break;
            case AIO_COMMAND_WRITE:
                io_count(stats, IO_COUNT_WRITES_STARTED);
                result = aio_submit_write(aio, req, error, stats);
                break;
            case AIO_COMMAND_FLUSH:
                result = aio_process_flush(aio, req, error, stats);
                break;
            case AIO_COMMAND_CLOSE:
                result = aio_process_close(aio, req, stats);
                break;
            case AIO_COMMAND_FINAL:
                result = aio_process_finalize(aio, req, stats);
                break;
            default:
                io_error(stats, IO_ERROR_INVALID_AIO_CMD);
                error  = ERROR_INVALID_PARAMETER;
                break;
        }
    }
    e_nanos = nanotime();
    io_count(stats, IO_COUNT_TICKS_ELAPSED_AIO);
    io_count_increment (stats, IO_COUNT_NANOS_ELAPSED_AIO    , e_nanos - s_nanos);
    io_count_assign_min(stats, IO_COUNT_MIN_TICK_DURATION_AIO, e_nanos - s_nanos);
    io_count_assign_max(stats, IO_COUNT_MAX_TICK_DURATION_AIO, e_nanos - s_nanos);
    return 0;
}

/// @summary Implements the main loop of the AIO driver.
/// @param aio The AIO driver state to update.
/// @param stats The I/O driver statistics to update.
/// @return Zero to continue with the next tick, 1 if the shutdown signal was received, -1 if an error occurred.
internal_function inline int aio_poll(aio_state_t *aio, io_stats_t *stats)
{   // configure a zero timeout so we won't block.
    return aio_tick(aio, stats, 0);
}

/// @summary Allocates a new AIO context and initializes the AIO state.
/// @param aio The AIO state to allocate and initialize.
/// @return 0 if the operation completed successfully; otherwise, the GetLastError value.
internal_function DWORD create_aio_state(aio_state_t *aio)
{
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (iocp == NULL)
    {   // unable to create the AIO context; everything else fails.
        return GetLastError();
    }
    // setup the iocb free list. all items are initially available.
    for (size_t i = 0, n =  AIO_MAX_ACTIVE; i < n; ++i)
    {
        aio->ASIOFree[i] = &aio->ASIOPool[i];
    }
    aio->ASIOContext   = iocp;
    aio->ActiveCount   = 0;
    aio->ASIOFreeCount = AIO_MAX_ACTIVE;
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
    if (aio->ASIOContext != NULL)
    {
        CloseHandle(aio->ASIOContext);
    }
    aio->ASIOContext   = NULL;
    aio->ActiveCount   = 0;
    aio->ASIOFreeCount = 0;
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
internal_function inline bool vfs_stat_by_asid(vfs_state_t const *vfs, intptr_t asid, size_t &index)
{
    intptr_t const  ASID      = asid;
    intptr_t const *ASIDList  = vfs->LiveASID;
    size_t   const  ASIDCount = vfs->LiveCount;
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

/// @summary Determines whether a path references a file on a remote drive.
/// @param path The NULL-terminated UTF-8 path of the file.
/// @param stats The I/O driver statistics to update.
/// @return true if the file exists on a remote drive.
internal_function bool is_remote(char const *path, io_stats_t *stats)
{   // the following prevents this function from being re-entrant.
    local_persist WCHAR hugebuf[32 * 1024];

    WCHAR  *pathbuf = NULL;
    UINT    drvtype = DRIVE_UNKNOWN;
    int     nchars  = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);

    // convert the path from UTF-8 to UCS-2, which Windows requires.
    if (nchars == 0)
    {   // the path cannot be converted from UTF-8 to UCS-2.
        return false;
    }

    pathbuf = (WCHAR*) malloc(nchars * sizeof(WCHAR));
    if (pathbuf != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, pathbuf, nchars) == 0)
    {   // the path cannot be converted from UTF-8 to UCS-2.
        if (pathbuf != NULL) free(pathbuf);
        return false;
    }

    // MSDN doesn't indicate whether GetVolumePathName() zero-terminates...
    // and it doesn't return the length of the string. so zero the huge buffer.
    // TODO: actually inspect the behavior of the function and get rid of this.
    memset(hugebuf, 0, 32 * 1024 * sizeof(WCHAR));

    // figure out what volume this file path refers to.
    if (!GetVolumePathName(pathbuf, hugebuf, 32 * 1024))
    {   // the file might not exist, etc.
        free(pathbuf);
        return false;
    }

    // finally, we can actually determine the drive type.
    drvtype = GetDriveType(hugebuf);
    free(pathbuf);
    return (drvtype == DRIVE_REMOTE);
}

/// @summary Determine whether a path references a file within an archive, (and
/// if so, which one) or whether it references a native file. Open the file if
/// necessary, and return basic file information to the caller. This function
/// should only be used for read-only files, files cannot be written in an archive.
/// @param path The NULL-terminated UTF-8 path of the file to resolve.
/// @param hints A combination of vfs_file_hint_e to control how the file is opened.
/// @param iocp The I/O completion port from the AIO driver to associate with the
/// file handle, or NULL if no I/O completion port is being used for I/O requests.
/// @param fd On return, stores the file descriptor of the archive or native file.
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
/// @param stats The I/O driver statistics to update.
/// @return true if the file could be resolved.
internal_function bool vfs_resolve_file_read(char const *path, int hints, HANDLE iocp, HANDLE &fd, int64_t &lsize, int64_t &psize, int64_t &offset, size_t &sector_size, stream_decoder_t *&decoder, io_stats_t *stats)
{   // TODO: determine whether this path references a file contained within an archive.
    // for now, we only handle native file paths, which may be absolute or relative.
    bool native_path = true;
    if  (native_path)
    {
        DWORD access = GENERIC_READ;
        DWORD share  = FILE_SHARE_READ;
        DWORD create = OPEN_EXISTING;
        DWORD flags  = FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN;
        if (((hints  & FILE_HINT_DIRECT) != 0) && is_remote(path, stats) == false)
        {   // ideally we would also check based on file size, but Windows
            // doesn't provide any way to set FILE_FLAG_NO_BUFFERING after the fact.
            // TODO: could stat the file beforehand...
            flags   |= FILE_FLAG_NO_BUFFERING;
        }
        if (open_file_raw(path, iocp, access, share, create, flags, fd, psize, sector_size))
        {   // native files always begin at the first byte.
            // logical and physical size are the same.
            decoder = new stream_decoder_t();
            lsize   = psize;
            offset  = 0;
            return true;
        }
        else
        {   // unable to open the file, so fail immediately.
            fd = INVALID_HANDLE_VALUE; lsize = psize = offset = sector_size = 0;
            decoder = NULL;
            return false;
        }
    }
    return false;
}

/// @summary Processes queued commands for creating a new stream-in.
/// @param vfs The VFS driver state.
/// @param stats The I/O driver statistics to update.
internal_function void vfs_process_sicreate(vfs_state_t *vfs, io_stats_t *stats)
{
    while (vfs->ActiveCount < MAX_STREAMS_IN)
    {
        vfs_sics_t    req;
        if (srmw_fifo_get(&vfs->StInCreateQ, req) == false)
        {   // there are no pending create requests, so we're done.
            break;
        }

        // the file is already open; it was opened during platform_open_stream().
        // all we need to do is update our internal active stream list.
        size_t index = vfs->ActiveCount++;
        vfs->StInASID[index] = req.ASID;
        vfs->Priority[index] = req.Priority;
        vfs->RdOffset[index] = 0;
        vfs->StInInfo[index].Fildes      = req.Fildes;
        vfs->StInInfo[index].FileSize    = req.FileSize;
        vfs->StInInfo[index].DataSize    = req.DataSize;
        vfs->StInInfo[index].FileOffset  = req.FileOffset;
        vfs->StInInfo[index].SectorSize  = req.SectorSize;
        vfs->StInInfo[index].EndBehavior = req.Behavior;
        vfs->StInInfo[index].FileType    = req.Type;

        // save the stream decoder state, used when processing read data.
        // maintained in a separate list; this has a different lifetime.
        index = vfs->LiveCount++;
        vfs->LiveASID[index] = req.ASID;
        vfs->LiveStat[index].StatusFlags = VFS_STATUS_NONE;
        vfs->LiveStat[index].NLiveIoOps  = 0;
        vfs->LiveStat[index].NLiveDecode = 0;
        vfs->LiveStat[index].Priority    = req.Priority;
        vfs->LiveStat[index].Decoder     = req.Decoder;

        // update statistics:
        io_count(stats, IO_COUNT_STREAM_IN_OPEN);
        if (req.Behavior == STREAM_IN_ONCE) io_count(stats, IO_COUNT_STREAM_IN_OPEN_ONCE);
        if (req.Behavior == STREAM_IN_LOOP) io_count(stats, IO_COUNT_STREAM_IN_OPEN_LOOP);
    }
}

/// @summary Processes any pending file close requests.
/// @param vfs The VFS driver state.
/// @param stats The I/O driver statistics to update.
internal_function void vfs_process_closes(vfs_state_t *vfs, io_stats_t *stats)
{
    size_t  index = 0;
    for (size_t i = 0, n = vfs->LiveCount; i < n; ++i)
    {
        vfs_sistat_t &st = vfs->LiveStat[i];
        if (st.StatusFlags & VFS_STATUS_CLOSE)
        {   // locate the stream by ID within the active stream list.
            if (vfs_find_by_asid(vfs, vfs->LiveASID[i], index) == false)
            {   // this stream isn't active, so it can't be closed.
                assert(false && "Inactive stream-in marked for close.");
                continue;
            }
            if (st.NLiveIoOps > 0)
            {   // there are pending I/O operations against this file.
                // the file cannot be closed until all operations have completed.
                // the file close will be queued after the last operation completes.
                st.StatusFlags |= VFS_STATUS_CLOSE;
                continue;
            }

            // queue the file close operation for the AIO driver.
            aio_req_t *aio_req = io_opq_put(&vfs->IoOperations, vfs->Priority[index]);
            if (aio_req != NULL)
            {   // fill out the request. it will be processed at a later time.
                aio_req->Command     = AIO_COMMAND_CLOSE;
                aio_req->Fildes      = vfs->StInInfo[index].Fildes;
                aio_req->DataAmount  = 0;
                aio_req->BaseOffset  = vfs->StInInfo[index].FileOffset;
                aio_req->FileOffset  = 0;
                aio_req->DataBuffer  = NULL;
                aio_req->QTimeNanos  = nanotime();
                aio_req->ATimeNanos  = 0;
                aio_req->AFID        = vfs->StInASID[index];
                aio_req->Type        = vfs->StInInfo[index].FileType;
                aio_req->Reserved    = 0;

                // delete the file from our internal state immediately.
                size_t const  lasti  = vfs->ActiveCount - 1;
                vfs->StInASID[index] = vfs->StInASID[lasti];
                vfs->Priority[index] = vfs->Priority[lasti];
                vfs->RdOffset[index] = vfs->RdOffset[lasti];
                vfs->StInInfo[index] = vfs->StInInfo[lasti];
                vfs->ActiveCount     = lasti;

                // mark the file as closed in the live list.
                st.StatusFlags &=~VFS_STATUS_CLOSE;
                st.StatusFlags |= VFS_STATUS_CLOSED;
                io_count(stats, IO_COUNT_CLOSES_STARTED);
            }
            else
            {   // there's no more space in the pending I/O operation queue.
                // we'll try closing the file again when there's space.
                st.StatusFlags |= VFS_STATUS_CLOSE;
                io_stall(stats, IO_STALL_FULL_VFS_QUEUE);
                break;
            }
        }
    }
}

/// @summary Processes all completed file close notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
/// @param stats The I/O driver statistics to update.
internal_function void vfs_process_completed_closes(vfs_state_t *vfs, aio_state_t *aio, io_stats_t *stats)
{
    aio_res_t res;
    size_t  index;
    while (srsw_fifo_get(&aio->CloseResults, res))
    {   // it's possible that the close is the last 'outstanding' operation.
        // if the stream decoder state is still live, we might need to delete it.
        if (vfs_stat_by_asid(vfs, res.AFID , index))
        {   // the decoder state is still live. check to see if it should be deleted.
            vfs_sistat_t &st = vfs->LiveStat[index];
            // if there are no more outstanding operations, the decoder state can be deleted.
            if (st.NLiveDecode == 0)
            {   // delete the decoder state object.
                delete st.Decoder; st.Decoder = NULL;
                // now swap the last item in the live list into slot index.
                size_t   last_index  = vfs->LiveCount - 1;
                vfs->LiveASID[index] = vfs->LiveASID[last_index];
                vfs->LiveStat[index] = vfs->LiveStat[last_index];
                vfs->LiveCount = last_index;
            }
        }
        if (SUCCEEDED(res.OSError)) io_count(stats, IO_COUNT_CLOSES_COMPLETE_SUCCESS);
        else io_count(stats, IO_COUNT_CLOSES_COMPLETE_ERROR);
    }
}

/// @summary Processes all completed file read notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
/// @param stats The I/O driver statistics to update.
internal_function void vfs_process_completed_reads(vfs_state_t *vfs, aio_state_t *aio, io_stats_t *stats)
{
    aio_res_t  res;
    vfs_sird_t read;
    size_t     index_a = 0; // index in the active stream list
    size_t     index_l = 0; // index if the live stream list
    while (srsw_fifo_get(&aio->ReadResults, res))
    {   // locate the stream in the active streams list. if the stream isn't
        // found in the active list, just return the I/O buffer.
        if (vfs_find_by_asid(vfs, res.AFID, index_a))
        {   // all active streams must have an active decoder state.
            vfs_stat_by_asid(vfs, res.AFID, index_l);
            // decrement the number of pending I/O operations.
            vfs->LiveStat[index_l].NLiveIoOps--;
            // map the file type to the thread ID where results are processed.
            int32_t  thread_id = io_thread_for_file_type(res.Type);
            // generate an end-of-stream notification, if appropriate.
            if (res.FileOffset + res.DataAmount >= vfs->StInInfo[index_a].FileSize)
            {   // handle end-of-stream using the default behavior for the stream.
                if (vfs->StInInfo[index_a].EndBehavior == STREAM_IN_LOOP)
                {   // unpause and rewind the stream.
                    vfs->LiveStat[index_l].StatusFlags &=~VFS_STATUS_PAUSE;
                    vfs->RdOffset[index_a] = 0;
                }
                // post the end-of-stream notification for the data consumer.
                vfs_sies_t eos = { res.AFID, vfs->StInInfo[index_a].EndBehavior };
                srsw_fifo_put(&vfs->SiEndOfS[thread_id], eos);
                io_count(stats, IO_COUNT_STREAM_IN_EOS);
            }
            // populate and enqueue a pending decode operation.
            read.ASID        = res.AFID;
            read.DataBuffer  = res.DataBuffer;
            read.FileOffset  = res.FileOffset; // relative offset
            read.DataAmount  = res.DataAmount;
            read.FileType    = res.Type;
            read.OSError     = res.OSError;
            read.Decoder     = vfs->LiveStat[index_l].Decoder;
            if (srsw_fifo_put(&vfs->SiResult[thread_id], read))
            {   // bump the number of pending decode operations.
                vfs->LiveStat[index_l].NLiveDecode++;
            }
            else
            {   // the result queue is full. this is a serious error.
                iobuf_put(vfs->IoAllocator, res.DataBuffer);
                io_error(stats, IO_ERROR_FULL_RESULTQUEUE);
            }
        }
        else
        {   // directly return the buffer - the application closed the stream;
            // we're just receiving remenants of the streaming process.
            iobuf_put(vfs->IoAllocator, res.DataBuffer);
            // even though the stream might not be active, it might still be live.
            if (vfs_stat_by_asid(vfs, res.AFID, index_l))
            {   // decrement the number of pending I/O operations.
                vfs->LiveStat[index_l].NLiveIoOps--;
            }
        }
        io_count_increment(stats, IO_COUNT_BYTES_READ_ACTUAL, res.DataAmount);
        if (SUCCEEDED(res.OSError)) io_count(stats, IO_COUNT_READS_COMPLETE_SUCCESS);
        else io_count(stats, IO_COUNT_READS_COMPLETE_ERROR);
    }
}

/// @summary Processes all completed file write notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
/// @param stats The I/O driver statistics to update.
internal_function void vfs_process_completed_writes(vfs_state_t *vfs, aio_state_t *aio, io_stats_t *stats)
{
    aio_res_t res;
    while (srsw_fifo_get(&aio->WriteResults, res))
    {   // no need to return anything to the platform layer.
        if (res.DataBuffer != NULL)
        {   // free the fixed-size write buffer.
            VirtualFree(res.DataBuffer, 0, MEM_RELEASE);
        }
        io_count_increment(stats, IO_COUNT_BYTES_WRITE_ACTUAL, res.DataAmount);
        if (SUCCEEDED(res.OSError)) io_count(stats, IO_COUNT_WRITES_COMPLETE_SUCCESS);
        else io_count(stats, IO_COUNT_WRITES_COMPLETE_ERROR);
    }
}

/// @summary Processes all completed file flush notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
/// @param stats The I/O driver statistics to update.
internal_function void vfs_process_completed_flushes(vfs_state_t *vfs, aio_state_t *aio, io_stats_t *stats)
{
    aio_res_t res;
    // there's nothing that the VFS driver needs to do here for the application.
    while (srsw_fifo_get(&aio->FlushResults, res))
    {
        if (SUCCEEDED(res.OSError)) io_count(stats, IO_COUNT_FLUSHES_COMPLETE_SUCCESS);
        else io_count(stats, IO_COUNT_FLUSHES_COMPLETE_ERROR);
    }
}

/// @summary Processes all pending buffer returns and releases memory back to the pool.
/// @param vfs The VFS driver state.
/// @param stats The I/O driver statistics to update.
internal_function void vfs_process_buffer_returns(vfs_state_t *vfs, io_stats_t *stats)
{
    for (size_t i = 0; i < THREAD_ID_COUNT; ++i)
    {
        vfs_sibr_t       ret;
        size_t           index_l   = 0;
        vfs_sireturnq_t *returnq   = &vfs->SiReturn[i];
        iobuf_alloc_t   &allocator =  vfs->IoAllocator;
        while (srsw_fifo_get(returnq, ret))
        {
            iobuf_put(allocator, ret.DataBuffer);
            if (vfs_stat_by_asid(vfs, ret.ASID, index_l))
            {
                vfs_sistat_t &st = vfs->LiveStat[index_l];
                // decrement the number of outstanding decode operations.
                st.NLiveDecode--;
                // check to see whether the decoder state should be deleted.
                if (st.StatusFlags & VFS_STATUS_CLOSED)
                {   // if there are no more outstanding operations,
                    // the decode state can be deleted safely.
                    if (st.NLiveDecode == 0)
                    {   // delete the decoder state object.
                        delete st.Decoder; st.Decoder = NULL;
                        // now swap the last item in the live list into slot index_l.
                        size_t   last_index    = vfs->LiveCount - 1;
                        vfs->LiveASID[index_l] = vfs->LiveASID[last_index];
                        vfs->LiveStat[index_l] = vfs->LiveStat[last_index];
                        vfs->LiveCount         = last_index;
                    }
                }
            }
        }
    }
}

/// @summary Updates the status of all active input streams, and submits I/O operations.
/// @param vfs The VFS driver state.
/// @param stats The I/O driver statistics to update.
/// @return true if the tick should continue submitting I/O operations, or false if
/// either buffer space is full or the I/O operation queue is full.
internal_function bool vfs_update_stream_in(vfs_state_t *vfs, io_stats_t *stats)
{
    iobuf_alloc_t &allocator = vfs->IoAllocator;
    size_t const read_amount = allocator.AllocSize;
    uint32_t     priority    = 0;
    size_t       index_a     = 0;
    size_t       index_l     = 0;
    vfs_io_fpq_t file_queue;
    vfs_siop_t   op;

    // process any pending stream-in control operations.
    while (srmw_fifo_get(&vfs->StInCommandQ, op))
    {
        if (vfs_find_by_asid(vfs, op.ASID, index_a))
        {   // the stream is currently in the active list, so process the control.
            vfs_stat_by_asid(vfs, op.ASID, index_l);
            switch (op.OpId)
            {
                case STREAM_IN_PAUSE:
                    vfs->LiveStat[index_l].StatusFlags |= VFS_STATUS_PAUSE;
                    vfs->LiveStat[index_l].StatusFlags &=~VFS_STATUS_CLOSE;
                    io_count(stats, IO_COUNT_STREAM_IN_PAUSE);
                    break;
                case STREAM_IN_RESUME:
                    vfs->LiveStat[index_l].StatusFlags &=~VFS_STATUS_PAUSE;
                    io_count(stats, IO_COUNT_STREAM_IN_RESUME);
                    break;
                case STREAM_IN_REWIND:
                    vfs->LiveStat[index_l].StatusFlags &=~VFS_STATUS_PAUSE;
                    vfs->RdOffset[index_a] = 0;
                    io_count(stats, IO_COUNT_STREAM_IN_REWIND);
                    break;
                case STREAM_IN_SEEK:
                    if ((op.Argument & (vfs->StInInfo[index_a].SectorSize-1)) != 0)
                    {   // round up to the nearest sector size multiple.
                        // then, subtract the sector size to get the next lowest multiple.
                        op.Argument  = align_up(op.Argument, vfs->StInInfo[index_a].SectorSize);
                        op.Argument -= vfs->StInInfo[index_a].SectorSize;
                    }
                    vfs->LiveStat[index_l].StatusFlags &=~VFS_STATUS_PAUSE;
                    vfs->RdOffset[index_a] = op.Argument;
                    io_count(stats, IO_COUNT_STREAM_IN_SEEK);
                    break;
                case STREAM_IN_STOP:
                    vfs->LiveStat[index_l].StatusFlags |=  VFS_STATUS_CLOSE;
                    io_count(stats, IO_COUNT_STREAM_IN_STOP);
                    break;
            }
        }
    }

    // build a priority queue of files, and then process them one at a time
    // starting with the highest-priority file. the goal here is to fill up
    // the queue of pending I/O operations and stay maximally busy.
    io_fpq_clear(&file_queue);
    for (size_t i = 0, n = vfs->LiveCount; i < n; ++i)
    {
        if (vfs->LiveStat[i].StatusFlags == VFS_STATUS_NONE)
        {   // only update streams that are active (not paused, not closed.)
            vfs_find_by_asid(vfs  , vfs->LiveASID[i], index_a);
            io_fpq_put(&file_queue, vfs->LiveStat[i].Priority, index_a, i);
        }
    }
    while(io_fpq_get(&file_queue, index_a, index_l, priority))
    {   // we want to submit as many sequential reads against the file as
        // possible for maximum efficiency. these operations will be
        // processed in-order, so this minimizes seeking as much as possible.
        // stop submitting operations for this file under these conditions:
        // 1. we've reached the end of the file data. continue with the next file.
        // 2. we've run out of pending queue space. stop processing for the tick.
        // 3. we've run out of I/O buffer space. stop processing for the tick.
        uint32_t  nqueued = 0;
        while (iobuf_bytes_free(allocator) > 0)
        {   // allocate a new request in our internal operation queue.
            aio_req_t *req = io_opq_put(&vfs->IoOperations, priority);
            if (req != NULL)
            {   // populate the (already queued) request.
                req->Command    = AIO_COMMAND_READ;
                req->Fildes     = vfs->StInInfo[index_a].Fildes;
                req->DataAmount = uint32_t(read_amount);
                req->BaseOffset = vfs->StInInfo[index_a].FileOffset;
                req->FileOffset = vfs->RdOffset[index_a];
                req->DataBuffer = iobuf_get(allocator);
                req->QTimeNanos = nanotime();
                req->ATimeNanos = 0;
                req->AFID       = vfs->StInASID[index_a];
                req->Type       = vfs->StInInfo[index_a].FileType;
                req->Reserved   = 0;
                nqueued++;

                // update statistics.
                io_count(stats, IO_COUNT_READS_STARTED);
                io_count_increment(stats, IO_COUNT_BYTES_READ_REQUEST, read_amount);
                io_count_assign_max(stats, IO_COUNT_MAX_STREAM_IN_BYTES_USED, iobuf_bytes_used(allocator));

                // update the byte offset to the next read.
                int64_t newofs  = vfs->RdOffset[index_a] + read_amount;
                vfs->RdOffset[index_a] = newofs;
                if (newofs >= vfs->StInInfo[index_a].FileSize)
                {   // reached or passed end-of-file.
                    // continue processing the next file.
                    break;
                }
            }
            else
            {   // we ran out of I/O queue space; no point in continuing.
                io_stall(stats, IO_STALL_FULL_VFS_QUEUE);
                return false;
            }
        }

        // update the number of pending AIO operations against the file.
        // this value is decremented as operations are completed.
        vfs->LiveStat[index_l].NLiveIoOps += nqueued;

        // handle end-of-stream conditions. the stream will be paused 
        // until AIO has completed all outstanding requests for the 
        // stream, at which point the end-of-stream notification will 
        // be generated for the data consumer. in the case of looping 
        // streams, this prevents the stream from consuming too much 
        // space in the VFS I/O operation queue. to avoid hitching, 
        // we won't wait until the data consumer has actually processed
        // the end-of-stream notification, or the data. the stream may
        // be unpaused in vfs_process_completed_reads().
        if (vfs->RdOffset[index_a] >= vfs->StInInfo[index_a].FileSize)
        {   // place the stream into the paused state.
            vfs->LiveStat[index_l].StatusFlags |= VFS_STATUS_PAUSE;
        }
        else if (nqueued == 0)
        {   // if we ran out of I/O buffer space there's no point in continuing.
            io_stall(stats, IO_STALL_OUT_OF_IOBUFS);
            return false;
        }
    }
    return true;
}

/// @summary Processes as many pending stream-out operations as possible.
/// @param vfs The VFS driver state to update.
/// @param stats The I/O driver statistics to update.
/// @return true if the tick should continue submitting I/O operations, or false if
/// the I/O operation queue is full.
internal_function bool vfs_update_stream_out(vfs_state_t *vfs, io_stats_t *stats)
{
    vfs_sowr_t write;
    while (srmw_fifo_get(&vfs->StOutWriteQ, write))
    {   // allocate a new request in our internal operation queue.
        aio_req_t *req  = io_opq_put(&vfs->IoOperations, write.Priority);
        if (req != NULL)
        {   // populate the (already queued) request.
            req->Command    = AIO_COMMAND_WRITE;
            req->Fildes     = write.Fildes;
            req->DataAmount = write.DataSize;
            req->BaseOffset = 0;
            req->FileOffset = write.FileOffset;
            req->DataBuffer = write.DataBuffer;
            req->QTimeNanos = nanotime();
            req->ATimeNanos = 0;
            req->AFID       = 0;
            req->Type       = 0;
            req->Reserved   = 0;

            // update statistics.
            io_count(stats, IO_COUNT_WRITES_STARTED);
            io_count_increment(stats, IO_COUNT_BYTES_WRITE_REQUEST, write.DataSize);
        }
        else
        {   // the internal operation queue is full.
            // put the write back in the queue, and return.
            io_stall(stats, IO_STALL_FULL_VFS_QUEUE);
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
            req->DataAmount = 0;
            req->BaseOffset = 0;
            req->FileOffset = close.FileSize;
            req->DataBuffer = NULL;
            req->QTimeNanos = nanotime();
            req->ATimeNanos = 0;
            req->AFID       = 0;
            req->Type       = 0;
            req->Reserved   = 0;

            // update statistics.
            io_count(stats, IO_COUNT_CLOSES_STARTED);
        }
        else
        {   // the internal operation queue is full.
            // put the close back in the queue, and return.
            io_stall(stats, IO_STALL_FULL_VFS_QUEUE);
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
    uint64_t   s_nanos = nanotime();
    uint64_t   e_nanos = 0;
    io_stats_t null_stats;
    if (stats == NULL)
    {   // prevent everything from constantly having to NULL-check this.
        init_io_stats(&null_stats);
        stats = &null_stats;
    }

    // free up as much buffer state as possible.
    vfs_process_buffer_returns(vfs, stats);

    // generate read and write I/O operations. this increments the number of
    // pending I/O operations across the set of active files. process stream
    // out first, as there are likely few of these operations, and we don't
    // want them to be starved out by the stream-in operations.
    vfs_update_stream_out(vfs, stats);
    vfs_update_stream_in(vfs, stats);
    io_count_assign(stats, IO_COUNT_STREAM_IN_BYTES_USED, iobuf_bytes_used(vfs->IoAllocator));

    // we're done generating operations, so push as much as possible to AIO.
    aio_req_t request;
    size_t    qsize  = vfs->IoOperations.Count;
    while (io_opq_top(&vfs->IoOperations, request))
    {   // we were able to retrieve an operation from our internal queue.
        if (srsw_fifo_put(&aio->RequestQueue, request))
        {   // we were able to push it to AIO, so remove it from our queue.
            io_opq_get(&vfs->IoOperations, request);
        }
        else
        {   // the AIO request queue was full, so stall out.
            io_stall(stats, IO_STALL_FULL_AIO_QUEUE);
        }
    }
    io_count_assign_max(stats, IO_COUNT_MAX_OPS_QUEUED, qsize);

    // dispatch any completed I/O operations to the per-type queues for
    // processing by the platform layer and dispatching to the application.
    // this decrements the number of pending I/O operations across the file set.
    vfs_process_completed_reads  (vfs, aio, stats);
    vfs_process_completed_writes (vfs, aio, stats);
    vfs_process_completed_flushes(vfs, aio, stats);
    vfs_process_completed_closes (vfs, aio, stats);

    // close file requests should be processed after all read and write requests.
    // this ensures that all I/O has been submitted before closing the file.
    // files with pending I/O will not be closed until the I/O completes.
    vfs_process_closes(vfs, stats);

    // open file requests should be processed after all close requests.
    // this increases the likelyhood that we'll have open file slots.
    vfs_process_sicreate(vfs, stats);

    // update statistics.
    e_nanos = nanotime();
    io_count(stats, IO_COUNT_TICKS_ELAPSED_VFS);
    io_count_increment (stats, IO_COUNT_NANOS_ELAPSED_VFS    , e_nanos - s_nanos);
    io_count_assign_min(stats, IO_COUNT_MIN_TICK_DURATION_VFS, e_nanos - s_nanos);
    io_count_assign_max(stats, IO_COUNT_MAX_TICK_DURATION_VFS, e_nanos - s_nanos);

    // update rates.
    double   sec = seconds(e_nanos - stats->StartTimeNanos);
    uint64_t bsi = stats->Counts[IO_COUNT_BYTES_READ_ACTUAL];
    uint64_t bso = stats->Counts[IO_COUNT_BYTES_WRITE_ACTUAL];
    io_rate(stats, IO_RATE_BYTES_PER_SEC_IN , bsi / sec);
    io_rate(stats, IO_RATE_BYTES_PER_SEC_OUT, bso / sec);
}

/// @param vfs The VFS state that posted the I/O result.
/// @param asid The application-defined stream identifier.
/// @param type One of file_type_e indicating the type of file being processed.
/// @param buffer The buffer to return. This value may be NULL.
internal_function void vfs_return_buffer(vfs_state_t *vfs, intptr_t asid, int32_t type, void *buffer)
{
    void const *iobeg = (uint8_t const *)  vfs->IoAllocator.BaseAddress;
    void const *ioend = (uint8_t const *)  vfs->IoAllocator.BaseAddress + vfs->IoAllocator.TotalSize;
    if (buffer >= iobeg && buffer < ioend)
    {   // only return the buffer if it's within the address range handed out
        // by the I/O buffer allocator. this excludes user-allocated buffers.
        int32_t    tid = io_thread_for_file_type(type);
        vfs_sibr_t ret = { asid, buffer };
        srsw_fifo_put(&vfs->SiReturn[tid], ret);
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
    for (size_t i = 0; i < THREAD_ID_COUNT; ++i)
    {
        flush_srsw_fifo(&vfs->SiResult[i]);
        flush_srsw_fifo(&vfs->SiReturn[i]);
        flush_srsw_fifo(&vfs->SiEndOfS[i]);
    }
    io_opq_clear(&vfs->IoOperations);
    vfs->ActiveCount = 0;
    vfs->LiveCount   = 0;
    return true;
}

/// @summary Free resources associated with a VFS driver state.
/// @param vfs The VFS driver state to delete.
internal_function void delete_vfs_state(vfs_state_t *vfs)
{
    vfs->LiveCount   = 0;
    vfs->ActiveCount = 0;
    io_opq_clear(&vfs->IoOperations);
    delete_srmw_fifo(&vfs->StInCreateQ);
    delete_srmw_fifo(&vfs->StInCommandQ);
    delete_srmw_fifo(&vfs->StOutCloseQ);
    delete_srmw_fifo(&vfs->StOutWriteQ);
    delete_iobuf_allocator(vfs->IoAllocator);
    for (size_t i = 0; i < THREAD_ID_COUNT; ++i)
    {
        flush_srsw_fifo(&vfs->SiResult[i]);
        flush_srsw_fifo(&vfs->SiReturn[i]);
        flush_srsw_fifo(&vfs->SiEndOfS[i]);
    }
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
    LPSTR  buffer = NULL;
    size_t size   = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM     |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer,
        0, NULL);
    fprintf(stderr, "I/O ERROR: %p(%s): %u(0x%08X): %s\n", (void*) app_id, FILE_TYPE_NAME[type], error_code, error_code, buffer);
    if (buffer != NULL) LocalFree(buffer);
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
    LPSTR  buffer = NULL;
    size_t size   = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM     |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer,
        0, NULL);
    fprintf(stderr, "I/O ERROR: %p(%s): %u(0x%08X): %s\n", (void*) app_id, FILE_TYPE_NAME[type], error_code, error_code, buffer);
    if (buffer != NULL) LocalFree(buffer);
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
    HANDLE  fd     = INVALID_HANDLE_VALUE;
    HANDLE  iocp   = AIO_STATE.ASIOContext;
    size_t  ssize  = 0;
    int64_t lsize  = 0;
    int64_t psize  = 0;
    int64_t offset = 0;
    stream_decoder_t *decoder = NULL;
    if (mode == STREAM_IN_ONCE)
    {   // for files that will be streamed in only once, prefer unbuffered I/O.
        // this avoids polluting the page cache with their data.
        hint  = FILE_HINT_DIRECT;
    }
    if (vfs_resolve_file_read(path, hint, iocp, fd, lsize, psize, offset, ssize, decoder, &IO_STATS))
    {   // queue a load file request to be processed by the VFS driver.
        vfs_sics_t req;
        req.Next       = NULL;
        req.Fildes     = fd;
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
{   // TODO: consider extending this to use vfs_resolve_file_read?
    LARGE_INTEGER sz = {0};
    file_t     *f = NULL;
    DWORD  access = read_only ? GENERIC_READ    : GENERIC_READ | GENERIC_WRITE;
    DWORD  share  = read_only ? FILE_SHARE_READ : 0;
    DWORD  create = OPEN_ALWAYS;
    DWORD  flags  = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE fd     = INVALID_HANDLE_VALUE;
    WCHAR *pathbuf= NULL;
    int    nchars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);

    // convert the path from UTF-8 to UCS-2, which Windows requires.
    if (nchars == 0)
    {   // the path cannot be converted from UTF-8 to UCS-2.
        goto error_cleanup;
    }
    pathbuf = (WCHAR*) malloc(nchars * sizeof(WCHAR));
    if (pathbuf != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, pathbuf, nchars) == 0)
    {   // the path cannot be converted from UTF-8 to UCS-2.
        goto error_cleanup;
    }

    // open the file with the requested access and flags.
    if ((fd = CreateFile(pathbuf, access, share, NULL, create, flags, NULL)) == INVALID_HANDLE_VALUE)
    {   // the file could not be opened; check GetLastError().
        goto error_cleanup;
    }
    if (!GetFileSizeEx(fd, &sz))
    {   // the file size could not be retrieved; check GetLastError().
        goto error_cleanup;
    }

    f =  (file_t*) malloc(sizeof(file_t));
    if (f == NULL) goto error_cleanup;

    file_size = sz.QuadPart;
    f->Fildes = fd;
    *file     = f;
    return true;

error_cleanup:
    if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd);
    if (pathbuf != NULL) free(pathbuf);
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
    LARGE_INTEGER pos = {0};
    pos.QuadPart  = offset;
    if (!SetFilePointerEx(file->Fildes, pos, NULL, FILE_BEGIN))
    {   // unable to seek to the specified offset. check GetLastError().
        bytes_read = 0;
        return false;
    }
    // safely read the requested number of bytes. for very large reads,
    // size > UINT32_MAX, the result is undefined, so possibly split the
    // read up into several sub-reads (though this case is unlikely...)
    uint8_t *b  =(uint8_t*) buffer;
    bytes_read  = 0;
    while (size > 0)
    {
        DWORD nread  = 0;
        DWORD toread = size <= 0xFFFFFFFFU ? DWORD(size) : 0xFFFFFFFFU;
        DWORD err    = ERROR_SUCCESS;
        if (ReadFile(file->Fildes, &b[bytes_read], toread, &nread, NULL))
        {
            if (nread > 0)
            {   // the read has completed successfully and returned data.
                bytes_read += nread;
                size       -= nread;
            }
            else if (nread == 0)
            {   // end-of-file was encountered (not sure if this is the path?)
                break;
            }
        }
        else if ((err = GetLastError()) == ERROR_HANDLE_EOF)
        {   // end of file was encountered. not sure if this is async only?
            break;
        }
        else
        {   // an error occurred; check err.
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
    LARGE_INTEGER pos = {0};
    pos.QuadPart  = offset;
    if (!SetFilePointerEx(file->Fildes, pos, NULL, FILE_BEGIN))
    {   // unable to seek to the specified offset. check GetLastError().
        bytes_written = 0;
        return false;
    }
    uint8_t const *b  = (uint8_t const*) buffer;
    bytes_written     = 0;
    while (size > 0)
    {
        DWORD nwrite  = 0;
        DWORD towrite = size <= 0xFFFFFFFFU ? DWORD(size) : 0xFFFFFFFFU;
        if (WriteFile(file->Fildes, &b[bytes_written], towrite, &nwrite, NULL))
        {
            bytes_written += nwrite;
            size          -= nwrite;
        }
        else
        {   // an error occurred; check GetLastError().
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
    return (FlushFileBuffers(file->Fildes) == TRUE);
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
        if (f->Fildes != INVALID_HANDLE_VALUE) CloseHandle(f->Fildes);
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
    FILE_END_OF_FILE_INFO eof;
    FILE_ALLOCATION_INFO  sec;
    uint8_t const     *buffer = (uint8_t const*)data;
    SYSTEM_INFO sysinfo       = {0};
    HANDLE      fd            = INVALID_HANDLE_VALUE;
    DWORD       mem_flags     = MEM_RESERVE  | MEM_COMMIT;
    DWORD       mem_protect   = PAGE_READWRITE;
    DWORD       access        = GENERIC_READ | GENERIC_WRITE;
    DWORD       share         = FILE_SHARE_READ;
    DWORD       create        = CREATE_ALWAYS;
    DWORD       flags         = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING;
    WCHAR      *temp_path     = make_temp_path(path, L"writeout");
    WCHAR      *file_path     = NULL;
    DWORD       page_size     = 0;
    int64_t     file_size     = 0;
    uint8_t    *sector_buffer = NULL;
    size_t      sector_count  = 0;
    int64_t     sector_bytes  = 0;
    size_t      sector_over   = 0;
    size_t      sector_size   = 0;
    int         nchars        = 0;

    // convert the destination path from UTF-8 to UCS-2.
    nchars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (nchars == 0)
    {   // unable to convert UTF-8 to UCS-2; check GetLastError().
        goto error_cleanup;
    }
    file_path = (WCHAR*) malloc(nchars * sizeof(WCHAR));
    if (file_path == NULL)
    {   // unable to allocate the UCS-2 path buffer.
        goto error_cleanup;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, file_path, nchars);

    // get the system page size, and allocate a single-page buffer. to support
    // unbuffered I/O, the buffer must be allocated on a sector-size boundary
    // and must also be a multiple of the physical disk sector size. allocating
    // a virtual memory page using VirtualAlloc will satisfy these constraints.
    GetNativeSystemInfo_Func(&sysinfo);
    page_size     = sysinfo.dwPageSize;
    sector_buffer = (uint8_t*) VirtualAlloc(NULL, page_size, mem_flags, mem_protect);
    if (sector_buffer == NULL)
    {   // unable to allocate the overage buffer; check GetLastError().
        goto error_cleanup;
    }

    // create the temporary file, and get the physical disk sector size.
    // pre-allocate storage for the file contents, which should improve performance.
    if ((fd = CreateFile(temp_path, access, share, NULL, create, flags, NULL)) == INVALID_HANDLE_VALUE)
    {   // unable to create the new file; check GetLastError().
        goto error_cleanup;
    }
    sector_size = physical_sector_size(fd);
    file_size   = align_up(size, sector_size);
    sec.AllocationSize.QuadPart = file_size;
    SetFileInformationByHandle_Func(fd, FileAllocationInfo , &sec, sizeof(sec));

    // copy the data extending into the tail sector into the overage buffer.
    sector_count = size_t (size / sector_size);
    sector_bytes = int64_t(sector_size) * sector_count;
    sector_over  = size_t (size - sector_bytes);
    if (sector_over > 0)
    {   // buffer the overlap amount. note that the page will have been zeroed.
        memcpy(sector_buffer, &buffer[sector_bytes], sector_over);
    }

    // write the data to the file.
    if (sector_bytes > 0)
    {   // write the bulk of the data, if the data is > 1 sector.
        int64_t amount = 0;
        DWORD   nwrite = 0;
        while  (amount < sector_bytes)
        {
            DWORD n = (sector_bytes - amount) < 0xFFFFFFFFU ? DWORD(sector_bytes - amount) : 0xFFFFFFFFU;
            WriteFile(fd, &buffer[amount], n, &nwrite, NULL);
            amount += nwrite;
        }
    }
    if (sector_over > 0)
    {   // write the remaining sector-sized chunk of data.
        DWORD n = (DWORD) sector_size;
        DWORD w = (DWORD) 0;
        WriteFile(fd, sector_buffer, n, &w, NULL);
    }

    // set the correct end-of-file marker.
    eof.EndOfFile.QuadPart      = size;
    SetFileInformationByHandle_Func(fd, FileEndOfFileInfo  , &eof, sizeof(eof));
    SetFileValidData(fd, eof.EndOfFile.QuadPart); // requires elevate_process_privileges().

    // close the file, and move it to the destination path.
    CloseHandle(fd); fd = INVALID_HANDLE_VALUE;
    if (!MoveFileEx(temp_path, file_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {   // the file could not be moved into place; check GetLastError().
        goto error_cleanup;
    }

    // clean up our temporary buffers; we're done.
    VirtualFree(sector_buffer, 0, MEM_RELEASE);
    free(file_path);
    free(temp_path);
    return true;

error_cleanup:
    if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd);
    if (sector_buffer != NULL) VirtualFree(sector_buffer, 0, MEM_RELEASE);
    if (file_path != NULL) free(file_path);
    if (temp_path != NULL) DeleteFile(temp_path);
    if (temp_path != NULL) free(temp_path);
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
    FILE_END_OF_FILE_INFO eof;
    FILE_ALLOCATION_INFO  sec;
    stream_writer_t *sw = NULL;                 // the file writer we return
    HANDLE  fd          = INVALID_HANDLE_VALUE; // file descriptor of the temporary file
    WCHAR  *temp_path   = NULL;                 // buffer for the path of the temporary file
    int64_t file_size   =  0;                   // the file size, rounded up to the nearest sector
    size_t  sector_size =  0;                   // the physical disk sector size, in bytes
    void   *buffer      = NULL;                 // the write buffer for the stream
    DWORD   mem_flags   = MEM_RESERVE | MEM_COMMIT;
    DWORD   mem_protect = PAGE_READWRITE;
    DWORD   access      = GENERIC_READ | GENERIC_WRITE;
    DWORD   share       = FILE_SHARE_READ;
    DWORD   create      = CREATE_ALWAYS;
    DWORD   flags       = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING;

    // allocate storage for the file writer up front.
    sw = (stream_writer_t*) malloc(sizeof(stream_writer_t));
    if (sw == NULL) goto error_cleanup;

    // we need to create this file in the same directory as the output path.
    // this avoids problems with rename() not being able to work across partitions.
    // so, generate a unique filename within that same directory.
    temp_path = make_temp_path(where, L"tempfile");
    if (temp_path == NULL) goto error_cleanup;

    // create the temporary file, and get the physical disk sector size.
    if ((fd = CreateFile(temp_path, access, share, NULL, create, flags, NULL)) == INVALID_HANDLE_VALUE)
    {   // unable to create the new file; check GetLastError().
        goto error_cleanup;
    }
    if (reserve_size > 0)
    {   // pre-allocate storage for the file contents, which should improve performance.
        // this is best-effort, so it's not a fatal error if it fails.
        sector_size = physical_sector_size(fd);
        file_size   = align_up(reserve_size, sector_size);
        sec.AllocationSize.QuadPart = file_size;
        SetFileInformationByHandle_Func(fd, FileAllocationInfo , &sec, sizeof(sec));
    }

    // now use VirtualAlloc to allocate a buffer for combining small writes.
    // use VirtualAlloc because it is guaranteed to return addresses with the
    // correct alignment, and ranges rounded up to the correct size for use
    // with unbuffered I/O.
    if ((buffer = VirtualAlloc(NULL, VFS_WRITE_SIZE, mem_flags, mem_protect)) == NULL)
    {   // the allocation failed; check GetLastError().
        goto error_cleanup;
    }

    // populate the file writer; we're done.
    sw->Fildes      = fd;
    sw->BaseAddress = (uint8_t*) buffer;
    sw->DataOffset  = 0;
    sw->FileOffset  = 0;
    sw->Priority    = priority;
    *writer = sw;
    return true;

error_cleanup:
    if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd);
    if (temp_path != NULL) DeleteFile(temp_path);
    if (temp_path != NULL) free(temp_path);
    if (sw != NULL) free(sw);
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
    DWORD mem_flags   = MEM_RESERVE | MEM_COMMIT;
    DWORD mem_protect = PAGE_READWRITE;

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
            if ((newbuf = VirtualAlloc(NULL, VFS_WRITE_SIZE, mem_flags, mem_protect)) == NULL)
            {   // this is a serious error - check GetLastError().
                return false;
            }

            // fill up the active buffer, and queue a write to the VFS.
            // if queueing the write fails, we won't update DataOffset,
            // and the caller can attempt to resume the write later.
            memcpy(&writer->BaseAddress[writer->DataOffset], srcbuf, nwrite);

            vfs_sowr_t write;
            write.Next       = NULL;
            write.Fildes     = writer->Fildes;
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
    HANDLE            fd =  sw->Fildes;
    WCHAR     *file_path =  NULL;
    int           nchars =  0;

    if (path != NULL)
    {   // convert the destination path from UTF-8 to UCS-2.
        nchars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
        if (nchars == 0)
        {   // unable to convert UTF-8 to UCS-2; check GetLastError().
            return false;
        }
        file_path = (WCHAR*) malloc(nchars * sizeof(WCHAR));
        if (file_path == NULL)
        {   // unable to allocate the UCS-2 path buffer.
            return false;
        }
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, file_path, nchars);
    }

    if (nb > 0)
    {   // queue a write with the remaining data.
        vfs_sowr_t       write;
        write.Next       = NULL;
        write.Fildes     = sw->Fildes;
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
        VirtualFree(addr, 0, MEM_RELEASE);
    }

    // submit the stream close request to the VFS.
    vfs_socs_t     close;
    close.Next     = NULL;
    close.Fildes   = fd;
    close.Priority = priority;
    close.FilePath = file_path;
    close.FileSize = fs;
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
    size_t num = 0;
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
        aio_poll(&AIO_STATE, &IO_STATS);

        if (VFS_STATE.ActiveCount == 0)
        {   // we could have closed due to an error in this test case.
            // normally, you wouldn't have this check.
            done = true;
        }

        // print statistic counters:
        print_io_rates(stdout, &IO_STATS);

        // process data received from the I/O system. normally, different
        // threads would handle one or more file types, depending on what
        // needs to be done with the data and who needs access to it.
        for (size_t i = 0; i < THREAD_ID_COUNT; ++i)
        {
            vfs_sird_t read;
            int32_t    type = int32_t(i); // the file_type_e.
            while (srsw_fifo_get(&VFS_STATE.SiResult[i], read))
            {
                if (SUCCEEDED(read.OSError))
                {   // echo the data to stdout.
                    // normally, you'd push the result to a callback.
                    // note that all of the reading of the data is performed
                    // through the stream decoder, which transparently performs
                    // any decompression and/or decryption that might be necessary.
                    if (read.FileOffset == 0)
                    {   // reset the decoder to its initial state.
                        read.Decoder->restart();
                    }
                    // attach the input buffer to the decoder.
                    read.Decoder->push(read.DataBuffer, read.DataAmount);
                    do
                    {   // read as much data as is available, and then call
                        // refill to decode the next chunk of the input buffer.
                        size_t amount = read.Decoder->amount();
                        void  *buffer = read.Decoder->Cursor;
                        //fwrite(buffer , 1, amount, stdout);
                        read.Decoder->Cursor += amount;
                    } while (read.Decoder->refill(read.Decoder) == DECODE_RESULT_START);
                    // after the application has had a chance to process
                    // the data, return the buffer so it can be used again.
                    vfs_return_buffer(&VFS_STATE, read.ASID, type, read.DataBuffer);
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
                //fprintf(stdout, "Reached end-of-stream for ASID %p.\n", (void*) eos.ASID);
                if (num < 1000)
                {
                    p->rewind_stream(eos.ASID);
                    num++;
                }
                else p->stop_stream(eos.ASID);
            }
        }
    }
    vfs_tick(&VFS_STATE, &AIO_STATE, &IO_STATS);
    aio_poll(&AIO_STATE, &IO_STATS);

    // print counters as a sanity check.
    fprintf(stdout, "\n\n");
    print_io_stats(stdout, &IO_STATS);

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
int main(int argc, char **argv)
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

    // initialize the high-resolution timer on the system.
    inittime();

    // resolve entry points in dynamic libraries.
    // TODO: this will probably need to return success/failure.
    resolve_kernel_apis();
    elevate_process_privileges();

    // TODO: dynamically load the application code.
    exit_code = test_stream_in(argc, argv, &platform_layer);
    //platform_write_out("C:\\Users\\rklenk\\abc.txt", "Hello, write out!", strlen("Hello, write out!"));

    exit(exit_code);
}

