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
#endif /* __GNUC__ <= 4.9 - MinGW */
#endif /* __GNUC__ comple - MinGW */

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

#include "bridge.h"

/*/////////////////
//   Constants   //
/////////////////*/
/// The scale used to convert from seconds into nanoseconds.
static uint64_t  const SEC_TO_NANOSEC = 1000000000ULL;

/// @summary Define the number of times per-second we want the I/O system to
/// update (assuming it's on a background thread and we have that control).
/// The lower the update rate of the I/O system, the more latency there is in
/// processing and completing I/O requests, and the lower the I/O thoroughput.
static size_t    const IO_SYSTEM_RATE = 60;

/// @summary Define the maximum number of concurrently open files.
static size_t    const MAX_OPEN_FILES = 128;

/// @summary Define the maximum number of concurrently active AIO operations.
/// We set this based on what the maximum number of AIO operations we want to
/// poll during each tick, and the maximum number the underlying OS can handle.
#ifndef WINOS_AIO_MAX_ACTIVE    // can override at compile time
#define WINOS_AIO_MAX_ACTIVE    512
#endif
static size_t    const AIO_MAX_ACTIVE = WINOS_AIO_MAX_ACTIVE;

/// @summary Define the size of the I/O buffer. This is calculated based on an
/// maximum I/O transfer rate of 960MB/s, and an I/O system tick rate of 60Hz;
/// 960MB/sec divided across 60 ticks/sec gives 16MB/tick maximum transfer rate.
#ifndef WINOS_VFS_IOBUF_SIZE    // can override at compile time
#define WINOS_VFS_IOBUF_SIZE   (16 * 1024 * 1024)
#endif
static size_t    const VFS_IOBUF_SIZE = WINOS_VFS_IOBUF_SIZE;

/// @summary Define the size of the buffer allocated for each I/O request.
static size_t    const VFS_ALLOC_SIZE = VFS_IOBUF_SIZE / AIO_MAX_ACTIVE;

/// @summary The spin count used on critical sections protecting shared resources
/// of srmw_freelist_t and srmw_fifo_t.
static DWORD     const SPIN_COUNT_Q   = 4096;

/// @summary The special I/O completion port completion key used to notify the
/// AIO driver to begin its shutdown process.
static ULONG_PTR const AIO_SHUTDOWN   = ULONG_PTR(-1);

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

/// @summary Defines the supported VFS file status flags.
enum vfs_status_e
{
    VFS_STATUS_NONE   = (0 << 0), /// No special status bits are set.
    VFS_STATUS_CLOSE  = (1 << 0), /// The file is marked as having a close pending.
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
    HANDLE             Fildes;       /// The file descriptor of the opened file.
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
    HANDLE             Fildes;       /// The file descriptor for the file.
    int64_t            FileSize;     /// The physical file size, in bytes.
    int64_t            DataSize;     /// The file size after any size-changing transforms.
    int64_t            FileOffset;   /// The absolute byte offset of the start of the file data.
    size_t             SectorSize;   /// The disk physical sector size, in bytes.
};

/// @summary Status information associated with an active file. This information is
/// required to properly process (for example) close operations, where there may be
/// one or more in-progress AIO operations against the file; the close must be
/// deferred until all in-progress AIO operations have completed.
struct vfs_status_t
{
    uint64_t           NPendingAIO;  /// The number of pending AIO operations on the file.
    uint32_t           StatusFlags;  /// A combination of vfs_status_e.
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
    vfs_status_t       FileStat[MF]; /// Pending AIO status for each active file.
    vfs_io_opq_t       IoOperations; /// The priority queue of pending I/O operations.
    vfs_resultq_t      IoResult[NT]; /// The per-file type queue for I/O results.
    vfs_returnq_t      IoReturn[NT]; /// The per-file type queue for I/O buffer returns.
    #undef NT
    #undef MF
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
typedef BOOL (WINAPI *SetFileInformationByHandleFn)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);

static GetNativeSystemInfoFn        GetNativeSystemInfo_Func        = NULL;
static SetProcessWorkingSetSizeExFn SetProcessWorkingSetSizeEx_Func = NULL;
static SetFileInformationByHandleFn SetFileInformationByHandle_Func = NULL;

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
        SetFileInformationByHandle_Func = (SetFileInformationByHandleFn) GetProcAddress(kernel, "SetFileInformationByHandle");
    }
    // fallback if any of these APIs are not available.
    if (GetNativeSystemInfo_Func        == NULL) GetNativeSystemInfo_Func = GetNativeSystemInfo_Fallback;
    if (SetProcessWorkingSetSizeEx_Func == NULL) SetProcessWorkingSetSizeEx_Func = SetProcessWorkingSetSizeEx_Fallback;
}

