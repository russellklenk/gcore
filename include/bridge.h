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
    FILE_TYPE_DDS  = 0, /// A DirectDraw Surface file containing image data.
    FILE_TYPE_TGA,      /// A TrueVision Targa file containing image data.
    FILE_TYPE_WAV,      /// A WAV file containing sound sample data.
    FILE_TYPE_JSON,     /// A JSON text file.
    FILE_TYPE_COUNT     /// The number of defined file types. Should always be last.
};

/// @summary Define the different stream-in modes.
enum stream_in_mode_e
{
    STREAM_IN_ONCE = 0, /// Stream in from beginning to end, and close the stream.
    STREAM_IN_LOOP = 1  /// Stream in from beginning to end, and rewind the stream.
};

/// @summary The sentinal value representing an invalid file ID. Applications
/// cannot specify this as the app_id parameter to any I/O functions.
static intptr_t const INVALID_ID = (intptr_t) -1;

/*////////////////////////////
//   Forward Declarations   //
////////////////////////////*/
struct file_t;
struct stream_writer_t;

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
typedef void (*al_stream_in_fn)(intptr_t app_id, int32_t type, void const *data, int64_t offset, uint32_t size);

/// @summary Function signature for a callback function invoked when an error
/// occurs while the platform I/O system encounters an error during a file operation.
/// @param app_id The application-defined identifier of the source file.
/// @param type One of the values of the file_type_e enumeration.
/// @param error_code The system error code value.
/// @param error_message An optional string description of the error.
typedef void (*al_io_error_fn)(intptr_t app_id, int32_t type, uint32_t error_code, char const *error_message);

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
typedef bool (*pl_stream_in_fn)(char const *path, intptr_t id, int32_t type, uint32_t priority, int64_t &stream_size);

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
typedef bool (*pl_open_stream_fn)(char const *path, intptr_t id, int32_t type, uint32_t priority, int32_t mode, bool start, int64_t &stream_size);

/// @summary Pauses a stream without closing the underlying file.
/// @param id The application-defined identifier of the stream.
/// @return true if the stream pause was queued.
typedef bool (*pl_pause_stream_fn)(intptr_t id);

/// @summary Resumes streaming a paused stream.
/// @param id The application-defined identifier of the stream.
/// @return true if the stream start was queued.
typedef bool (*pl_resume_stream_fn)(intptr_t id);

/// @summary Starts streaming data from the beginning of the stream.
/// @param id The application-defined identifier of the stream.
/// @return true if the stream rewind was queued.
typedef bool (*pl_rewind_stream_fn)(intptr_t id);

/// @summary Stops loading a stream and closes the underlying file.
/// @param id The application-defined identifier of the stream.
/// @return true if the stream close was queued.
typedef bool (*pl_stop_stream_fn)(intptr_t id);

/// @summary Opens a file for reading or writing. The file is opened in buffered
/// mode, and all operations will block the calling thread. If the file exists,
/// it is opened and any existing data is preserved. If the file does not exist,
/// it is created and initialized to empty.
/// @param path The path of the file to open.
/// @param read_only Specify true to open the file in read-only mode.
/// @param file_size On return this value indicates the current size of the file, in bytes.
/// @param file On return, this value is set to the file record used for subsequent operations.
/// @return true if the file was opened.
typedef bool (*pl_open_file_fn)(char const *path, bool read_only, int64_t &file_size, file_t **file);

/// @summary Synchronously reads data from a file.
/// @param file The file state returned from open_file().
/// @param offset The absolute byte offset at which to start reading data.
/// @param buffer The buffer into which data will be written.
/// @param size The maximum number of bytes to read.
/// @param bytes_read On return, this value is set to the number of bytes actually
/// read. This may be less than the number of bytes requested, or 0 at end-of-file.
/// @return true if the read operation was successful.
typedef bool (*pl_read_file_fn)(file_t *file, int64_t offset, void *buffer, size_t size, size_t &bytes_read);

