/*****************************************************************************
 * platform.c
 *
 * No-CRT platform layer implementation. Memory comes from the Win32 process
 * heap and the string/character routines are implemented by hand. Bulk copies
 * go through the OS RtlMoveMemory export. The winnt.h RtlMoveMemory and
 * CopyMemory names are only macros that expand back to memcpy/memmove, so the
 * genuine exported function is declared here after #undef. The only symbol that
 * still must be defined locally is the _fltused floating-point marker, which
 * has no OS export.
 *
 * Why the string/character helpers are hand-rolled instead of calling the
 * documented Win32 equivalents. This layer exists to be no-CRT, bounded, and
 * ASCII-ordinal; for most of these the Win32 routine fails one of those bars:
 *
 *   - Memory (Allocate/Reallocate/Free/CopyMemory) are NOT reinvented: they
 *     wrap HeapAlloc/HeapReAlloc/HeapFree/RtlMoveMemory and add the policy this
 *     project requires (SecureZeroMemory wipe, overflow-checked sizing, and a
 *     destination-size bounds check on the copy).
 *   - LengthA/W: lstrlenA/W is UNBOUNDED; ours caps at a caller-supplied max.
 *   - AppendA: lstrcatA is an unbounded buffer-overflow footgun; ours is bounded.
 *   - CopyA/W: lstrcpynA/W and strsafe's StringCchCopy are merely lateral - the
 *     same bounded copy with no added value, and strsafe re-introduces CRT-ish
 *     helpers a /NODEFAULTLIB build is trying to avoid.
 *   - ComparePrefixIgnoreCaseA: lstrcmpiA / StrCmpNIA case-fold using the user
 *     LOCALE, not a guaranteed ASCII rule (e.g. the Turkish dotless-i), which is
 *     wrong for matching fixed format tokens. CompareStringOrdinal is the right
 *     ordinal primitive but is WIDE-only (used for the wide compares in
 *     archive.c / document.c); there is no narrow equivalent, so the narrow
 *     case-insensitive prefix match is done by hand to stay locale-independent.
 *   - IsDigitA: IsCharAlphaNumericA is locale-aware (would accept non-ASCII
 *     digits); we need strict '0'..'9'. HexValueA has no Win32 equivalent.
 *   - FindCharA / FindLastCharA / FindSubstringA: these DO have exact shlwapi
 *     drop-ins (StrChrA / StrRChrA / StrStrA). They are kept hand-rolled only
 *     for consistency with the rest of this layer - the shlwapi versions are
 *     also plain scans, so switching would be lateral, not an optimisation.
 *****************************************************************************/

#include "platform.h"

/*
 * winnt.h defines RtlMoveMemory as a macro that expands to memmove. Undefine
 * it and declare the real ntdll/kernel32 export so the copy below binds to the
 * OS routine instead of a CRT or hand-rolled symbol.
 */
#undef RtlMoveMemory
__declspec(dllimport) VOID WINAPI RtlMoveMemory(VOID *Destination, const VOID *Source, SIZE_T Length);

/*
 * The linker references _fltused as soon as any floating-point code is used.
 * The CRT normally defines it; without the CRT we must supply it ourselves.
 */
int _fltused = 1;

/*---------------------------------------------------------------------------
 * Heap memory
 *--------------------------------------------------------------------------*/

void *Platform_Allocate(size_t byteCount)
{
    void *allocatedBlock = NULL;

    if (0 == byteCount)
    {
        byteCount = 1;      /* always return a distinct, freeable pointer */
    }
    allocatedBlock = HeapAlloc(GetProcessHeap(), 0, byteCount);
    if (NULL != allocatedBlock)
    {
        /* Zero with SecureZeroMemory rather than HEAP_ZERO_MEMORY so the wipe
         * is guaranteed (never optimised away) and the contract is uniform. */
        (void)SecureZeroMemory(allocatedBlock, byteCount);
    }

    return allocatedBlock;
}

void *Platform_AllocateZeroed(size_t elementCount, size_t elementSize)
{
    void  *allocatedBlock = NULL;
    size_t totalByteCount = 0;

    if ((0 == elementCount) || (0 == elementSize))
    {
        totalByteCount = 1;
    }
    else
    {
        if (elementCount > (MAXSIZE_T / elementSize))
        {
            goto CLEANUP;       /* multiply would overflow */
        }
        totalByteCount = elementCount * elementSize;
    }
    allocatedBlock = HeapAlloc(GetProcessHeap(), 0, totalByteCount);
    if (NULL != allocatedBlock)
    {
        (void)SecureZeroMemory(allocatedBlock, totalByteCount);
    }

CLEANUP:
    return allocatedBlock;
}

