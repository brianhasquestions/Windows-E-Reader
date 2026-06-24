/*****************************************************************************
 * inflate.c
 *
 * DEFLATE (RFC 1951) decompressor. The decode strategy is the classic
 * canonical-Huffman approach: build per-block length and distance code
 * tables, then emit literals and resolve LZ77 back-references. Because the
 * exact output size is known from the ZIP central directory, the destination
 * buffer is pre-sized and every write is bounds-checked against it.
 *
 * The state, Huffman-table, and work structures live in inflate.h.
 *****************************************************************************/

#include "inflate.h"

/* Bit-stream layout constants. */
#define INFLATE_BITS_PER_BYTE                8
#define INFLATE_FINAL_BLOCK_FLAG_BITS        1
#define INFLATE_BLOCK_TYPE_BITS              2
#define INFLATE_SINGLE_BIT                   1

/* Block types from the three-bit block header. */
#define INFLATE_BLOCK_TYPE_STORED            0
#define INFLATE_BLOCK_TYPE_FIXED             1
#define INFLATE_BLOCK_TYPE_DYNAMIC           2

/* Literal/length alphabet landmarks. */
#define INFLATE_END_OF_BLOCK_SYMBOL          256
#define INFLATE_FIRST_LENGTH_SYMBOL          257
#define INFLATE_LENGTH_SYMBOL_COUNT          29

/* Dynamic-block header field widths and their stored-minus-offset bias. */
#define INFLATE_LITERAL_COUNT_BITS           5
#define INFLATE_DISTANCE_COUNT_BITS          5
#define INFLATE_CODE_LENGTH_COUNT_BITS       4
#define INFLATE_LITERAL_COUNT_OFFSET         257
#define INFLATE_DISTANCE_COUNT_OFFSET        1
#define INFLATE_CODE_LENGTH_COUNT_OFFSET     4

/* Code-length code alphabet (the code that encodes the other code lengths). */
#define INFLATE_CODE_LENGTH_SYMBOLS          19
#define INFLATE_CODE_LENGTH_BITS             3
#define INFLATE_DIRECT_LENGTH_LIMIT          16   /* symbols < 16 are literal lengths */
#define INFLATE_REPEAT_PREVIOUS_SYMBOL       16
#define INFLATE_REPEAT_ZERO_SHORT_SYMBOL     17
#define INFLATE_REPEAT_ZERO_LONG_SYMBOL      18
#define INFLATE_REPEAT_PREVIOUS_EXTRA_BITS   2
#define INFLATE_REPEAT_PREVIOUS_MINIMUM      3
#define INFLATE_REPEAT_ZERO_SHORT_EXTRA_BITS 3
#define INFLATE_REPEAT_ZERO_SHORT_MINIMUM    3
#define INFLATE_REPEAT_ZERO_LONG_EXTRA_BITS  7
#define INFLATE_REPEAT_ZERO_LONG_MINIMUM     11

/* Stored (uncompressed) block: LEN and ~LEN, two bytes each. */
#define INFLATE_STORED_HEADER_BYTES          4

/* Fixed-block code-length assignment boundaries (RFC 1951 section 3.2.6). */
#define INFLATE_FIXED_LIT_8BIT_BOUNDARY      144
#define INFLATE_FIXED_LIT_9BIT_BOUNDARY      256
#define INFLATE_FIXED_LIT_7BIT_BOUNDARY      280
#define INFLATE_FIXED_LENGTH_8BIT            8
#define INFLATE_FIXED_LENGTH_9BIT            9
#define INFLATE_FIXED_LENGTH_7BIT            7
#define INFLATE_FIXED_DISTANCE_LENGTH        5

/* Error sentinels returned by the decode helpers (any negative value fails). */
#define INFLATE_ERROR_INPUT_EXHAUSTED        (-1)
#define INFLATE_ERROR_BAD_CODE               (-9)
#define INFLATE_ERROR_OUT_OF_MEMORY          (-10)

