/*/////////////////////////////////////////////////////////////////////////////
/// @summary Defines the types and functions implementing various streaming
/// decoders, that transform data being streamed in to the application before
/// sending the data off to a parser. The decoders are designed to operate with
/// a small, fixed memory footprint, and to minimize the need to copy data.
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
/// @summary Defines the possible results from calling stream_decoder_t::refill().
/// These values instruct a parser on how to continue processing.
enum decode_result_e
{
    DECODE_RESULT_START = 0,  /// At least one byte of data is available for parsing.
    DECODE_RESULT_YIELD = 1,  /// No more input is currently available, try again later.
    DECODE_RESULT_ERROR = 2   /// An error was encountered processing the input. Halt.
};

/// @summary Define the error codes that may be set by a stream decoder. The
/// errors for all implemented decoders are listed here.
enum decode_error_e
{
    DECODE_ERROR_NONE   = 0,  /// No error has occurred.
    DECODE_ERROR_EOF    = 1,  /// Attempt to read beyond the end of the file.
};

/// @summary Defines the 'base class' for all types of stream decoders. Each
/// derived type that needed to perform a decode transformation might maintain
/// a small, fixed-size buffer consumed by the parser, and refilled when the
/// parser calls the stream_decoder_t::refill() function. It is important that
/// the refill function be a function pointer, and not a virtual function, so
/// that state machines can be implemented. Decoders can be layered.
struct stream_decoder_t
{
    uint8_t  *BufferBeg;      /// The start of the buffer of available data.
    uint8_t  *BufferEnd;      /// One past the end of the buffer of available data.
    uint8_t  *Cursor;         /// The current read cursor within the buffer.
    int32_t   DecodeError;    /// The sticky error value for the decoder.
    int32_t (*refill)(stream_decoder_t *s); /// Decode the next chunk. Returns decode_result_e.
};

/*///////////////
//   Globals   //
///////////////*/

/*///////////////////////
//   Local Functions   //
///////////////////////*/
/// @summary Stream refill implementation that returns an error status, and
/// does not refill the internal stream buffer.
/// @param s The stream decoder being refilled.
/// @return One of decode_result_e specifying the current status (DECODE_RESULT_ERROR).
internal_function int32_t refill_error(stream_decoder_t *s)
{
    return DECODE_RESULT_ERROR;
}

/// @summary Stream refill implementation that returns a yield status, indicating
/// that no more data is immediately available, but no error has occurred, and
/// does not refill the internal stream buffer.
/// @param s The stream decoder being refilled.
/// @return One of decode_result_e specifying the current status (DECODE_RESULT_YIELD).
internal_function int32_t refill_yield(stream_decoder_t *s)
{
    return DECODE_RESULT_YIELD;
}

/// @summary Helper function that sets the error status of a stream decoder, and
/// also sets the refill function to refill_error().
/// @param s The stream decoder that encountered the error.
/// @param error One of decode_error_e specifying the error encountered by the decoder.
/// @return One of decode_result_e specifying the current status (DECODE_RESULT_ERROR).
internal_function int32_t decode_fail(stream_decoder_t *s, int32_t error)
{
    s->DecodeError = error;
    s->refill      = refill_error;
    return s->refill(s);
}

/// @summary Implements a dummy refill function that refills the buffer with zero-bytes.
/// @param s The stream decoder being refilled.
/// @return One of decode_result_e specifing the current status (DECODE_RESULT_START).
internal_function int32_t refill_zeroes(stream_decoder_t *s)
{
    local_persist  uint8_t ZERO_DATA[256] = {0};
    s->BufferBeg = ZERO_DATA;
    s->BufferEnd = ZERO_DATA + sizeof(ZERO_DATA);
    s->Cursor    = ZERO_DATA;
    return DECODE_RESULT_START;
}

/// @summary Implements the refill function for a fixed-length (non-streaming)
/// memory buffer. Attempting to read beyond the end of the buffer results in
/// an end-of-file error being set on the stream.
/// @param s The stream decoder being refilled.
/// @return One of decode_resule_e specifying the current status (DECODE_RESULT_ERROR).
internal_function int32_t refill_memory(stream_decoder_t *s)
{   // an attempt to refill a fixed-length memory stream fails.
    return decode_fail(s, DECODE_ERROR_EOF);
}

/*////////////////////////
//   Public Functions   //
////////////////////////*/
/// @summary Initializes a decoder for a chunked stream that does not perform
/// any transformation on the stream data. When the current buffer is exhausted,
/// the stream will cause the parser to yield.
/// @param s The stream decoder being initialized.
/// @param buffer The buffer being attached to the stream.
/// @param size The number of bytes being attached to the stream.
public_function void init_stream_decoder(stream_decoder_t *s, void *buffer, size_t size)
{
    s->BufferBeg   = (uint8_t*) buffer;
    s->BufferEnd   = (uint8_t*) buffer + size;
    s->Cursor      = (uint8_t*) buffer;
    s->DecodeError = DECODE_ERROR_NONE;
    s->refill      = refill_yield;
}

/// @summary Initializes a decoder for a fixed-length memory stream that does
/// not perform any transformation on the stream data.
/// @param s The stream decoder being initialized.
/// @param buffer The buffer being attached to the stream.
/// @param size The number of bytes being attached to the stream.
public_function void init_memory_decoder(stream_decoder_t *s, void *buffer, size_t size)
{
    s->BufferBeg   = (uint8_t*) buffer;
    s->BufferEnd   = (uint8_t*) buffer + size;
    s->Cursor      = (uint8_t*) buffer;
    s->DecodeError = DECODE_ERROR_NONE;
    s->refill      = refill_memory;
}