/// @summary Elevates the privileges for the process to include the privilege
/// SE_MANAGE_VOLUME_NAME, so that SetFileValidData() can be used to initialize
/// a file without having to zero-fill the underlying sectors. This is optional,
/// and we don't want it failing to prevent the application from launching.
static void elevate_process_privileges(void)
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
static inline size_t align_up(size_t size, size_t pow2)
{
    assert((pow2 & (pow2-1)) == 0);
    return (size == 0) ? pow2 : ((size + (pow2-1)) & ~(pow2-1));
}

/// @summary Rounds a size up to the nearest even multiple of a given power-of-two.
/// @param size The size value to round up.
/// @param pow2 The power-of-two alignment.
/// @return The input size, rounded up to the nearest even multiple of pow2.
static inline int64_t align_up(int64_t size, size_t pow2)
{
    assert((pow2 & (pow2-1)) == 0);
    return (size == 0) ? int64_t(pow2) : ((size + int64_t(pow2-1)) & ~int64_t(pow2-1));
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

/// @summary Retrieve the physical sector size for a block-access device.
/// @param file The handle to an open file on the device.
/// @return The size of a physical sector on the specified device.
static size_t physical_sector_size(HANDLE file)
{   // http://msdn.microsoft.com/en-us/library/ff800831(v=vs.85).aspx
    // for structure STORAGE_ACCESS_ALIGNMENT
    // Vista and Server 2008+ only - XP not supported.
    size_t const DefaultPhysicalSectorSize = 4096;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR desc;
    STORAGE_PROPERTY_QUERY              query;
    memset(&desc , 0, sizeof(desc));
    memset(&query, 0, sizeof(query));

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
static bool open_file_raw(char const *path, HANDLE iocp, DWORD access, DWORD share, DWORD create, DWORD flags, HANDLE &fd, int64_t &file_size, size_t &sector_size)
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
static void close_file_raw(HANDLE &fd)
{
    if (fd != INVALID_HANDLE_VALUE)
    {
        CloseHandle(fd);
        fd = INVALID_HANDLE_VALUE;
    }
}

/// @summary Resets the platform I/O statistics to zero.
/// @param stats The counters to reset.
static void init_io_stats(io_stats_t *stats)
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
static inline OVERLAPPED* asio_get(aio_state_t *aio)
{
    assert(aio->ASIOFreeCount > 0);
    return aio->ASIOFree[--aio->ASIOFreeCount];
}

/// @summary Returns an iocb instance to the free list.
/// @param aio The AIO driver state managing the free list.
/// @param asio The OVERLAPPED instance to return to the free list.
static inline void asio_put(aio_state_t *aio, OVERLAPPED *asio)
{
    assert(aio->ASIOFreeCount < AIO_MAX_ACTIVE);
    aio->ASIOFree[aio->ASIOFreeCount++] = asio;
}

/// @summary Helper function to build an AIO result packet.
/// @param error The error code to return.
/// @param amount The amount of data returned.
/// @param req The request associated with the result.
/// @return The populated AIO result packet.
static inline aio_res_t aio_result(DWORD error, uint32_t amount, aio_req_t const &req)
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
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
static int aio_submit_read(aio_state_t *aio, aio_req_t const &req, DWORD &error)
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
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
static int aio_submit_write(aio_state_t *aio, aio_req_t const &req, DWORD &error)
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
/// @return Zero if the operation was successfully submitted, or -1 if an error occurred.
static int aio_process_flush(aio_state_t *aio, aio_req_t const &req, DWORD &error)
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
/// @return Zero if the result was successfully submitted, or -1 if the result queue is full.
static int aio_process_close(aio_state_t *aio, aio_req_t const &req)
{   // close the file descriptors associated with the file.
    if (req.Fildes != INVALID_HANDLE_VALUE)
    {
        CloseHandle(req.Fildes);
    }

    // generate the completion result and push it to the queue.
    aio_res_t res = aio_result(ERROR_SUCCESS, 0, req);
    return srsw_fifo_put(&aio->CloseResults, res) ? 0 : -1;
}

/// @summary Implements the main loop of the AIO driver using a polling mechanism.
/// @param aio The AIO driver state to update.
/// @param timeout The timeout value indicating the amount of time to wait, or
/// INFINITE to block indefinitely. Note that aio_poll() just calls aio_tick() with
/// a timeout of zero, which will return immediately if no events are available.
/// @return Zero to continue with the next tick, 1 if the shutdown signal was received, -1 if an error occurred.
static int aio_tick(aio_state_t *aio, DWORD timeout)
{   // poll kernel AIO for any completed events, and process them first.
    OVERLAPPED_ENTRY events[AIO_MAX_ACTIVE];  // STACK: 8-16KB depending on OS.
    ULONG nevents = 0;
    BOOL  iocpres = GetQueuedCompletionStatusEx(aio->ASIOContext, events, AIO_MAX_ACTIVE, &nevents, timeout, FALSE);
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
            {   // TODO: track this statistic somewhere.
                // this should not happen.
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
                result = aio_submit_read (aio, req, error);
                break;
            case AIO_COMMAND_WRITE:
                result = aio_submit_write(aio, req, error);
                break;
            case AIO_COMMAND_FLUSH:
                result = aio_process_flush(aio, req, error);
                break;
            case AIO_COMMAND_CLOSE:
                result = aio_process_close(aio, req);
                break;
            default:
                error  = ERROR_INVALID_PARAMETER;
                break;
        }
    }
    return 0;
}

/// @summary Implements the main loop of the AIO driver.
/// @param aio The AIO driver state to update.
/// @return Zero to continue with the next tick, 1 if the shutdown signal was received, -1 if an error occurred.
static inline int aio_poll(aio_state_t *aio)
{   // configure a zero timeout so we won't block.
    return aio_tick(aio , 0);
}

/// @summary Allocates a new AIO context and initializes the AIO state.
/// @param aio The AIO state to allocate and initialize.
/// @return 0 if the operation completed successfully; otherwise, the GetLastError value.
static DWORD create_aio_state(aio_state_t *aio)
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
static void delete_aio_state(aio_state_t *aio)
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
/// active file given the file's application-defined file ID.
/// @param vfs The VFS driver state to search.
/// @param afid The application-defined file ID to locate.
/// @param index On return, this value is set to the zero-based index of the
/// current slot in the active file list associated with the input AFID.
/// @return true if the AFID was located in the list.
static inline bool vfs_find_by_afid(vfs_state_t const *vfs, intptr_t afid, size_t &index)
{
    intptr_t const  AFID      = afid;
    intptr_t const *AFIDList  = vfs->FileAFID;
    size_t   const  AFIDCount = vfs->ActiveCount;
    for (size_t i = 0; i < AFIDCount; ++i)
    {
        if (AFIDList[i] == AFID)
        {
            index = i;
            return true;
        }
    }
    return false;
}

/// @summary Determine whether a path references a file within an archive, (and
/// if so, which one) or whether it references a native file. Open the file if
/// necessary, and return basic file information to the caller. This function
/// should only be used for read-only files, files cannot be written in an archive.
/// @param path The NULL-terminated UTF-8 path of the file to resolve.
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
/// @return true if the file could be resolved.
static bool vfs_resolve_file_read(char const *path, HANDLE iocp, HANDLE &fd, int64_t &lsize, int64_t &psize, int64_t &offset, size_t &sector_size)
{   // TODO: determine whether this path references a file contained within an archive.
    // for now, we only handle native file paths, which may be absolute or relative.
    bool native_path = true;
    if  (native_path)
    {
        DWORD access = GENERIC_READ;
        DWORD share  = FILE_SHARE_READ;
        DWORD create = OPEN_EXISTING;
        DWORD flags  = FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN;
        if (open_file_raw(path, iocp, access, share, create, flags, fd, psize, sector_size))
        {   // native files always begin at the first byte.
            // logical and physical size are the same.
            lsize  = psize;
            offset = 0;
            return true;
        }
        else
        {   // unable to open the file, so fail immediately.
            fd = INVALID_HANDLE_VALUE; lsize = psize = offset = sector_size = 0;
            return false;
        }
    }
}

/// @summary Resolve a path to a path on the native file system, open that file
/// for writing, and return basic file information to the caller. This function
/// should be used for files which can be read or written.
/// @param path The NULL-terminated UTF-8 path of the file to resolve.
/// @param iocp The I/O completion port from the AIO driver to associate with the
/// file handle, or NULL if no I/O completion port is being used for I/O requests.
/// @param fd On return, stores the file descriptor of the archive or native file.
/// @param lsize On return, stores the logical size of the file, in bytes. If this
/// value is set to something greater than zero when the function is called, the
/// file is pre-allocated to be the specified size; otherwise, the file size is
/// left unchanged (if the file exists), or is zero if the file was just created.
/// @param psize On return, this is always set to the same value as lsize.
/// @param offset On return, stores the byte offset of the first byte of the file.
/// If this value is set to -1 when the function is called, the file pointer is
/// positioned at the current end-of-file marker (so that data will be appended.)
/// @param sector_size On return, stores the physical sector size of the disk.
/// @return true if the file could be resolved.
static bool vfs_resolve_file_write(char const *path, HANDLE iocp, HANDLE &fd, int64_t &lsize, int64_t &psize, int64_t &offset, size_t &sector_size)
{   // TODO: resolve path to a native path. only files opened on the native
    // file system can be opened for writing.
    bool native_path = true;
    if  (native_path)
    {
        DWORD access = GENERIC_READ | GENERIC_WRITE;
        DWORD share  = 0;          // do not share with anyone
        DWORD create = OPEN_ALWAYS;// or CREATE_ALWAYS, to truncate?
        DWORD flags  = FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH;
        if (open_file_raw(path, iocp, access, share, create, flags, fd, psize, sector_size))
        {
            if (lsize > 0)
            {   // pre-allocate space for the file. this is best-effort.
                int64_t logical_size  = lsize;
                int64_t physical_size = align_up(logical_size, sector_size);
                FILE_END_OF_FILE_INFO eof; eof.EndOfFile.QuadPart      = logical_size;
                FILE_ALLOCATION_INFO  sec; sec.AllocationSize.QuadPart = physical_size;
                SetFileInformationByHandle_Func(fd, FileAllocationInfo , &sec, sizeof(sec));
                SetFileInformationByHandle_Func(fd, FileEndOfFileInfo  , &eof, sizeof(eof));
                SetFileValidData(fd, eof.EndOfFile.QuadPart); // requires elevate_process_privileges().
                lsize  = int64_t(logical_size);
                psize  = int64_t(logical_size);
            }
            else
            {   // no size was specified, so keep whatever the current size is.
                lsize  = psize;
            }
            if (offset < 0)
            {   // position the file pointer at the current end-of-file.
                LARGE_INTEGER dist    = {0};
                LARGE_INTEGER new_ptr = {0};
                SetFilePointerEx(fd, dist, &new_ptr, FILE_END);
                offset = new_ptr.QuadPart;
            }
            else offset = 0;
            return true;
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
        vfs->FileAFID[index]             = req.AFID;
        vfs->FileType[index]             = req.Type;
        vfs->FileMode[index]             = VFS_MODE_LOAD;
        vfs->Priority[index]             = req.Priority;
        vfs->RdOffset[index]             = 0;
        vfs->FileInfo[index].Fildes      = req.Fildes;
        vfs->FileInfo[index].FileSize    = req.FileSize;
        vfs->FileInfo[index].DataSize    = req.DataSize;
        vfs->FileInfo[index].FileOffset  = req.FileOffset;
        vfs->FileInfo[index].SectorSize  = req.SectorSize;
        vfs->FileStat[index].NPendingAIO = 0;
        vfs->FileStat[index].StatusFlags = VFS_STATUS_NONE;
    }
}

/// @summary Attempts to queue a file close request for the AIO driver.
/// @param vfs The VFS driver state to update.
/// @param i The zero-based index of the active file to close.
/// @return The zero-based index of the next record to check.
static size_t vfs_queue_close(vfs_state_t *vfs, size_t i)
{
    if (vfs->FileStat[i].NPendingAIO > 0)
    {   // there are pending AIO operations against this file.
        // the file cannot be closed until all operations have completed.
        // the file close will be queued after the last operation completes.
        vfs->FileStat[i].StatusFlags |= VFS_STATUS_CLOSE;
        return (i + 1);
    }

    // queue the file close operation for the AIO driver.
    aio_req_t *aio_req = io_opq_put(&vfs->IoOperations, vfs->Priority[i]);
    if (aio_req != NULL)
    {   // fill out the request. it will be processed at a later time.
        aio_req->Command    = AIO_COMMAND_CLOSE;
        aio_req->Fildes     = vfs->FileInfo[i].Fildes;
        aio_req->DataAmount = 0;
        aio_req->BaseOffset = vfs->FileInfo[i].FileOffset;
        aio_req->FileOffset = 0;
        aio_req->DataBuffer = NULL;
        aio_req->QTimeNanos = nanotime();
        aio_req->ATimeNanos = 0;
        aio_req->AFID       = vfs->FileAFID[i];
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
        return i;
    }
    else
    {   // there's no more space in the pending I/O operation queue.
        // we'll try closing the file again when there's space.
        vfs->FileStat[i].StatusFlags |= VFS_STATUS_CLOSE;
        return (i + 1);
    }
}

/// @summary Processes any pending file close requests.
/// @param vfs The VFS driver state.
static void vfs_process_closes(vfs_state_t *vfs)
{   // process explicit close requests.
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
        size_t index = 0;
        if (vfs_find_by_afid(vfs, req.AFID, index))
        {   // attempt to queue the close. note that the operation
            // might not get queued immediately, because we could
            // have run out of space in the pending operation queue,
            // or there could be outstanding I/O operations.
            vfs_queue_close(vfs, index);
        }
    }

    // process deferred close requests.
    for (size_t i = 0; i < vfs->ActiveCount; )
    {
        if (vfs->FileStat[i].StatusFlags & VFS_STATUS_CLOSE)
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
static void vfs_process_completed_closes(vfs_state_t *vfs, aio_state_t *aio)
{
    aio_res_t res;
    // there's nothing that the VFS driver needs to do here for the application.
    while (srsw_fifo_get(&aio->CloseResults, res))
    {
        /* empty */
    }
}

/// @summary Processes all completed file read notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
static void vfs_process_completed_reads(vfs_state_t *vfs, aio_state_t *aio)
{
    aio_res_t res;
    size_t    index = 0;
    while (srsw_fifo_get(&aio->ReadResults, res))
    {   // convert the AIO result into something useful for the platform layer.
        vfs_res_t iores;
        iores.AFID       = res.AFID;
        iores.DataBuffer = res.DataBuffer;
        iores.FileOffset = res.FileOffset; // this is the relative offset
        iores.DataAmount = res.DataAmount;
        iores.OSError    = res.OSError;
        if (srsw_fifo_put(&vfs->IoResult[res.Type], iores))
        {   // a single read operation has completed.
            if (vfs_find_by_afid(vfs, res.AFID, index))
            {   // decrement the number of pending I/O operations.
                vfs->FileStat[index].NPendingAIO--;
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
static void vfs_process_completed_writes(vfs_state_t *vfs, aio_state_t *aio)
{
    aio_res_t res;
    size_t    index = 0;
    while (srsw_fifo_get(&aio->WriteResults, res))
    {   // convert the AIO result into something useful for the platform layer.
        // TODO: writes need to be differentiated from reads, obviously.
        vfs_res_t iores;
        iores.AFID       = res.AFID;
        iores.DataBuffer = res.DataBuffer;
        iores.FileOffset = res.FileOffset; // this is the relative offset
        iores.DataAmount = res.DataAmount;
        iores.OSError    = res.OSError;
        if (srsw_fifo_put(&vfs->IoResult[res.Type], iores))
        {   // a single write operation has completed.
            if (vfs_find_by_afid(vfs, res.AFID, index))
            {   // decrement the number of pending I/O operations.
                vfs->FileStat[index].NPendingAIO--;
            }
        }
        else
        {   // TODO: track this statistic somewhere.
            // This should not be happening.
        }
    }
}

/// @summary Processes all completed file flush notifications from AIO.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
static void vfs_process_completed_flushes(vfs_state_t *vfs, aio_state_t *aio)
{
    aio_res_t res;
    // there's nothing that the VFS driver needs to do here for the application.
    // note that on Windows, FlushFileBuffers() is always synchronous, so there
    // is no need to locate and decrement the number of pending AIO operations.
    while (srsw_fifo_get(&aio->FlushResults, res))
    {
        /* empty */
    }
}

/// @summary Processes all pending buffer returns and releases memory back to the pool.
/// @param vfs The VFS driver state.
static void vfs_process_buffer_returns(vfs_state_t *vfs)
{
    for (size_t i = 0; i < FILE_TYPE_COUNT; ++i)
    {
        void          *buffer;
        vfs_returnq_t *returnq   = &vfs->IoReturn[i];
        iobuf_alloc_t &allocator =  vfs->IoAllocator;
        while (srsw_fifo_get(returnq, buffer))
        {
            iobuf_put(allocator, buffer);
        }
    }
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

    // build a priority queue of files, and then process them one at a time
    // starting with the highest-priority file. the goal here is to fill up
    // the queue of pending I/O operations and stay maximally busy.
    io_fpq_clear(&file_queue);
    file_count = map_files_by_mode(index_list, vfs, VFS_MODE_LOAD);
    build_file_queue(&file_queue, vfs, index_list, file_count);
    while(io_fpq_get(&file_queue, index, priority))
    {   // we want to submit as many sequential reads against the file as
        // possible for maximum efficiency. these operations will be
        // processed in-order, so this minimizes seeking as much as possible.
        // stop submitting operations for this file under these conditions:
        // 1. we've reached the end of the file data. continue with the next file.
        // 2. we've run out of pending queue space. stop processing for the tick.
        // 3. we've run out of I/O buffer space. stop processing for the tick.
        // TODO: investigate combining reads to reduce AIO op count?
        size_t nqueued = 0;
        while (iobuf_bytes_free(allocator) > 0)
        {   // allocate a new request in our internal operation queue.
            aio_req_t *req  = io_opq_put(&vfs->IoOperations, priority);
            if (req != NULL)
            {   // populate the (already queued) request.
                req->Command    = AIO_COMMAND_READ;
                req->Fildes     = vfs->FileInfo[index].Fildes;
                req->DataAmount = read_amount;
                req->BaseOffset = vfs->FileInfo[index].FileOffset;
                req->FileOffset = vfs->RdOffset[index];
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
                {   // reached or passed end-of-file; mark as pending close.
                    // processing will continue on with the next file.
                    vfs->FileStat[index].StatusFlags |= VFS_STATUS_CLOSE;
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
        vfs->FileStat[index].NPendingAIO += nqueued;

        // if we ran out of I/O buffer space, there's no point in continuing.
        if (nqueued == 0)
        {   // TODO: track this statistic somewhere.
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
    // TODO: be sure to increment the number of outstanding AIO operations!
    return true;
}

/// @summary Processes all queued explicit write operations.
/// @param vfs The VFS driver state.
/// @return true if the tick should continue submitting I/O operations, or false if
/// either buffer space is full or the I/O operation queue is full.
static bool vfs_process_writes(vfs_state_t *vfs)
{   // TODO: process any pending explicit write operations.
    // TODO: be sure to increment the number of outstanding AIO operations!
    return true;
}

/// @summary Implements the main body of the VFS update loop, which processes
/// requests from the application layer, submits I/O requests to the AIO driver,
/// and dispatches completion notifications from the AIO layer back to the application.
/// @param vfs The VFS driver state.
/// @param aio The AIO driver state.
/// @param stats Optional VFS and AIO counters. May be NULL.
static void vfs_tick(vfs_state_t *vfs, aio_state_t *aio, io_stats_t *stats)
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
    vfs_update_loads(vfs);
    vfs_process_reads(vfs);
    vfs_process_writes(vfs);

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
    vfs_process_opens(vfs);
    vfs_process_loads(vfs);
}

/// @summary Returns an I/O buffer to the pool. This function should be called
/// for every read or write result that the platform layer dequeues.
/// @param vfs The VFS state that posted the I/O result.
/// @param type One of file_type_e indicating the type of file being processed.
/// @param buffer The buffer to return. This value may be NULL.
static void vfs_return_buffer(vfs_state_t *vfs, int32_t type, void *buffer)
{   // TODO: it might be better to check that the buffer does come from
    // the address range of the I/O buffer allocator, to prevent accidental
    // returns of application-managed buffers (for writes, etc.)
    if (buffer != NULL) srsw_fifo_put(&vfs->IoReturn[type], buffer);
}

/// @summary Initialize a VFS driver state object and allocate any I/O resources.
/// @param vfs The VFS driver state to initialize.
/// @return true if the VFS driver state is initialized.
static bool create_vfs_state(vfs_state_t *vfs)
{   // TODO: some error handling would be nice.
    create_iobuf_allocator(vfs->IoAllocator, VFS_IOBUF_SIZE, VFS_ALLOC_SIZE);
    create_srmw_fifo(&vfs->CloseQueue, MAX_OPEN_FILES);
    create_srmw_fifo(&vfs->LoadQueue , MAX_OPEN_FILES);
    for (size_t i = 0; i < FILE_TYPE_COUNT; ++i)
    {
        flush_srsw_fifo(&vfs->IoResult[i]);
        flush_srsw_fifo(&vfs->IoReturn[i]);
    }
    io_opq_clear(&vfs->IoOperations);
    vfs->ActiveCount = 0;
    return true;
}

/// @summary Free resources associated with a VFS driver state.
/// @param vfs The VFS driver state to delete.
static void delete_vfs_state(vfs_state_t *vfs)
{
    vfs->ActiveCount = 0;
    io_opq_clear(&vfs->IoOperations);
    delete_srmw_fifo(&vfs->LoadQueue);
    delete_srmw_fifo(&vfs->CloseQueue);
    delete_iobuf_allocator(vfs->IoAllocator);
    for (size_t i = 0; i < FILE_TYPE_COUNT; ++i)
    {
        flush_srsw_fifo(&vfs->IoResult[i]);
        flush_srsw_fifo(&vfs->IoReturn[i]);
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

/// @summary No-op callback function invoked when the platform I/O system has
/// some data available for processing by the application.
/// @param app_id The application-defined identifier of the source file.
/// @param type One of the values of the file_type_e enumeration.
/// @param data Pointer to the data buffer. The data to read starts at offset 0.
/// @param offset The starting offset of the buffered data within the file.
/// @param size The number of valid bytes in the buffer.
static void null_read_func(intptr_t app_id, int32_t type, void const *data, int64_t offset, uint32_t size)
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
static void null_write_func(intptr_t app_id, int32_t type, void const *data, int64_t offset, uint32_t size)
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
static void null_error_func(intptr_t app_id, int32_t type, uint32_t error_code, char const *error_message)
{
#ifdef DEBUG
    fprintf(stderr, "I/O ERROR: %p(%s): %u(0x%08X): %s\n", app_id, FILE_TYPE_NAME[type], error_code, error_code, strerror(error_code));
#else
    // in release mode, all parameters are unused. suppress compiler warnings.
    (void) sizeof(app_id);
    (void) sizeof(type);
    (void) sizeof(error_code);
    (void) sizeof(error_message);
#endif
}

/// @summary Queues a file for loading. The file is read from beginning to end and
/// data is returned to the application on the thread appropriate for the given type.
/// @param path The NULL-terminated UTF-8 path of the file to load.
/// @param id The application-defined identifier for the load request.
/// @param type One of file_type_e indicating the type of file being loaded. This allows
/// the platform to decide the thread on which data should be returned to the application.
/// @param priority The file loading priority, with 0 indicating the highest possible priority.
/// @param file_size On return, this location is updated with the logical size of the file.
/// @return true if the file was successfully opened and the load was queued.
static bool platform_load_file(char const *path, intptr_t id, int32_t type, uint32_t priority, int64_t &file_size)
{
    HANDLE  fd     = INVALID_HANDLE_VALUE;
    HANDLE  iocp   = AIO_STATE.ASIOContext;
    size_t  ssize  = 0;
    int64_t lsize  = 0;
    int64_t psize  = 0;
    int64_t offset = 0;
    if (vfs_resolve_file_read(path, iocp, fd, lsize, psize, offset, ssize))
    {   // queue a file load request to be processed by the VFS driver.
        vfs_lfreq_t req;
        req.Next       = NULL;
        req.Fildes     = fd;
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

/// @summary Saves a file to disk. If the file exists, it is overwritten. This
/// operation is performed entirely synchronously and will block the calling
/// thread until the file is written. The file is guaranteed to have been either
/// written successfully, or not at all.
/// @param path The path of the file to write.
/// @param data The contents of the file.
/// @param size The number of bytes to read from data and write to the file.
/// @return true if the operation was successful.
static bool platform_save_file(char const *path, void const *data, int64_t size)
{
    // generate a temporary filename (see platform_create_file),
    // vfs_resolve_file_write that path (preallocate the file),
    // write the data and close the file,
    // rename() to the input path
}

/// @summary Opens a file for read-write access. The application is responsible
/// for submitting read and write operations. If the file exists, it is opened
/// without truncation. If the file does not exist, it is created.
/// @param path The path of the file to open.
/// @param id The application-defined identifier of the file.
/// @param type One of file_type_e indicating the type of file being opened. This allows
/// the platform to decide the thread on which data should be returned to the application.
/// @param priority The file operation priority, with 0 indicating the highest possible priority.
/// @param read_only Specify true to open a file as read-only.
/// @param reserve_size The size, in bytes, to preallocate for the file. This makes write
/// operations more efficient. If an estimate is unknown, specify zero.
/// @param file_size On return, this value is updated with the current size of the file, in bytes.
/// @return true if the file was opened successfully.
static bool platform_open_file(char const *path, intptr_t id, int32_t type, uint32_t priority, bool read_only, int64_t reserve_size, int64_t &file_size)
{
    // if read_only is true, use vfs_resolve_file_read(), and support archive files.
    // if read_only is false, use vfs_resolve_file_write(), and support native files only.
}

/// @summary Closes a file explicitly opened for reading or writing.
/// @param id The application-defined identifier associated with the file.
/// @return true if the close request was successfuly queued.
static bool platform_close_file(intptr_t id)
{
    vfs_cfreq_t req;
    req.Next = NULL;
    req.AFID = id;
    return srmw_fifo_put(&VFS_STATE.CloseQueue, req);
}

/// @summary Queues a read operation against an open file. The file should have
/// previously been opened using platform_open_file(), platform_append_file(),
/// or platform_create_file(). Reads starting at arbitrary locations are supported,
/// however, the read may return more or less data than requested.
/// @param id The application-defined identifier of the file.
/// @param offset The absolute byte offset within the file at which to being reading data.
/// @param size The number of bytes to read. The read may return more or less data than requested.
/// @return true if the read operation was successfully submitted.
static bool platform_read_file(intptr_t id, int64_t offset, uint32_t size)
{
    // again, offsets must be sector-aligned. in this case, we can calculate
    // the nearest sector offset <= offset, offset_aligned, and then calculate
    // the size as align_to(size + (offset_aligned - offset), sector_size).
    // when the read completes, we would then have to do some calculations to
    // determine the starting offset of the data in the buffer, and return that
    // to the caller; we would *also* have to make sure that the base buffer
    // pointer is passed through so it can be returned, but all of this complexity
    // occurs in the platform layer.
}

/// @summary Queues a write operation against an open file. The file should have
/// previously been opened using platform_open_file(), platform_append_file() or
/// platform_create_file(). Writes starting at arbitrary locations are not supported;
/// all writes occur at the end of the file. It is possible that not all data is
/// immediately flushed to disk; if this behavior is required, use the function
/// platform_flush_file() to flush any buffered data to disk.
/// @param id The application-defined identifier of the file.
/// @param data The data to write. Do not modify the contents of this buffer
/// until the write completion notification is received.
/// @param size The number of bytes to write.
static bool platform_write_file(intptr_t id, void const *data, uint32_t size)
{
    // note that we may not be able to support a user-specified offset here,
    // because offset must be sector-aligned. so we probably should support
    // only sequential writes to files, and we have to do it in sector-size
    // multiples, so that means we have to perform some internal buffering.
    // in order to support arbitrary offsets, we would have to perform a read
    // followed by the write, which is unacceptable. basically in this case,
    // we will submit writes up to the sector size multiple <= size, and then
    // we have to buffer any remaining data. when the file is closed, if there
    // is any buffered data, we zero-pad up to the nearest sector size multiple,
    // then ftruncate() to set EOF, and then do the close. this means that for
    // each open file, we must maintain a one-sector write buffer, which for
    // the current 128 file limit amounts to up to 512KB of overhead.
    // if a write request is received and data is currently buffered, and the
    // write would cross the sector boundary, copy from the user data buffer
    // to the internal sector buffer, write the sector buffer, and then continue
    // as usual. we could support offsets by failing if they don't fall on a
    // sector boundary, but that seems kind of horrible, and unlikely to succeed.
    // seeking also cannot be supported, because we'd then have to deal with the
    // same issue as supporting arbitrary offsets (we'd have to read and write
    // any buffered data, and then seek.)
    //
    // writes will be blocking unless the file has been preallocated using
    // posix_fadvise(), so platform_open_file() and platform_create_file()
    // should accept an optional file size parameter to perform this
    // preallocation.
}

/// @summary Flushes any pending writes to disk.
/// @param id The application-defined identifier of the file to flush.
/// @return true if the flush operation was successfully queued.
static bool platform_flush_file(intptr_t id)
{
    // this should be pretty straightforward.
}

/// @summary Opens a new temporary file for writing. The file is initially empty.
/// Data may be written to or read from the file using platform_[read/write]_file().
/// When finished, call platform_finalize_file() to close the file and move it to
/// its final destination or delete the file.
/// @param id The application-defined identifier of the file.
/// @param type One of file_type_e indicating the type of file being created. This allows
/// the platform to decide the thread on which data should be returned to the application.
/// @param priority The file operation priority, with 0 indicating the highest possible priority.
/// @param reserve_size The size, in bytes, to preallocate for the file. This makes write
/// operations more efficient. If an estimate is unknown, specify zero.
/// @return true if the file is opened and ready for I/O operations.
static bool platform_create_file(intptr_t id, int32_t type, uint32_t priority, int64_t reserve_size)
{
    // generate a filename using mkostemp(), which also opens the file.
    //
    // On MacOS, use mktemp() with a template of /tmp/temp.XXXXXX, and then open with O_EXCL.
    //
    // On Windows, use GetTempPath() and GetTempFileName().
}

/// @summary Closes a file previously opened using platform_create_file(), and
/// atomically renames that file to move it to the specified path. This function
/// blocks the calling thread until all operations have completed.
/// @param id The application-defined identifier of the file passed to the create file call.
/// @param path The target path and filename of the file, or NULL to delete the file.
/// @return true if the rename or delete was performed successfully.
static bool platform_finalize_file(intptr_t id, char const *path)
{
    // rename() or unlink(). we have the problem that we have only a fd, so on Linux:
    // 1. get the required buffer size using lstat('/proc/self/fd/###') .st_size field.
    // 2. use ssize_t readlink('/proc/self/fd/###', buffer, bufsz) to read the absolute path.
    // 3. close the temporary file.
    // 4. call either rename() or unlink() on the path.
    //
    // On MacOS, use fcntl(fd, F_GETPATH, dst[MAXPATHLEN?]) to get the path, and
    // then use either rename() or unlink().
    //
    // On Windows, use GetFileInformationByHandleEx with FileNameInfo, and then
    // use either MoveFileEx() or DeleteFile().
}

/*////////////////////////
//   Public Functions   //
////////////////////////*/
static aio_state_t AIO_STATE;
static vfs_state_t VFS_STATE;
static io_stats_t  IO_STATS;

/// @summary Entry point of the application.
/// @param argc The number of command-line arguments.
/// @param argv An array of NULL-terminated strings specifying command-line arguments.
/// @return Either EXIT_SUCCESS or EXIT_FAILURE.
int main(int argc, char **argv)
{
    platform_layer_t platform_layer;
    int exit_code = EXIT_SUCCESS;

    // set up the platform layer callbacks:
    platform_layer.print_ioerror = platform_print_ioerror;
    platform_layer.load_file     = platform_load_file;
    platform_layer.save_file     = platform_save_file;
    platform_layer.open_file     = platform_open_file;
    platform_layer.close_file    = platform_close_file;
    platform_layer.read_file     = platform_read_file;
    platform_layer.write_file    = platform_write_file;
    platform_layer.flush_file    = platform_flush_file;
    platform_layer.create_file   = platform_create_file;
    platform_layer.finalize_file = platform_finalize_file;

    // initialize the high-resolution timer on the system.
    inittime();

    // resolve entry points in dynamic libraries.
    // TODO: this will probably need to return success/failure.
    resolve_kernel_apis();
    elevate_process_privileges();

    // TODO: dynamically load the application code.

    exit(exit_code);
}

