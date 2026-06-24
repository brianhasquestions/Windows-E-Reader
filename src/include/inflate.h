/*****************************************************************************
 * inflate.h
 *
 * Self-contained DEFLATE (RFC 1951) decompressor. ZIP archives (and thus
 * both EPUB and CBZ files) store their members using the raw DEFLATE stream,
 * so the application needs an inflater that depends on nothing but the C
 * runtime. The uncompressed size is always known up front from the ZIP
 * central directory, which lets the caller allocate an exact destination
 * buffer and keeps this module simple.
 *****************************************************************************/

#ifndef INFLATE_H
#define INFLATE_H

#include "ereader.h"

/* Alphabet and code-length limits defined by RFC 1951. */
#define INFLATE_MAX_BITS        15      /* longest Huffman code length          */
#define INFLATE_MAX_LIT_CODES   286     /* literal/length alphabet size         */
#define INFLATE_MAX_DIST_CODES  30      /* distance alphabet size               */
#define INFLATE_FIXED_LIT_CODES 288     /* fixed-block literal alphabet size    */

/*
 * A single request describing one DEFLATE stream to decompress. Bundled into
 * a struct so the public entry point stays within the three-parameter rule.
 */
typedef struct INFLATE_REQUEST
{
    const unsigned char *sourceData;        /* raw DEFLATE bytes              */
    unsigned long        sourceLength;       /* number of compressed bytes    */
    unsigned char       *destinationData;    /* caller-allocated output buffer */
    unsigned long        destinationLength;  /* exact expected output size    */
} INFLATE_REQUEST;

/* Running state for one inflate operation: the bit-level cursor over input. */
typedef struct INFLATE_STATE
{
    unsigned char *outputBuffer;
    unsigned long  outputLength;
    unsigned long  outputCount;

    const unsigned char *inputBuffer;
    unsigned long        inputLength;
    unsigned long        inputCount;

    int bitBuffer;                      /* bits not yet consumed               */
    int bitCount;                       /* number of valid bits in bitBuffer   */
} INFLATE_STATE;

/* A canonical Huffman table: symbol counts per length plus sorted symbols. */
typedef struct HUFFMAN_TABLE
{
    short *symbolCountPerLength;        /* index by code length (0..MAX_BITS)  */
    short *sortedSymbols;               /* symbols ordered by code             */
} HUFFMAN_TABLE;

/* Cursor used while filling the code-length array of a dynamic block. */
typedef struct INFLATE_LENGTH_FILL
{
    int filledCount;        /* code lengths written so far                  */
    int totalLengthCount;   /* literal + distance code lengths expected      */
} INFLATE_LENGTH_FILL;

/* All scratch storage for one operation, allocated dynamically by the caller. */
typedef struct INFLATE_WORK
{
    INFLATE_STATE  state;
    HUFFMAN_TABLE  lengthCode;
    HUFFMAN_TABLE  distanceCode;
    short          lengthCountStorage[INFLATE_MAX_BITS + 1];
    short          lengthSymbolStorage[INFLATE_FIXED_LIT_CODES];
    short          distanceCountStorage[INFLATE_MAX_BITS + 1];
    short          distanceSymbolStorage[INFLATE_MAX_DIST_CODES];
    short          codeLengths[INFLATE_MAX_LIT_CODES + INFLATE_MAX_DIST_CODES];
} INFLATE_WORK;

/*
 * Decompresses the DEFLATE stream described by the request. Returns TRUE on
 * success; the destination buffer is then filled with exactly
 * destinationLength bytes. Returns FALSE on any malformed-stream condition.
 */
BOOL Inflate_Decompress(INFLATE_REQUEST *inflateRequest);

#endif /* INFLATE_H */