void *Platform_Reallocate(void *existingBlock, size_t newByteCount)
{
    void  *resultBlock = NULL;
    SIZE_T previousSize = 0;

    if (0 == newByteCount)
    {
        newByteCount = 1;
    }

    if (NULL == existingBlock)
    {
        resultBlock = HeapAlloc(GetProcessHeap(), 0, newByteCount);
        if (NULL != resultBlock)
        {
            (void)SecureZeroMemory(resultBlock, newByteCount);
        }
        goto CLEANUP;
    }

    previousSize = HeapSize(GetProcessHeap(), 0, existingBlock);
    resultBlock = HeapReAlloc(GetProcessHeap(), 0, existingBlock, newByteCount);
    if ((NULL != resultBlock) && ((SIZE_T)-1 != previousSize) && (newByteCount > previousSize))
    {
        /* Zero only the freshly grown tail; the preserved contents are kept. */
        (void)SecureZeroMemory((unsigned char *)resultBlock + previousSize,
                               newByteCount - previousSize);
    }

CLEANUP:
    return resultBlock;
}

void Platform_Free(void *blockToFree)
{
    SIZE_T blockSize = 0;

    if (NULL == blockToFree)
    {
        goto CLEANUP;
    }

    /* Scrub the whole block with SecureZeroMemory before handing it back to
     * the heap, so freed memory never retains its previous contents. */
    blockSize = HeapSize(GetProcessHeap(), 0, blockToFree);
    if ((SIZE_T)-1 != blockSize)
    {
        (void)SecureZeroMemory(blockToFree, blockSize);
    }
    (void)HeapFree(GetProcessHeap(), 0, blockToFree);

CLEANUP:
    return;
}