/* Base lengths and extra bits for the length alphabet (symbols 257..285). */
static const short g_lengthBase[INFLATE_LENGTH_SYMBOL_COUNT] =
{
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const short g_lengthExtraBits[INFLATE_LENGTH_SYMBOL_COUNT] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* Base distances and extra bits for the distance alphabet. */
static const short g_distanceBase[INFLATE_MAX_DIST_CODES] =
{
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const short g_distanceExtraBits[INFLATE_MAX_DIST_CODES] =
{
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Order in which code-length code lengths appear in a dynamic block header. */
static const short g_codeLengthOrder[INFLATE_CODE_LENGTH_SYMBOLS] =
{
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/*
 * Pulls bitsNeeded bits from the stream, least-significant bit first.
 * Returns the value, or a negative sentinel when the input is exhausted.
 */
static int Inflate_ReadBits(INFLATE_STATE *state, int bitsNeeded)
{
    long accumulatedValue = 0L;
    int  resultValue = INFLATE_ERROR_INPUT_EXHAUSTED;

    accumulatedValue = (long)state->bitBuffer;

    while (state->bitCount < bitsNeeded)
    {
        if (state->inputCount == state->inputLength)
        {
            resultValue = INFLATE_ERROR_INPUT_EXHAUSTED;
            goto CLEANUP;
        }

        accumulatedValue |= (long)(state->inputBuffer[state->inputCount]) << state->bitCount;
        state->inputCount += 1UL;
        state->bitCount += INFLATE_BITS_PER_BYTE;
    }

    state->bitBuffer = (int)(accumulatedValue >> bitsNeeded);
    state->bitCount -= bitsNeeded;
    resultValue = (int)(accumulatedValue & ((1L << bitsNeeded) - 1L));

CLEANUP:
    return resultValue;
}

/*
 * Decodes a single symbol using the supplied Huffman table. Returns the
 * symbol, or a negative value on a malformed code / exhausted input.
 */
static int Inflate_DecodeSymbol(INFLATE_STATE *state, const HUFFMAN_TABLE *huffmanTable)
{
    int code = 0;
    int firstCodeOfLength = 0;
    int symbolIndex = 0;
    int currentLength = 0;
    int decodedSymbol = INFLATE_ERROR_INPUT_EXHAUSTED;

    for (currentLength = 1; currentLength <= INFLATE_MAX_BITS; currentLength += 1)
    {
        int nextBit = 0;
        int countAtLength = 0;

        nextBit = Inflate_ReadBits(state, INFLATE_SINGLE_BIT);
        if (0 > nextBit)
        {
            decodedSymbol = INFLATE_ERROR_INPUT_EXHAUSTED;
            goto CLEANUP;
        }

        code |= nextBit;
        countAtLength = (int)huffmanTable->symbolCountPerLength[currentLength];

        if ((code - countAtLength) < firstCodeOfLength)
        {
            decodedSymbol = (int)huffmanTable->sortedSymbols[symbolIndex + (code - firstCodeOfLength)];
            goto CLEANUP;
        }

        symbolIndex += countAtLength;
        firstCodeOfLength += countAtLength;
        firstCodeOfLength <<= 1;
        code <<= 1;
    }

    decodedSymbol = INFLATE_ERROR_BAD_CODE;     /* ran past the longest legal code */

CLEANUP:
    return decodedSymbol;
}

/*
 * Builds a canonical Huffman table from an array of symbolCount code lengths.
 * Returns 0 for a complete code, a positive value for an incomplete code, or
 * a negative value for an over-subscribed (invalid) code or allocation error.
 */
static int Inflate_BuildHuffman(HUFFMAN_TABLE *huffmanTable, const short *symbolLengths, int symbolCount)
{
    int    symbolIndex = 0;
    int    lengthIndex = 0;
    int    leftoverCodes = 0;
    short *lengthOffsets = NULL;
    int    returnValue = 0;

    lengthOffsets = (short *)Platform_AllocateZeroed((size_t)(INFLATE_MAX_BITS + 1), sizeof(short));
    if (NULL == lengthOffsets)
    {
        returnValue = INFLATE_ERROR_OUT_OF_MEMORY;
        goto CLEANUP;
    }

    for (lengthIndex = 0; lengthIndex <= INFLATE_MAX_BITS; lengthIndex += 1)
    {
        huffmanTable->symbolCountPerLength[lengthIndex] = 0;
    }

    for (symbolIndex = 0; symbolIndex < symbolCount; symbolIndex += 1)
    {
        huffmanTable->symbolCountPerLength[symbolLengths[symbolIndex]] += 1;
    }

    if (symbolCount == (int)huffmanTable->symbolCountPerLength[0])
    {
        returnValue = 0;    /* no codes at all is a valid (empty) code */
        goto CLEANUP;
    }

    leftoverCodes = 1;
    for (lengthIndex = 1; lengthIndex <= INFLATE_MAX_BITS; lengthIndex += 1)
    {
        leftoverCodes <<= 1;
        leftoverCodes -= (int)huffmanTable->symbolCountPerLength[lengthIndex];
        if (0 > leftoverCodes)
        {
            returnValue = leftoverCodes;    /* over-subscribed */
            goto CLEANUP;
        }
    }

    lengthOffsets[1] = 0;
    for (lengthIndex = 1; lengthIndex < INFLATE_MAX_BITS; lengthIndex += 1)
    {
        lengthOffsets[lengthIndex + 1] = (short)(lengthOffsets[lengthIndex] + huffmanTable->symbolCountPerLength[lengthIndex]);
    }

    for (symbolIndex = 0; symbolIndex < symbolCount; symbolIndex += 1)
    {
        if (0 != symbolLengths[symbolIndex])
        {
            huffmanTable->sortedSymbols[lengthOffsets[symbolLengths[symbolIndex]]] = (short)symbolIndex;
            lengthOffsets[symbolLengths[symbolIndex]] += 1;
        }
    }

    returnValue = leftoverCodes;

CLEANUP:
    SAFE_FREE(lengthOffsets);
    return returnValue;
}

/* Writes one literal byte to the output, bounds-checked against the end of the
 * destination buffer. Returns TRUE on success. */
static BOOL Inflate_EmitLiteral(INFLATE_STATE *state, int literalByte)
{
    BOOL successResult = FALSE;

    if (state->outputCount >= state->outputLength)
    {
        goto CLEANUP;           /* would overrun the destination buffer */
    }
    state->outputBuffer[state->outputCount] = (unsigned char)literalByte;
    state->outputCount += 1UL;
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/*
 * Resolves one LZ77 back-reference whose length symbol has already been
 * decoded: reads the length's extra bits, decodes the distance symbol and its
 * extra bits, validates both against the output produced so far, then copies
 * the run from behind the write cursor. Returns TRUE on success. Extracted from
 * the decode loop so that loop stays a shallow literal-vs-reference dispatch.
 */
static BOOL Inflate_CopyBackReference(INFLATE_WORK *work, int lengthSymbol)
{
    INFLATE_STATE *state = NULL;
    int lengthSlot = 0;
    int distanceSlot = 0;
    int extraBitsValue = 0;
    unsigned long copyLength = 0UL;
    unsigned long copyDistance = 0UL;
    BOOL successResult = FALSE;

    state = &work->state;

    lengthSlot = lengthSymbol - INFLATE_FIRST_LENGTH_SYMBOL;
    if (INFLATE_LENGTH_SYMBOL_COUNT <= lengthSlot)
    {
        goto CLEANUP;
    }

    extraBitsValue = Inflate_ReadBits(state, (int)g_lengthExtraBits[lengthSlot]);
    if (0 > extraBitsValue)
    {
        goto CLEANUP;
    }
    copyLength = (unsigned long)g_lengthBase[lengthSlot] + (unsigned long)extraBitsValue;

    distanceSlot = Inflate_DecodeSymbol(state, &work->distanceCode);
    if (0 > distanceSlot)
    {
        goto CLEANUP;
    }

    extraBitsValue = Inflate_ReadBits(state, (int)g_distanceExtraBits[distanceSlot]);
    if (0 > extraBitsValue)
    {
        goto CLEANUP;
    }
    copyDistance = (unsigned long)g_distanceBase[distanceSlot] + (unsigned long)extraBitsValue;

    if (copyDistance > state->outputCount)
    {
        goto CLEANUP;           /* reference points before the output start */
    }
    if (copyLength > (state->outputLength - state->outputCount))
    {
        goto CLEANUP;           /* would overrun the destination buffer */
    }

    while (0UL != copyLength)
    {
        state->outputBuffer[state->outputCount] = state->outputBuffer[state->outputCount - copyDistance];
        state->outputCount += 1UL;
        copyLength -= 1UL;
    }
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/*
 * Emits the literals and resolves the back-references for one compressed block
 * using the given length and distance tables. The per-symbol work lives in
 * Inflate_EmitLiteral / Inflate_CopyBackReference, so this is a flat dispatch:
 * literal, back-reference, or end-of-block. Returns TRUE on success.
 */
static BOOL Inflate_DecodeBlock(INFLATE_WORK *work)
{
    INFLATE_STATE *state = NULL;
    int decodedSymbol = 0;
    BOOL successResult = FALSE;

    state = &work->state;

    do
    {
        decodedSymbol = Inflate_DecodeSymbol(state, &work->lengthCode);
        if (0 > decodedSymbol)
        {
            goto CLEANUP;
        }

        if (INFLATE_END_OF_BLOCK_SYMBOL > decodedSymbol)
        {
            if (FALSE == Inflate_EmitLiteral(state, decodedSymbol))
            {
                goto CLEANUP;
            }
        }
        else if (INFLATE_END_OF_BLOCK_SYMBOL < decodedSymbol)
        {
            if (FALSE == Inflate_CopyBackReference(work, decodedSymbol))
            {
                goto CLEANUP;
            }
        }
    } while (INFLATE_END_OF_BLOCK_SYMBOL != decodedSymbol);

    successResult = TRUE;

CLEANUP:
    return successResult;
}

/* Copies a stored (uncompressed) block straight from input to output. */
static BOOL Inflate_StoredBlock(INFLATE_WORK *work)
{
    INFLATE_STATE *state = NULL;
    unsigned long blockLength = 0UL;
    BOOL successResult = FALSE;

    state = &work->state;

    /* Stored blocks are byte aligned, so drop any buffered partial byte. */
    state->bitBuffer = 0;
    state->bitCount = 0;

    if ((state->inputCount + (unsigned long)INFLATE_STORED_HEADER_BYTES) > state->inputLength)
    {
        goto CLEANUP;
    }

    blockLength = (unsigned long)state->inputBuffer[state->inputCount] | ((unsigned long)state->inputBuffer[state->inputCount + 1UL] << INFLATE_BITS_PER_BYTE);
    /* Bytes 2 and 3 are the one's complement of the length; integrity only. */
    state->inputCount += (unsigned long)INFLATE_STORED_HEADER_BYTES;

    if ((state->inputCount + blockLength) > state->inputLength)
    {
        goto CLEANUP;
    }
    if (blockLength > (state->outputLength - state->outputCount))
    {
        goto CLEANUP;
    }

    while (0UL != blockLength)
    {
        state->outputBuffer[state->outputCount] = state->inputBuffer[state->inputCount];
        state->outputCount += 1UL;
        state->inputCount += 1UL;
        blockLength -= 1UL;
    }

    successResult = TRUE;

CLEANUP:
    return successResult;
}

/* Configures the fixed Huffman tables defined by RFC 1951 and decodes. */
static BOOL Inflate_FixedBlock(INFLATE_WORK *work)
{
    int symbolIndex = 0;
    BOOL successResult = FALSE;

    for (symbolIndex = 0; symbolIndex < INFLATE_FIXED_LIT_8BIT_BOUNDARY; symbolIndex += 1)
    {
        work->codeLengths[symbolIndex] = INFLATE_FIXED_LENGTH_8BIT;
    }
    for (symbolIndex = INFLATE_FIXED_LIT_8BIT_BOUNDARY; symbolIndex < INFLATE_FIXED_LIT_9BIT_BOUNDARY; symbolIndex += 1)
    {
        work->codeLengths[symbolIndex] = INFLATE_FIXED_LENGTH_9BIT;
    }
    for (symbolIndex = INFLATE_FIXED_LIT_9BIT_BOUNDARY; symbolIndex < INFLATE_FIXED_LIT_7BIT_BOUNDARY; symbolIndex += 1)
    {
        work->codeLengths[symbolIndex] = INFLATE_FIXED_LENGTH_7BIT;
    }
    for (symbolIndex = INFLATE_FIXED_LIT_7BIT_BOUNDARY; symbolIndex < INFLATE_FIXED_LIT_CODES; symbolIndex += 1)
    {
        work->codeLengths[symbolIndex] = INFLATE_FIXED_LENGTH_8BIT;
    }
    (void)Inflate_BuildHuffman(&work->lengthCode, work->codeLengths, INFLATE_FIXED_LIT_CODES);

    for (symbolIndex = 0; symbolIndex < INFLATE_MAX_DIST_CODES; symbolIndex += 1)
    {
        work->codeLengths[symbolIndex] = INFLATE_FIXED_DISTANCE_LENGTH;
    }
    (void)Inflate_BuildHuffman(&work->distanceCode, work->codeLengths, INFLATE_MAX_DIST_CODES);

    successResult = Inflate_DecodeBlock(work);

    goto CLEANUP;

CLEANUP:
    return successResult;
}

/*
 * Applies one decoded code-length symbol to the code-lengths array, advancing
 * the fill cursor. Direct lengths (0..15) are stored verbatim; the repeat
 * codes (16/17/18) replicate a value a run-length number of times. Returns
 * TRUE on success. Kept separate so the decode loop stays shallow and the
 * repeat-code dispatch can use a switch.
 */
static BOOL Inflate_ApplyCodeLengthSymbol(INFLATE_WORK *work, int decodedSymbol, INFLATE_LENGTH_FILL *lengthFill)
{
    short repeatedValue = 0;
    int   repeatCount = 0;
    int   extraBitCount = 0;
    int   extraBitsValue = 0;
    BOOL  successResult = FALSE;

    /* A direct code length (0..15) is stored as-is. */
    if (INFLATE_DIRECT_LENGTH_LIMIT > decodedSymbol)
    {
        work->codeLengths[lengthFill->filledCount] = (short)decodedSymbol;
        lengthFill->filledCount += 1;
        successResult = TRUE;
        goto CLEANUP;
    }

    /* Otherwise it is one of the three repeat codes; pick its run parameters. */
    switch (decodedSymbol)
    {
        case INFLATE_REPEAT_PREVIOUS_SYMBOL:
            if (0 == lengthFill->filledCount)
            {
                goto CLEANUP;       /* nothing precedes this to repeat */
            }
            repeatedValue = work->codeLengths[lengthFill->filledCount - 1];
            extraBitCount = INFLATE_REPEAT_PREVIOUS_EXTRA_BITS;
            repeatCount = INFLATE_REPEAT_PREVIOUS_MINIMUM;
            break;

        case INFLATE_REPEAT_ZERO_SHORT_SYMBOL:
            extraBitCount = INFLATE_REPEAT_ZERO_SHORT_EXTRA_BITS;
            repeatCount = INFLATE_REPEAT_ZERO_SHORT_MINIMUM;
            break;

        case INFLATE_REPEAT_ZERO_LONG_SYMBOL:
            extraBitCount = INFLATE_REPEAT_ZERO_LONG_EXTRA_BITS;
            repeatCount = INFLATE_REPEAT_ZERO_LONG_MINIMUM;
            break;

        default:
            goto CLEANUP;           /* not a valid code-length symbol */
    }

    extraBitsValue = Inflate_ReadBits(&work->state, extraBitCount);
    if (0 > extraBitsValue)
    {
        goto CLEANUP;
    }
    repeatCount += extraBitsValue;

    if ((lengthFill->filledCount + repeatCount) > lengthFill->totalLengthCount)
    {
        goto CLEANUP;               /* run would overflow the code-length array */
    }
    while (0 != repeatCount)
    {
        work->codeLengths[lengthFill->filledCount] = repeatedValue;
        lengthFill->filledCount += 1;
        repeatCount -= 1;
    }
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/* Reads the dynamic Huffman header, builds both tables, then decodes. */
static BOOL Inflate_DynamicBlock(INFLATE_WORK *work)
{
    INFLATE_STATE *state = NULL;
    int literalLengthCount = 0;
    int distanceLengthCount = 0;
    int codeLengthCount = 0;
    int orderIndex = 0;
    int buildResult = 0;
    INFLATE_LENGTH_FILL lengthFill = {0};
    BOOL successResult = FALSE;

    state = &work->state;

    literalLengthCount = Inflate_ReadBits(state, INFLATE_LITERAL_COUNT_BITS);
    distanceLengthCount = Inflate_ReadBits(state, INFLATE_DISTANCE_COUNT_BITS);
    codeLengthCount = Inflate_ReadBits(state, INFLATE_CODE_LENGTH_COUNT_BITS);
    if ((0 > literalLengthCount) || (0 > distanceLengthCount) || (0 > codeLengthCount))
    {
        goto CLEANUP;
    }
    literalLengthCount += INFLATE_LITERAL_COUNT_OFFSET;
    distanceLengthCount += INFLATE_DISTANCE_COUNT_OFFSET;
    codeLengthCount += INFLATE_CODE_LENGTH_COUNT_OFFSET;

    if ((INFLATE_MAX_LIT_CODES < literalLengthCount) || (INFLATE_MAX_DIST_CODES < distanceLengthCount))
    {
        goto CLEANUP;
    }

    /* Read the code-length code lengths in their permuted order. */
    for (orderIndex = 0; orderIndex < codeLengthCount; orderIndex += 1)
    {
        int oneLength = 0;
        oneLength = Inflate_ReadBits(state, INFLATE_CODE_LENGTH_BITS);
        if (0 > oneLength)
        {
            goto CLEANUP;
        }
        work->codeLengths[g_codeLengthOrder[orderIndex]] = (short)oneLength;
    }
    for (orderIndex = codeLengthCount; orderIndex < INFLATE_CODE_LENGTH_SYMBOLS; orderIndex += 1)
    {
        work->codeLengths[g_codeLengthOrder[orderIndex]] = 0;
    }

    buildResult = Inflate_BuildHuffman(&work->lengthCode, work->codeLengths, INFLATE_CODE_LENGTH_SYMBOLS);
    if (0 != buildResult)
    {
        goto CLEANUP;       /* the code-length code must be complete */
    }

    /* Decode the literal/length and distance code lengths themselves. */
    lengthFill.filledCount = 0;
    lengthFill.totalLengthCount = literalLengthCount + distanceLengthCount;
    while (lengthFill.filledCount < lengthFill.totalLengthCount)
    {
        int decodedSymbol = 0;

        decodedSymbol = Inflate_DecodeSymbol(state, &work->lengthCode);
        if (0 > decodedSymbol)
        {
            goto CLEANUP;
        }
        if (FALSE == Inflate_ApplyCodeLengthSymbol(work, decodedSymbol, &lengthFill))
        {
            goto CLEANUP;
        }
    }

    /* The distance code may legitimately be incomplete; a single missing
     * symbol code is tolerated, anything else is corrupt. */
    buildResult = Inflate_BuildHuffman(&work->lengthCode, work->codeLengths, literalLengthCount);
    if (0 != buildResult)
    {
        goto CLEANUP;
    }
    buildResult = Inflate_BuildHuffman(&work->distanceCode, work->codeLengths + literalLengthCount, distanceLengthCount);
    if (0 > buildResult)
    {
        goto CLEANUP;
    }

    successResult = Inflate_DecodeBlock(work);

CLEANUP:
    return successResult;
}

BOOL Inflate_Decompress(INFLATE_REQUEST *inflateRequest)
{
    INFLATE_WORK *work = NULL;
    int finalBlockFlag = 0;
    BOOL successResult = FALSE;

    if ((NULL == inflateRequest) || (NULL == inflateRequest->sourceData) || (NULL == inflateRequest->destinationData))
    {
        goto CLEANUP;
    }

    work = (INFLATE_WORK *)Platform_AllocateZeroed(1, sizeof(INFLATE_WORK));
    if (NULL == work)
    {
        goto CLEANUP;
    }

    work->state.outputBuffer = inflateRequest->destinationData;
    work->state.outputLength = inflateRequest->destinationLength;
    work->state.outputCount = 0UL;
    work->state.inputBuffer = inflateRequest->sourceData;
    work->state.inputLength = inflateRequest->sourceLength;
    work->state.inputCount = 0UL;
    work->state.bitBuffer = 0;
    work->state.bitCount = 0;

    /* Wire the Huffman tables to their backing storage inside the work block. */
    work->lengthCode.symbolCountPerLength = work->lengthCountStorage;
    work->lengthCode.sortedSymbols = work->lengthSymbolStorage;
    work->distanceCode.symbolCountPerLength = work->distanceCountStorage;
    work->distanceCode.sortedSymbols = work->distanceSymbolStorage;

    do
    {
        int blockType = 0;
        BOOL blockResult = FALSE;

        finalBlockFlag = Inflate_ReadBits(&work->state, INFLATE_FINAL_BLOCK_FLAG_BITS);
        blockType = Inflate_ReadBits(&work->state, INFLATE_BLOCK_TYPE_BITS);
        if ((0 > finalBlockFlag) || (0 > blockType))
        {
            goto CLEANUP;
        }

        if (INFLATE_BLOCK_TYPE_STORED == blockType)
        {
            blockResult = Inflate_StoredBlock(work);
        }
        else if (INFLATE_BLOCK_TYPE_FIXED == blockType)
        {
            blockResult = Inflate_FixedBlock(work);
        }
        else if (INFLATE_BLOCK_TYPE_DYNAMIC == blockType)
        {
            blockResult = Inflate_DynamicBlock(work);
        }
        else
        {
            blockResult = FALSE;        /* reserved block type */
        }

        if (FALSE == blockResult)
        {
            goto CLEANUP;
        }
    } while (0 == finalBlockFlag);

    /* The decompressed size must match the central directory exactly. */
    if (work->state.outputCount == inflateRequest->destinationLength)
    {
        successResult = TRUE;
    }

CLEANUP:
    SAFE_FREE(work);
    return successResult;
}
