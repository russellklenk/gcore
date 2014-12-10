/*/////////////////////////////////////////////////////////////////////////////
/// @summary Forward-declares the types and functions between the platform and
/// game layers of the system. This file is for forward declarations only.
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
typedef void (*data_callback_fn)(uint32_t app_id, int32_t type, void const *data, uint64_t offset, size_t size);

/// @summary Function signature for a callback function invoked when an error
/// occurs while the platform I/O system encounters an error during a file operation.
/// @param app_id The application-defined identifier of the source file.
/// @param type One of the values of the file_type_e enumeration.
/// @param error_code The system error code value.
/// @param error_message An optional string description of the error.
typedef void (*file_error_fn)(uint32_t app_id, int32_t type, int64_t error_code, char const *error_message);

/// @summary Defines the set of callback functions into the application code
/// used for handling data read from files. There is one callback for each
/// defined value of the file_type_e enumeration.
struct io_callbacks_t
{
    data_callback_fn DataForDDS;  /// Callback invoked when data is read from a DDS file.
    data_callback_fn DataForTGA;  /// Callback invoked when data is read from a TGA file.
    data_callback_fn DataForWAV;  /// Callback invoked when data is read from a WAV file.
    data_callback_fn DataForJSON; /// Callback invoked when data is read from a JSON file.
    file_error_fn    IoError;     /// Callback invoked when a system I/O error occurs.
};

/// @summary A single module in the application code must define an instance of
/// io_callbacks_t named IoCallback and initialize it.
extern io_callbacks_t IoCallback;

/*/////////////////
//   Functions   //
/////////////////*/
/// @summary attempts to open a file and read it from beginning to end.
/// @param path the location of the file to load.
/// @param file_type one of the values of the file_type_e enumeration.
/// @param app_id the application-defined identifier associated with the file.
extern bool platform_read_file(char const *path, int32_t file_type, uint32_t app_id);

/// @summary Closes a file previously opened with platform_read_file. This
/// should be called when the application has finished processing the file
/// data, or when the platform has reported an error while reading the file.
/// @param file_type One of the values of the file_type_e enumeration.
/// @param app_id The application-defined identifier of the file to close.
extern void platform_close_file(int32_t file_type, uint32_t app_id);

// TODO: So the big question here is I/O, and whether or not we even want to
// expose the internals of the I/O system to the 'application' layer, having
// queues and whatnot to communicate back and forth, or whether we instead
// want to have the platform layer perform *all* of the I/O, including parsing
// data and creating device objects, etc. and then just notifying the application
// when the operation has succeeded or failed.
//
// The latter option seems better, because only the platform layer knows how the
// application is structured, what runs on what threads, etc. In this case, we
// would have a pretty simple interface, something like this:
//
// bool result = platform_load_file(path, type, app_id);
//
// If type is unknown, the call would fail immediately. Notifying the application
// layer that the operation has completed is kind of tricky. Basically we need a
// callback per-type of file, that the platform layer will call at the appropriate
// time (see next paragraph.)
//
// when the load has completed, the platform has to decide what thread to process
// the completion notification on, since loading some types of files may need to
// complete on a particular thread to access GPU data, etc. most likely, each
// type of file will have its own completion queue that serviced by the platform
// layer in the appropriate location.
//
// virtual file systems are handled internally by the platform layer, since they
// involve filesystem enumeration, path parsing, and so on. internally, everything
// resolves to an AIO operation. at startup, before control was ever transferred
// to the application layer, the platform layer would set up the VFS.
//
// PLAN:
// Prerequisite: The bridge module defines the file and stream types. These
// must be known to both sides.
//
// Prerequisite: The bridge module defines a series of function pointer types
// for the I/O callbacks from platform->application, one per file/stream type.
//
// On startup, the platform layer sets up the VFS.
//
// The application layer submits a request to load a file, or start a stream:
//   bool result = platform_read_file(path, type, app_id)
//                 platform_close_file(type, app_id)
//   bool result = platform_start_stream(path, type, app_id)
//                 platform_cancel_stream(type, app_id)
//   bool result = platform_write_file(path, data, data_size) // synchronous, overwrite
//
// The platform layer pushes data to the application layer as it becomes
// available (which means after the raw I/O has completed, and after any
// decryption, decompression, etc.) through the callback registered for the
// type. The callback must fully process the data it receives before it returns,
// which includes performing any application-level parsing. The callback is
// guaranteed to be invoked on the correct thread by the platform layer. Once
// the application layer returns, the platform layer can free the buffer passed
// to the application layer (or return it to the free pool, etc.)

#endif /* !defined(BRIDGE_H) */

