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

/*////////////////////////////
//   Forward Declarations   //
////////////////////////////*/

/*//////////////////
//   Data Types   //
//////////////////*/

/*/////////////////
//   Functions   //
/////////////////*/
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

