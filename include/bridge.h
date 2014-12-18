/*/////////////////////////////////////////////////////////////////////////////
/// @summary Forward-declares the types and functions shared between the
/// platform and game layers of the system. This file is for forward
/// declarations only. The platform layer will dynamically load the game layer.
///////////////////////////////////////////////////////////////////////////80*/

#ifndef BRIDGE_H
#define BRIDGE_H

/*////////////////
//   Includes   //
////////////////*/
#include <stddef.h>
#include <stdint.h>

/*/////////////////
//   Constants   //
/////////////////*/
/// @summary Define the set of known file types. When adding a file type, also
/// be sure to add a function pointer entry for the io_callbacks_t structure.
/// These values MUST start at 0, and must be sequential. FILE_TYPE_COUNT must
/// be the last item in the enum definition.
enum file_type_e
{
    FILE_TYPE_DDS   = 0,
    FILE_TYPE_TGA,
    FILE_TYPE_WAV,
    FILE_TYPE_JSON,
    FILE_TYPE_COUNT
};

/// @summary The sentinal value representing an invalid file ID. Applications
/// cannot specify this as the app_id parameter to any I/O functions.
static intptr_t const INVALID_ID = (intptr_t) -1;

/*////////////////////////////
//   Forward Declarations   //
////////////////////////////*/

/*//////////////////
//   Data Types   //
//////////////////*/
/// @summary Function signature for the callback function invoked when the
/// platform I/O system has some data available for processing by the application.
/// @param app_id The application-defined identifier of the source file.
/// @param type One of the values of the file_type_e enumeration.
/// @param data Pointer to the data buffer. The data to read starts at offset 0.
/// @param offset The starting offset of the buffered data within the file.
/// @param size The number of valid bytes in the buffer.
typedef void (*al_read_callback_fn)(intptr_t app_id, int32_t type, void const *data, int64_t offset, uint32_t size);

/// @summary Function signature for the callback function invoked when the
/// platform I/O system has completed writing some data to a file.
/// @param app_id The application-defined identifier of the target file.
/// @param type One of the values of the file_type_e enumeration.
/// @param data Pointer to the data buffer. The data written starts at offset 0.
/// @param offset The byte offset of the start of the write operation within the file.
/// @param size The number of bytes written to the file.
typedef void (*al_write_callback_fn)(intptr_t app_id, int32_t type, void const *data, int64_t offset, uint32_t size);

/// @summary Function signature for a callback function invoked when an error
/// occurs while the platform I/O system encounters an error during a file operation.
/// @param app_id The application-defined identifier of the source file.
/// @param type One of the values of the file_type_e enumeration.
/// @param error_code The system error code value.
/// @param error_message An optional string description of the error.
typedef void (*al_file_error_fn)(intptr_t app_id, int32_t type, uint32_t error_code, char const *error_message);

/// @summary Formats and writes an I/O error description to stderr.
/// @param app_id The application-defined identifier of the file being accessed when the error occurred.
/// @param type The of the values of the file_type_e enumeration.
/// @param error_code The system error code value.
/// @param error_message An optional string description of the error.
typedef void (*pl_print_ioerror_fn)(intptr_t app_id, int32_t type, uint32_t error_code, char const *error_message);

/// @summary Queues a file for loading. The file is read from beginning to end and
/// data is returned to the application on the thread appropriate for the given type.
/// The file is close automatically when all data has been read, or an error occurs.
/// @param path The NULL-terminated UTF-8 path of the file to load.
/// @param id The application-defined identifier for the load request.
/// @param type One of file_type_e indicating the type of file being loaded. This allows
/// the platform to decide the thread on which data should be returned to the application.
/// @param priority The file loading priority, with 0 indicating the highest possible priority.
/// @param file_size On return, this location is updated with the logical size of the file.
/// @return true if the file was successfully opened and the load was queued.
typedef bool (*pl_load_file_fn)(char const *path, intptr_t id, int32_t type, uint32_t priority, int64_t &file_size);

/// @summary Saves a file to disk. If the file exists, it is overwritten. This
/// operation is performed entirely synchronously and will block the calling
/// thread until the file is written. The file is guaranteed to have been either
/// written successfully, or not at all.
/// @param path The path of the file to write.
/// @param data The contents of the file.
/// @param size The number of bytes to read from data and write to the file.
/// @return true if the operation was successful.
typedef bool (*pl_save_file_fn)(char const *path, void const *data, int64_t size);

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
typedef bool (*pl_open_file_fn)(char const *path, intptr_t id, int32_t type, uint32_t priority, bool read_only, int64_t reserve_size, int64_t &file_size);

/// @summary Closes a file opened using platform_open_file().
/// @param id The application-defined identifier associated with the file.
/// @return true if the close request was successfuly queued.
typedef bool (*pl_close_file_fn)(intptr_t id);

/// @summary Queues a read operation against an open file. The file should have
/// previously been opened using platform_open_file(), platform_append_file(),
/// or platform_create_file(). Reads starting at arbitrary locations are supported,
/// however, the read may return more or less data than requested.
/// @param id The application-defined identifier of the file.
/// @param offset The absolute byte offset within the file at which to being reading data.
/// @param size The number of bytes to read. The read may return more or less data than requested.
/// @return true if the read operation was successfully submitted.
typedef bool (*pl_read_file_fn)(intptr_t id, int64_t offset, uint32_t size);

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
typedef bool (*pl_write_file_fn)(intptr_t id, void const *data, uint32_t size);

/// @summary Flushes any pending writes to disk.
/// @param id The application-defined identifier of the file to flush.
/// @return true if the flush operation was successfully queued.
typedef bool (*pl_flush_file_fn)(intptr_t id);

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
typedef bool (*pl_create_file_fn)(intptr_t id, int32_t type, uint32_t priority, int64_t reserve_size);

/// @summary Closes a file previously opened using platform_create_file(), and
/// atomically renames that file to move it to the specified path. This function
/// blocks the calling thread until all operations have completed.
/// @param id The application-defined identifier of the file passed to the create file call.
/// @param path The target path and filename of the file, or NULL to delete the file.
/// @return true if the rename or delete was performed successfully.
typedef bool (*pl_finalize_file_fn)(intptr_t id, char const *path);

/// @summary Defines the interface that the platform layer provides to the application layer.
struct platform_layer_t
{   // **** I/O Functions
    pl_print_ioerror_fn  print_ioerror; /// Print a formatted I/O error to stderr.
    pl_load_file_fn      load_file;     /// Open a file, read it all, and close it.
    pl_save_file_fn      save_file;     /// Create a file, write it all, and close it.
    pl_open_file_fn      open_file;     /// Open a file for explicit I/O.
    pl_close_file_fn     close_file;    /// Close a file opened for explicit I/O.
    pl_read_file_fn      read_file;     /// Explicitly read data from a file.
    pl_write_file_fn     write_file;    /// Explicitly append data to a file.
    pl_flush_file_fn     flush_file;    /// Explicitly flush all pending writes.
    pl_create_file_fn    create_file;   /// Create a new file for explicit I/O.
    pl_finalize_file_fn  finalize_file; /// Close a newly created file.
};

/*/////////////////
//   Functions   //
/////////////////*/

#endif /* !defined(BRIDGE_H) */

