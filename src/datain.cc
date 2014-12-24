/*/////////////////////////////////////////////////////////////////////////////
/// @summary Defines the types and functions for parsing input data formats.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////////
//   Preprocessor   //
////////////////////*/

/*/////////////////
//   Constants   //
/////////////////*/

/*///////////////////
//   Local Types   //
///////////////////*/
// how do we handle compressed or encrypted source data being streamed?
// need to decompress/decrypt prior to passing the data to the parser.
// want to do this with as little additional memory usage/copying as possible.
//
// for example, say we have a DEFLATE-compressed stream of DDS data.
// the raw, compressed data is being returned by I/O in 32KB chunks.
// dds_parser_t::push() gets a data_in_stream_t from which it will read data.
// dds_parser_t::push() wants to call data_in_stream_t::refill() to get as
// much data as possible, and will consume all available data and send it to
// the parsing logic.
// the data_in_stream_t::refill() points to a function that will decompress and
// return up to say 4KB at a time, until the buffer is exhausted, in which case
// it returns a special value that tells the parser to yield.
// there are three possible results from calling data_in_stream_t::refill():
// 1. DATA_IN_RESULT_START: BufferBeg = Cursor < BufferEnd, parser calls refill when Cursor = BufferEnd
// 2. DATA_IN_RESULT_YIELD: Cursor = BufferEnd, return from parser
// 3. DATA_IN_RESULT_ERROR: Cursor = unchanged, return from parser
//
// so, the question is, who/where is it determined what creates and initializes
// the data_in_stream_t? probably the VFS layer needs to do this, as it needs to
// have knowledge about file types, archive files, etc. and is what receives the
// raw data from AIO. VFS would receive a vfs_sird_t, and then call the appropriate
// init_data_in_stream() function for the file type (it would keep a global table
// indexed on file_type_e => func ptr.) actually, vfs_process_completed_reads()
// would do the lookup based on the source file type (is it an archive, etc.)
// and attributes, and then set the appropriate init fn ptr in the vfs_sird_t,
// to be called when the read processor dequeues the read items and passes them
// off to the file type handlers. this in turn begs the question, what about
// state that needs to be preserved between completed reads? this would mean
// that the VFS needs to keep another table mapping ASID to data_in_stream_t*,
// and setting this stuff up would best be done when processing the opens, by
// vfs_resolve_file_read() (the data_in_stream_t* would be passed as part of the
// vfs_sics_t, where the VFS driver would create and update the table, but this
// begs yet another question - how are items deleted from this table? [easy
// enough, we need another srmw_fifo_t and push delete requests to the VFS driver]
//
// basically, on the VFS side of the fence, we have the decoder [decode.cc], and
// on the APP side of the fence we have the parser [datain.cc] with the stream
// [bridge.h] in between. I'm not really liking having the parsing done on the
// APP side - it's too much of a pain to maintain the separation. so all of that
// stuff should be done on the platform side, and if necessary, push *specific*
// data over to the application side, like decoded RGBA data, or sound samples.
// the application should not really care that the data came from an OGG or a WAV...
// aside from the raw data, the only thing that needs to be pushed over to the
// application layer is the end-of-stream notification, so the app can decide
// whether to rewind, pause, stop, etc.
//
// as an aside, this design will also work with memory-mapped files, which is a
// nice half-step towards full AIO...once the parsers are implemented, the back
// end can be swapped out with no change to the application code.

/*///////////////
//   Globals   //
///////////////*/

/*///////////////////////
//   Local Functions   //
///////////////////////*/

/*////////////////////////
//   Public Functions   //
////////////////////////*/