/// @summary Synchronously writes data to a file.
/// @param file The file state returned from open_file().
/// @param offset The absolute byte offset at which to start writing data.
/// @param buffer The data to be written to the file.
/// @param size The number of bytes to write to the file.
/// @param bytes_written On return, this value is set to the number of bytes
/// actually written to the file.
/// @return true if the write operation was successful.
typedef bool (*pl_write_file_fn)(file_t *file, int64_t offset, void const *buffer, size_t size, size_t &bytes_written);

/// @summary Flushes any buffered writes to the file, and updates file metadata.
/// @param file The file state returned from open_file().
/// @return true if the flush operation was successful.
typedef bool (*pl_flush_file_fn)(file_t *file);

/// @summary Closes a file.
/// @param file The file state returned from open_file().
/// @return true if the file is closed.
typedef bool (*pl_close_file_fn)(file_t **file);

/// @summary Saves a file to disk. If the file exists, it is overwritten. This
/// operation is performed entirely synchronously and will block the calling
/// thread until the file is written. The file is guaranteed to have been either
/// written successfully, or not at all.
/// @param path The path of the file to write.
/// @param data The contents of the file.
/// @param size The number of bytes to read from data and write to the file.
/// @return true if the operation was successful.
typedef bool (*pl_write_out_fn)(char const *path, void const *data, int64_t size);

/// @summary Opens a new temporary file for writing. The file is initially empty.
/// Data may be written to the file using append_stream(). When finished, call
/// close_stream() to close the file and move it to its final destination.
/// @param where The directory path where the file will be created, or NULL to use the CWD.
/// @param priority The file operation priority, with 0 indicating the highest possible priority.
/// @param reserve_size The size, in bytes, to preallocate for the file. This makes write
/// operations more efficient. Specify zero if unknown.
/// @param writer On return, this value will point to the file writer state.
/// @return true if the file is opened and ready for write operations.
typedef bool (*pl_create_stream_fn)(char const *where, uint32_t priority, int64_t reserve_size, stream_writer_t **writer);

/// @summary Queues a write operation against an open file. The file should have
/// previously been opened using create_stream(). The data is always appended to
/// the end of the file; writes to arbitrary locations are not supported.
/// @param writer The file writer state returned from the create file call.
/// @param data The data to write. Do not modify the contents of this buffer
/// until the write completion notification is received.
/// @param size The number of bytes to write.
/// @param bytes_written On return, this value is updated with the number of bytes written.
/// @return true if the write operation was successful.
typedef bool (*pl_append_stream_fn)(stream_writer_t *writer, void const *data, uint32_t size, size_t &bytes_written);

/// @summary Closes a file previously opened using create_stream(), and atomically
/// renames that file to move it to the specified path.
/// @param writer The stream writer state returned from the create stream call.
/// @param path The target path and filename of the file, or NULL to delete the file.
/// @return true if the finalize operation was successfully queued.
typedef bool (*pl_close_stream_fn)(stream_writer_t **writer, char const *path);

/// @summary Defines the interface that the platform layer provides to the application layer.
struct platform_layer_t
{   // **** I/O Functions
    pl_stream_in_fn      stream_in;     /// Stream-in-once API.
    pl_open_stream_fn    open_stream;   /// Streaming read API.
    pl_pause_stream_fn   pause_stream;  /// Streaming read API.
    pl_resume_stream_fn  resume_stream; /// Streaming read API.
    pl_rewind_stream_fn  rewind_stream; /// Streaming read API.
    pl_stop_stream_fn    stop_stream;   /// Streaming read API.

    pl_open_file_fn      open_file;     /// Synchronous buffered I/O API.
    pl_read_file_fn      read_file;     /// Synchronous buffered I/O API.
    pl_write_file_fn     write_file;    /// Synchronous buffered I/O API.
    pl_flush_file_fn     flush_file;    /// Synchronous buffered I/O API.
    pl_close_file_fn     close_file;    /// Synchronous buffered I/O API.

    pl_write_out_fn      write_out;     /// Write-at-once API.
    pl_create_stream_fn  create_stream; /// Streaming write API.
    pl_append_stream_fn  append_stream; /// Streaming write API.
    pl_close_stream_fn   close_stream;  /// Streaming write API.
};

/*/////////////////
//   Functions   //
/////////////////*/

#endif /* !defined(BRIDGE_H) */