BOOL Platform_CopyMemory(const MEMORY_COPY_REQUEST *copyRequest)
{
    BOOL successResult = FALSE;

    if ((NULL == copyRequest) || (NULL == copyRequest->destination) || (NULL == copyRequest->source))
    {
        goto CLEANUP;
    }
    if (copyRequest->byteCount > copyRequest->destinationSize)
    {
        goto CLEANUP;       /* would overrun the destination */
    }

    RtlMoveMemory(copyRequest->destination, copyRequest->source, copyRequest->byteCount);
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/*---------------------------------------------------------------------------
 * Narrow strings
 *--------------------------------------------------------------------------*/

size_t Platform_LengthA(const char *text, size_t maximumLength)
{
    size_t measuredLength = 0;

    if (NULL == text)
    {
        goto CLEANUP;
    }
    while ((measuredLength < maximumLength) && ('\0' != text[measuredLength]))
    {
        measuredLength += 1;
    }

CLEANUP:
    return measuredLength;
}

BOOL Platform_CopyA(char *destination, size_t destinationCount, const char *source)
{
    size_t copyIndex = 0;
    BOOL   successResult = FALSE;

    if ((NULL == destination) || (NULL == source) || (0 == destinationCount))
    {
        goto CLEANUP;
    }

    while (('\0' != source[copyIndex]) && (copyIndex < (destinationCount - 1)))
    {
        destination[copyIndex] = source[copyIndex];
        copyIndex += 1;
    }
    if ('\0' != source[copyIndex])
    {
        destination[0] = '\0';
        goto CLEANUP;       /* source did not fit */
    }
    destination[copyIndex] = '\0';
    successResult = TRUE;

CLEANUP:
    return successResult;
}

BOOL Platform_AppendA(char *destination, size_t destinationCount, const char *source)
{
    size_t existingLength = 0;
    BOOL   successResult = FALSE;

    if ((NULL == destination) || (NULL == source) || (0 == destinationCount))
    {
        goto CLEANUP;
    }

    existingLength = Platform_LengthA(destination, destinationCount);
    if (existingLength >= destinationCount)
    {
        goto CLEANUP;       /* destination is not terminated within its size */
    }
    successResult = Platform_CopyA(destination + existingLength, destinationCount - existingLength, source);

CLEANUP:
    return successResult;
}

char *Platform_FindSubstringA(const char *haystack, const char *needle)
{
    const char *foundPosition = NULL;
    const char *scanCursor = NULL;

    if ((NULL == haystack) || (NULL == needle))
    {
        goto CLEANUP;
    }
    if ('\0' == needle[0])
    {
        foundPosition = haystack;       /* empty needle matches at the start */
        goto CLEANUP;
    }

    for (scanCursor = haystack; '\0' != *scanCursor; scanCursor += 1)
    {
        size_t matchIndex = 0;
        while (('\0' != needle[matchIndex]) && (scanCursor[matchIndex] == needle[matchIndex]))
        {
            matchIndex += 1;
        }
        if ('\0' == needle[matchIndex])
        {
            foundPosition = scanCursor;
            goto CLEANUP;
        }
    }

CLEANUP:
    return (char *)foundPosition;
}

char *Platform_FindCharA(const char *text, char target)
{
    const char *foundPosition = NULL;
    const char *scanCursor = NULL;

    if (NULL == text)
    {
        goto CLEANUP;
    }
    for (scanCursor = text; '\0' != *scanCursor; scanCursor += 1)
    {
        if (target == *scanCursor)
        {
            foundPosition = scanCursor;
            goto CLEANUP;
        }
    }

CLEANUP:
    return (char *)foundPosition;
}

char *Platform_FindLastCharA(const char *text, char target)
{
    const char *foundPosition = NULL;
    const char *scanCursor = NULL;

    if (NULL == text)
    {
        goto CLEANUP;
    }
    for (scanCursor = text; '\0' != *scanCursor; scanCursor += 1)
    {
        if (target == *scanCursor)
        {
            foundPosition = scanCursor;
        }
    }

CLEANUP:
    return (char *)foundPosition;
}

int Platform_ComparePrefixIgnoreCaseA(const char *left, const char *right, size_t count)
{
    size_t compareIndex = 0;
    int    comparisonResult = 0;

    if ((NULL == left) || (NULL == right))
    {
        comparisonResult = (left == right) ? 0 : 1;
        goto CLEANUP;
    }

    for (compareIndex = 0; compareIndex < count; compareIndex += 1)
    {
        char leftChar = left[compareIndex];
        char rightChar = right[compareIndex];

        if (('A' <= leftChar) && ('Z' >= leftChar))
        {
            leftChar = (char)(leftChar + ('a' - 'A'));
        }
        if (('A' <= rightChar) && ('Z' >= rightChar))
        {
            rightChar = (char)(rightChar + ('a' - 'A'));
        }
        if (leftChar != rightChar)
        {
            comparisonResult = (int)(unsigned char)leftChar - (int)(unsigned char)rightChar;
            goto CLEANUP;
        }
        if ('\0' == leftChar)
        {
            goto CLEANUP;       /* both reached the terminator */
        }
    }

CLEANUP:
    return comparisonResult;
}

BOOL Platform_IsDigitA(char character)
{
    return (('0' <= character) && ('9' >= character)) ? TRUE : FALSE;
}

int Platform_HexValueA(char character)
{
    int hexValue = 0;

    if (FALSE != Platform_IsDigitA(character))
    {
        hexValue = character - '0';
    }
    else
    {
        char loweredCharacter = character;
        if (('A' <= loweredCharacter) && ('Z' >= loweredCharacter))
        {
            loweredCharacter = (char)(loweredCharacter + ('a' - 'A'));
        }
        if (('a' <= loweredCharacter) && ('f' >= loweredCharacter))
        {
            hexValue = (loweredCharacter - 'a') + 10;
        }
    }

    return hexValue;
}

/*---------------------------------------------------------------------------
 * Wide strings
 *--------------------------------------------------------------------------*/

size_t Platform_LengthW(const wchar_t *text, size_t maximumLength)
{
    size_t measuredLength = 0;

    if (NULL == text)
    {
        goto CLEANUP;
    }
    while ((measuredLength < maximumLength) && (L'\0' != text[measuredLength]))
    {
        measuredLength += 1;
    }

CLEANUP:
    return measuredLength;
}

BOOL Platform_CopyW(wchar_t *destination, size_t destinationCount, const wchar_t *source)
{
    size_t copyIndex = 0;
    BOOL   successResult = FALSE;

    if ((NULL == destination) || (NULL == source) || (0 == destinationCount))
    {
        goto CLEANUP;
    }

    while ((L'\0' != source[copyIndex]) && (copyIndex < (destinationCount - 1)))
    {
        destination[copyIndex] = source[copyIndex];
        copyIndex += 1;
    }
    if (L'\0' != source[copyIndex])
    {
        destination[0] = L'\0';
        goto CLEANUP;       /* source did not fit */
    }
    destination[copyIndex] = L'\0';
    successResult = TRUE;

CLEANUP:
    return successResult;
}
