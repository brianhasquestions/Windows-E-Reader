/*****************************************************************************
 * document.c
 *
 * Builds the document model on top of the ZIP archive reader.
 *
 *  - CBZ: every image member becomes a page; the members are sorted by name
 *    so the reading order matches the comic's intended order.
 *  - EPUB: META-INF/container.xml points at the OPF package; the OPF spine
 *    gives the chapter reading order; each chapter's XHTML is reduced to
 *    reflowable plain text and concatenated into one wide-character stream,
 *    with chapter boundaries recorded for navigation.
 *
 * The XHTML-to-text reduction is deliberately lightweight (no CSS, no inline
 * images): it strips tags, decodes the common entities, inserts breaks at
 * block-level elements, and collapses runs of whitespace.
 *
 * The builder/context struct definitions live in document.h.
 *****************************************************************************/

#include "document.h"

/* Initial capacities and growth policy for the dynamic buffers. */
#define DOCUMENT_BYTE_BUILDER_INITIAL_CAPACITY  4096
#define DOCUMENT_WIDE_BUILDER_INITIAL_CAPACITY  8192
#define DOCUMENT_CHAPTER_INITIAL_CAPACITY       16
#define DOCUMENT_MANIFEST_CAPACITY              64
#define DOCUMENT_GROWTH_FACTOR                  2

/* Fixed-size scratch buffer sizes. */
#define DOCUMENT_TAG_NAME_CAPACITY              32
#define DOCUMENT_CHAPTER_TITLE_CAPACITY         64

/* Shell-sort gap is halved each pass. */
#define DOCUMENT_SHELL_GAP_DIVISOR              2

/* Parsing limits and numeric bases. */
#define DOCUMENT_MAX_ENTITY_LENGTH              10
#define DOCUMENT_ASCII_LIMIT                    128
#define DOCUMENT_DECIMAL_BASE                   10
#define DOCUMENT_HEX_BASE                       16
#define DOCUMENT_NIBBLE_BITS                    4
#define DOCUMENT_PERCENT_ESCAPE_LENGTH          3    /* '%' plus two hex digits */
#define DOCUMENT_NUL_TERMINATOR_SIZE            1U
#define DOCUMENT_PATH_JOIN_EXTRA                2U   /* room for a '/' and a NUL */

/* Tag-name lengths used while scanning the OPF and container documents. */
#define DOCUMENT_ITEM_TAG_LENGTH                5    /* strlen("<item")     */
#define DOCUMENT_ITEMREF_TAG_LENGTH             8    /* strlen("<itemref")  */
#define DOCUMENT_ROOTFILE_TAG_LENGTH            9    /* strlen("<rootfile") */

/*
 * Upper bounds for the bounded length helpers (Platform_LengthA /
 * Platform_LengthW). They never read past these limits and return 0 for a
 * NULL pointer, so a malformed or NULL string from the archive cannot run a
 * length scan off the end of a buffer or crash on a NULL dereference.
 */
#define DOCUMENT_MAX_NAME_LENGTH                65536U       /* names, paths, tags */
#define DOCUMENT_MAX_CONTENT_LENGTH             0x7FFFFFFFU  /* document bodies     */

/* Recognised image extensions for CBZ page detection (lower case). */
static const char *g_imageExtensions[] =
{
    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp"
};

/* The blank line inserted between consecutive EPUB chapters. */
static const wchar_t g_chapterSeparator[] = L"\n\n";

/* Named XHTML entities this reader decodes (checked in order). */
static const DOCUMENT_NAMED_ENTITY g_namedEntities[] =
{
    { "amp",  '&'  },
    { "lt",   '<'  },
    { "gt",   '>'  },
    { "quot", '"'  },
    { "apos", '\'' },
    { "nbsp", ' '  }
};

/*---------------------------------------------------------------------------
 * Small dynamic-buffer helpers
 *--------------------------------------------------------------------------*/

/* Appends one byte to a BYTE_BUILDER, growing as needed. Returns TRUE on success. */
static BOOL Document_AppendByte(BYTE_BUILDER *builder, char byteToAppend)
{
    char  *grownData = NULL;
    size_t newCapacity = 0;
    BOOL   successResult = FALSE;

    if (builder->length >= builder->capacity)
    {
        newCapacity = (0 == builder->capacity) ? DOCUMENT_BYTE_BUILDER_INITIAL_CAPACITY : (builder->capacity * DOCUMENT_GROWTH_FACTOR);
        grownData = (char *)Platform_Reallocate(builder->data, newCapacity);
        if (NULL == grownData)
        {
            goto CLEANUP;
        }
        builder->data = grownData;
        builder->capacity = newCapacity;
    }

    builder->data[builder->length] = byteToAppend;
    builder->length += 1;
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/* Appends wide characters to a WIDE_BUILDER, growing as needed. Returns TRUE on success. */
static BOOL Document_AppendWide(WIDE_BUILDER *builder, const wchar_t *text, size_t characterCount)
{
    wchar_t            *grownData = NULL;
    size_t              requiredCapacity = 0;
    size_t              newCapacity = 0;
    MEMORY_COPY_REQUEST wideCopy = {0};
    BOOL                successResult = FALSE;

    if (0 == characterCount)
    {
        successResult = TRUE;
        goto CLEANUP;
    }

    requiredCapacity = builder->length + characterCount + DOCUMENT_NUL_TERMINATOR_SIZE;
    /* Reject a size_t wrap so a crafted document can never shrink the buffer. */
    if (requiredCapacity <= builder->length)
    {
        goto CLEANUP;
    }
    if (requiredCapacity > builder->capacity)
    {
        newCapacity = (0 == builder->capacity) ? DOCUMENT_WIDE_BUILDER_INITIAL_CAPACITY : builder->capacity;
        /* One geometric step (guarded against wrap), then jump straight to the
         * required size when a single append is larger than that step. */
        if (newCapacity <= (MAXSIZE_T / DOCUMENT_GROWTH_FACTOR))
        {
            newCapacity *= DOCUMENT_GROWTH_FACTOR;
        }
        if (newCapacity < requiredCapacity)
        {
            newCapacity = requiredCapacity;
        }
        if (newCapacity > (MAXSIZE_T / sizeof(wchar_t)))
        {
            goto CLEANUP;           /* byte size would overflow */
        }
        grownData = (wchar_t *)Platform_Reallocate(builder->data, newCapacity * sizeof(wchar_t));
        if (NULL == grownData)
        {
            goto CLEANUP;
        }
        builder->data = grownData;
        builder->capacity = newCapacity;
    }

    wideCopy.destination = builder->data + builder->length;
    wideCopy.destinationSize = (builder->capacity - builder->length) * sizeof(wchar_t);
    wideCopy.source = text;
    wideCopy.byteCount = characterCount * sizeof(wchar_t);
    (void)Platform_CopyMemory(&wideCopy);

    builder->length += characterCount;
    builder->data[builder->length] = L'\0';
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/* Duplicates a NUL-terminated UTF-8 string onto the heap. Returns NULL on failure. */
static char *Document_DuplicateUtf8(const char *sourceString)
{
    char               *duplicateString = NULL;
    size_t              lengthWithNul = 0;
    MEMORY_COPY_REQUEST duplicateCopy = {0};

    if (NULL == sourceString)
    {
        goto CLEANUP;
    }

    lengthWithNul = Platform_LengthA(sourceString, DOCUMENT_MAX_NAME_LENGTH) + DOCUMENT_NUL_TERMINATOR_SIZE;
    duplicateString = (char *)Platform_Allocate(lengthWithNul);
    if (NULL == duplicateString)
    {
        goto CLEANUP;
    }

    duplicateCopy.destination = duplicateString;
    duplicateCopy.destinationSize = lengthWithNul;
    duplicateCopy.source = sourceString;
    duplicateCopy.byteCount = lengthWithNul;
    (void)Platform_CopyMemory(&duplicateCopy);

CLEANUP:
    return duplicateString;
}

/* Copies an extracted (length-bounded) member into a NUL-terminated string. */
static char *Document_BufferToString(ARCHIVE_BUFFER *sourceBuffer)
{
    char               *resultString = NULL;
    MEMORY_COPY_REQUEST stringCopy = {0};

    resultString = (char *)Platform_Allocate((size_t)sourceBuffer->size + DOCUMENT_NUL_TERMINATOR_SIZE);
    if (NULL == resultString)
    {
        goto CLEANUP;
    }
    if (0UL != sourceBuffer->size)
    {
        stringCopy.destination = resultString;
        stringCopy.destinationSize = (size_t)sourceBuffer->size + DOCUMENT_NUL_TERMINATOR_SIZE;
        stringCopy.source = sourceBuffer->data;
        stringCopy.byteCount = (size_t)sourceBuffer->size;
        (void)Platform_CopyMemory(&stringCopy);
    }
    resultString[sourceBuffer->size] = '\0';

CLEANUP:
    return resultString;
}

/*---------------------------------------------------------------------------
 * Win32-native string comparison
 *
 * Microsoft recommends CompareStringOrdinal over the CRT strcmp/_stricmp
 * family for secure, locale-independent comparison. It operates on wide
 * strings, so the UTF-8 inputs are widened first. Document_OrdinalCompare
 * returns the raw CSTR_LESS_THAN/EQUAL/GREATER_THAN result (or 0 on failure,
 * which the equality wrappers treat as "not equal").
 *--------------------------------------------------------------------------*/

static int Document_OrdinalCompare(const char *leftString, const char *rightString, BOOL ignoreCase)
{
    wchar_t *leftWide = NULL;
    wchar_t *rightWide = NULL;
    int leftWideLength = 0;
    int rightWideLength = 0;
    int comparisonResult = 0;

    if ((NULL == leftString) || (NULL == rightString))
    {
        goto CLEANUP;
    }

    leftWideLength = MultiByteToWideChar(CP_UTF8, 0, leftString, -1, NULL, 0);
    rightWideLength = MultiByteToWideChar(CP_UTF8, 0, rightString, -1, NULL, 0);
    if ((0 >= leftWideLength) || (0 >= rightWideLength))
    {
        goto CLEANUP;
    }

    leftWide = (wchar_t *)Platform_AllocateZeroed((size_t)leftWideLength, sizeof(wchar_t));
    rightWide = (wchar_t *)Platform_AllocateZeroed((size_t)rightWideLength, sizeof(wchar_t));
    if ((NULL == leftWide) || (NULL == rightWide))
    {
        goto CLEANUP;
    }
    (void)MultiByteToWideChar(CP_UTF8, 0, leftString, -1, leftWide, leftWideLength);
    (void)MultiByteToWideChar(CP_UTF8, 0, rightString, -1, rightWide, rightWideLength);

    comparisonResult = CompareStringOrdinal(leftWide, -1, rightWide, -1, ignoreCase);

CLEANUP:
    SAFE_FREE(rightWide);
    SAFE_FREE(leftWide);
    return comparisonResult;
}

/* Case-sensitive ordinal equality (replaces strcmp == 0). */
static BOOL Document_OrdinalEqual(const char *leftString, const char *rightString)
{
    return (CSTR_EQUAL == Document_OrdinalCompare(leftString, rightString, FALSE)) ? TRUE : FALSE;
}

/* Case-insensitive ordinal equality (replaces _stricmp == 0). */
static BOOL Document_OrdinalEqualIgnoreCase(const char *leftString, const char *rightString)
{
    return (CSTR_EQUAL == Document_OrdinalCompare(leftString, rightString, TRUE)) ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * CBZ support
 *--------------------------------------------------------------------------*/

/* Case-insensitive test that memberName ends with one of the image extensions. */
static BOOL Document_IsImageMember(const char *memberName)
{
    size_t nameLength = 0;
    size_t extensionIndex = 0;
    BOOL   isImageResult = FALSE;

    nameLength = Platform_LengthA(memberName, DOCUMENT_MAX_NAME_LENGTH);

    /* Skip directory markers and resource-fork noise from some archivers. */
    if ((0 == nameLength) || ('/' == memberName[nameLength - 1]))
    {
        goto CLEANUP;
    }
    if (NULL != Platform_FindSubstringA(memberName, "__MACOSX"))
    {
        goto CLEANUP;
    }

    for (extensionIndex = 0; extensionIndex < EREADER_ARRAY_LENGTH(g_imageExtensions); extensionIndex += 1)
    {
        size_t extensionLength = 0;
        extensionLength = Platform_LengthA(g_imageExtensions[extensionIndex], DOCUMENT_MAX_NAME_LENGTH);
        if (nameLength < extensionLength)
        {
            continue;       /* member name is too short to carry this extension */
        }
        if (FALSE != Document_OrdinalEqualIgnoreCase(memberName + (nameLength - extensionLength), g_imageExtensions[extensionIndex]))
        {
            isImageResult = TRUE;
            goto CLEANUP;
        }
    }

CLEANUP:
    return isImageResult;
}

/* Widens a NUL-terminated UTF-8 string to a freshly allocated UTF-16 string. */
static wchar_t *Document_WidenUtf8(const char *utf8Text)
{
    wchar_t *wideText = NULL;
    int      wideLength = 0;

    if (NULL == utf8Text)
    {
        goto CLEANUP;
    }
    wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8Text, -1, NULL, 0);
    if (0 >= wideLength)
    {
        goto CLEANUP;
    }
    wideText = (wchar_t *)Platform_AllocateZeroed((size_t)wideLength, sizeof(wchar_t));
    if (NULL == wideText)
    {
        goto CLEANUP;
    }
    (void)MultiByteToWideChar(CP_UTF8, 0, utf8Text, -1, wideText, wideLength);

CLEANUP:
    return wideText;
}

/*
 * Compares two sort items, case-insensitive ordinal. Uses the pre-widened
 * keys when both are present (the common, allocation-free path) and falls
 * back to the allocating compare only for a name that failed to widen.
 * Returns the raw CSTR_LESS_THAN / EQUAL / GREATER_THAN result.
 */
static int Document_CompareSortItems(const IMAGE_SORT_ITEM *leftItem, const IMAGE_SORT_ITEM *rightItem)
{
    int comparisonResult = 0;

    if ((NULL != leftItem->wideKey) && (NULL != rightItem->wideKey))
    {
        comparisonResult = CompareStringOrdinal(leftItem->wideKey, -1, rightItem->wideKey, -1, TRUE);
    }
    else
    {
        comparisonResult = Document_OrdinalCompare(leftItem->originalName, rightItem->originalName, TRUE);
    }

    return comparisonResult;
}

/* Performs the gapped insertion of one element, the inner step of the sort. */
static void Document_GappedInsert(IMAGE_SORT_ITEM *sortItems, unsigned long gap, unsigned long scanIndex)
{
    IMAGE_SORT_ITEM liftedItem = {0};
    unsigned long   compareIndex = 0UL;

    liftedItem = sortItems[scanIndex];
    compareIndex = scanIndex;
    while ((compareIndex >= gap) && (CSTR_GREATER_THAN == Document_CompareSortItems(&sortItems[compareIndex - gap], &liftedItem)))
    {
        sortItems[compareIndex] = sortItems[compareIndex - gap];
        compareIndex -= gap;
    }
    sortItems[compareIndex] = liftedItem;
}

/*
 * Sorts the image-name array into case-insensitive page order. Each name is
 * widened to UTF-16 once up front (decorate), the cached keys are Shell-sorted
 * with the Win32 ordinal comparison, and the reordered names are written back
 * (undecorate) -- so comparisons never re-convert or re-allocate.
 */
static void Document_SortImageNames(char **imageNames, unsigned long imageCount)
{
    IMAGE_SORT_ITEM *sortItems = NULL;
    unsigned long    itemIndex = 0UL;
    unsigned long    gap = 0UL;
    unsigned long    scanIndex = 0UL;

    if (DOCUMENT_SHELL_GAP_DIVISOR > imageCount)
    {
        goto CLEANUP;       /* 0 or 1 entries are already in order */
    }

    sortItems = (IMAGE_SORT_ITEM *)Platform_AllocateZeroed((size_t)imageCount, sizeof(IMAGE_SORT_ITEM));
    if (NULL == sortItems)
    {
        goto CLEANUP;       /* out of memory: leave the order untouched */
    }

    /* Decorate: widen each name once (a failed widen leaves wideKey NULL). */
    for (itemIndex = 0UL; itemIndex < imageCount; itemIndex += 1UL)
    {
        sortItems[itemIndex].originalName = imageNames[itemIndex];
        sortItems[itemIndex].wideKey = Document_WidenUtf8(imageNames[itemIndex]);
    }

    /* Shell sort over the cached keys; the inner pass lives in the helper. */
    for (gap = imageCount / DOCUMENT_SHELL_GAP_DIVISOR; gap > 0UL; gap /= DOCUMENT_SHELL_GAP_DIVISOR)
    {
        for (scanIndex = gap; scanIndex < imageCount; scanIndex += 1UL)
        {
            Document_GappedInsert(sortItems, gap, scanIndex);
        }
    }

    /* Undecorate: write the reordered names back to the caller's array. */
    for (itemIndex = 0UL; itemIndex < imageCount; itemIndex += 1UL)
    {
        imageNames[itemIndex] = sortItems[itemIndex].originalName;
    }

CLEANUP:
    if (NULL != sortItems)
    {
        for (itemIndex = 0UL; itemIndex < imageCount; itemIndex += 1UL)
        {
            SAFE_FREE(sortItems[itemIndex].wideKey);
        }
        SAFE_FREE(sortItems);
    }
    return;
}

/* Collects and sorts the image members of a CBZ archive. Returns TRUE on success. */
static BOOL Document_BuildCbz(DOCUMENT *document)
{
    char        **imageNames = NULL;
    unsigned long collectedCount = 0UL;
    unsigned long entryIndex = 0UL;
    BOOL          successResult = FALSE;

    imageNames = (char **)Platform_AllocateZeroed((size_t)document->backingArchive->entryCount, sizeof(char *));
    if (NULL == imageNames)
    {
        goto CLEANUP;
    }

    for (entryIndex = 0UL; entryIndex < document->backingArchive->entryCount; entryIndex += 1UL)
    {
        const char *candidateName = NULL;
        candidateName = document->backingArchive->entries[entryIndex].entryName;
        if (FALSE != Document_IsImageMember(candidateName))
        {
            imageNames[collectedCount] = Document_DuplicateUtf8(candidateName);
            if (NULL == imageNames[collectedCount])
            {
                goto CLEANUP;
            }
            collectedCount += 1UL;
        }
    }

    if (0UL == collectedCount)
    {
        goto CLEANUP;       /* not a usable comic archive */
    }

    Document_SortImageNames(imageNames, collectedCount);

    document->documentType = DOCUMENT_TYPE_CBZ;
    document->imageEntryNames = imageNames;
    document->imageCount = collectedCount;
    imageNames = NULL;      /* ownership transferred to the document */
    successResult = TRUE;

CLEANUP:
    if (NULL != imageNames)
    {
        for (entryIndex = 0UL; entryIndex < collectedCount; entryIndex += 1UL)
        {
            SAFE_FREE(imageNames[entryIndex]);
        }
        SAFE_FREE(imageNames);
    }
    return successResult;
}

/*---------------------------------------------------------------------------
 * EPUB support
 *--------------------------------------------------------------------------*/

/* Returns TRUE when ch is XML whitespace. */
static BOOL Document_IsXmlWhitespace(char characterToTest)
{
    return ((' ' == characterToTest) || ('\t' == characterToTest) || ('\r' == characterToTest) || ('\n' == characterToTest)) ? TRUE : FALSE;
}

/*
 * Extracts the value of attributeName from an element beginning at
 * elementStart (up to the next '>'). Returns a freshly allocated string or
 * NULL when the attribute is absent.
 */
static char *Document_ExtractAttribute(const char *elementStart, const char *attributeName)
{
    char               *resultValue = NULL;
    const char         *elementEnd = NULL;
    const char         *cursor = NULL;
    size_t              attributeNameLength = 0;
    MEMORY_COPY_REQUEST valueCopy = {0};

    elementEnd = Platform_FindCharA(elementStart, '>');
    if (NULL == elementEnd)
    {
        goto CLEANUP;
    }
    attributeNameLength = Platform_LengthA(attributeName, DOCUMENT_MAX_NAME_LENGTH);

    for (cursor = elementStart; cursor < elementEnd; cursor += 1)
    {
        const char *valueStart = NULL;
        const char *valueEnd = NULL;
        char        quoteCharacter = '\0';
        size_t      valueLength = 0;

        if (0 != Platform_ComparePrefixIgnoreCaseA(cursor, attributeName, attributeNameLength))
        {
            continue;
        }
        /* The character before the name must be a boundary, not part of a longer
         * name. The && short-circuits, so *(cursor - 1) is only read when there
         * is a preceding character. */
        if ((cursor > elementStart) && (FALSE == Document_IsXmlWhitespace(*(cursor - 1))))
        {
            continue;
        }

        valueStart = cursor + attributeNameLength;
        while ((valueStart < elementEnd) && (FALSE != Document_IsXmlWhitespace(*valueStart)))
        {
            valueStart += 1;
        }
        if ((valueStart >= elementEnd) || ('=' != *valueStart))
        {
            continue;
        }
        valueStart += 1;
        while ((valueStart < elementEnd) && (FALSE != Document_IsXmlWhitespace(*valueStart)))
        {
            valueStart += 1;
        }
        if (valueStart >= elementEnd)
        {
            continue;
        }
        quoteCharacter = *valueStart;
        if (('"' != quoteCharacter) && ('\'' != quoteCharacter))
        {
            continue;
        }
        valueStart += 1;
        valueEnd = valueStart;
        while ((valueEnd < elementEnd) && (quoteCharacter != *valueEnd))
        {
            valueEnd += 1;
        }
        if (valueEnd >= elementEnd)
        {
            continue;
        }

        valueLength = (size_t)(valueEnd - valueStart);
        resultValue = (char *)Platform_Allocate(valueLength + DOCUMENT_NUL_TERMINATOR_SIZE);
        if (NULL == resultValue)
        {
            goto CLEANUP;
        }
        if (0 != valueLength)
        {
            valueCopy.destination = resultValue;
            valueCopy.destinationSize = valueLength + DOCUMENT_NUL_TERMINATOR_SIZE;
            valueCopy.source = valueStart;
            valueCopy.byteCount = valueLength;
            (void)Platform_CopyMemory(&valueCopy);
        }
        resultValue[valueLength] = '\0';
        goto CLEANUP;
    }

CLEANUP:
    return resultValue;
}

/* Decodes percent-escapes in an href in place (e.g. %20 -> space). */
static void Document_PercentDecode(char *textToDecode)
{
    size_t readIndex = 0;
    size_t writeIndex = 0;

    while ('\0' != textToDecode[readIndex])
    {
        if (('%' == textToDecode[readIndex]) && ('\0' != textToDecode[readIndex + 1]) && ('\0' != textToDecode[readIndex + 2]))
        {
            int highNibble = 0;
            int lowNibble = 0;
            highNibble = Platform_HexValueA(textToDecode[readIndex + 1]);
            lowNibble = Platform_HexValueA(textToDecode[readIndex + 2]);

            textToDecode[writeIndex] = (char)((highNibble << DOCUMENT_NIBBLE_BITS) | lowNibble);
            writeIndex += 1;
            readIndex += DOCUMENT_PERCENT_ESCAPE_LENGTH;
        }
        else
        {
            textToDecode[writeIndex] = textToDecode[readIndex];
            writeIndex += 1;
            readIndex += 1;
        }
    }
    textToDecode[writeIndex] = '\0';
}

/*
 * Applies one path segment to the normalised segment stack: an empty segment
 * (from "//") or "." is ignored, ".." pops the previous segment, and anything
 * else is pushed. segmentStack must have room for one more entry.
 */
static void Document_PushPathSegment(char **segmentStack, size_t *segmentCount, char *segment)
{
    if ('\0' == segment[0])
    {
        goto CLEANUP;       /* empty segment (e.g. from "//" or a leading '/') */
    }
    if (('.' == segment[0]) && ('\0' == segment[1]))
    {
        goto CLEANUP;       /* "." current directory: ignore */
    }
    if (('.' == segment[0]) && ('.' == segment[1]) && ('\0' == segment[2]))
    {
        if (0 != *segmentCount)
        {
            *segmentCount -= 1;     /* ".." parent directory: pop */
        }
        goto CLEANUP;
    }

    segmentStack[*segmentCount] = segment;
    *segmentCount += 1;

CLEANUP:
    return;
}

/*
 * Joins the OPF directory with a relative href and normalises "." and ".."
 * segments, producing a ZIP member path. Returns a freshly allocated string.
 */
static char *Document_ResolvePath(const char *baseDirectory, const char *relativePath)
{
    char  *combinedPath = NULL;
    char  *normalizedPath = NULL;
    char **segmentStack = NULL;
    char  *segmentCursor = NULL;
    char  *segmentStart = NULL;
    size_t combinedLength = 0;
    size_t segmentCount = 0;
    size_t segmentIndex = 0;

    combinedLength = Platform_LengthA(baseDirectory, DOCUMENT_MAX_NAME_LENGTH) + Platform_LengthA(relativePath, DOCUMENT_MAX_NAME_LENGTH) + DOCUMENT_PATH_JOIN_EXTRA;
    combinedPath = (char *)Platform_Allocate(combinedLength);
    if (NULL == combinedPath)
    {
        goto CLEANUP;
    }
    combinedPath[0] = '\0';
    (void)Platform_CopyA(combinedPath, combinedLength, baseDirectory);
    (void)Platform_AppendA(combinedPath, combinedLength, relativePath);

    /* A path can contain at most one segment per character. */
    segmentStack = (char **)Platform_AllocateZeroed(combinedLength, sizeof(char *));
    if (NULL == segmentStack)
    {
        goto CLEANUP;
    }

    /* Split on '/' in place, classifying each segment as a boundary is reached. */
    segmentStart = combinedPath;
    for (segmentCursor = combinedPath; ; segmentCursor += 1)
    {
        BOOL reachedEnd = FALSE;

        if (('/' != *segmentCursor) && ('\0' != *segmentCursor))
        {
            continue;       /* still inside the current segment */
        }

        reachedEnd = ('\0' == *segmentCursor) ? TRUE : FALSE;
        *segmentCursor = '\0';
        Document_PushPathSegment(segmentStack, &segmentCount, segmentStart);

        if (FALSE != reachedEnd)
        {
            break;
        }
        segmentStart = segmentCursor + 1;
    }

    normalizedPath = (char *)Platform_Allocate(combinedLength);
    if (NULL == normalizedPath)
    {
        goto CLEANUP;
    }
    normalizedPath[0] = '\0';
    for (segmentIndex = 0; segmentIndex < segmentCount; segmentIndex += 1)
    {
        if (0 != segmentIndex)
        {
            (void)Platform_AppendA(normalizedPath, combinedLength, "/");
        }
        (void)Platform_AppendA(normalizedPath, combinedLength, segmentStack[segmentIndex]);
    }

CLEANUP:
    SAFE_FREE(segmentStack);
    SAFE_FREE(combinedPath);
    return normalizedPath;
}

/*
 * Decodes a numeric XHTML entity body ("#NNN" decimal or "#xHH" hex) to a
 * single byte. entityBody[0] is the leading '#'. Code points outside ASCII
 * collapse to a space (the renderer is ASCII-only).
 */
static char Document_DecodeNumericEntity(const char *entityBody, size_t entityLength)
{
    long   codePoint = 0;
    size_t digitIndex = 0;
    char   decodedCharacter = ' ';

    if ((entityLength > 1) && (('x' == entityBody[1]) || ('X' == entityBody[1])))
    {
        for (digitIndex = 2; digitIndex < entityLength; digitIndex += 1)
        {
            codePoint = (codePoint * DOCUMENT_HEX_BASE) + Platform_HexValueA(entityBody[digitIndex]);
        }
    }
    else
    {
        for (digitIndex = 1; digitIndex < entityLength; digitIndex += 1)
        {
            codePoint = (codePoint * DOCUMENT_DECIMAL_BASE) + (entityBody[digitIndex] - '0');
        }
    }

    if (codePoint < DOCUMENT_ASCII_LIMIT)
    {
        decodedCharacter = (char)codePoint;
    }

    goto CLEANUP;

CLEANUP:
    return decodedCharacter;
}

/* Decodes a numeric or named XHTML entity into a single byte where possible. */
static char Document_DecodeEntity(const char *entityBody, size_t entityLength)
{
    char   decodedCharacter = ' ';
    size_t entityIndex = 0;

    if (0 == entityLength)
    {
        goto CLEANUP;
    }

    if ('#' == entityBody[0])
    {
        decodedCharacter = Document_DecodeNumericEntity(entityBody, entityLength);
        goto CLEANUP;
    }

    /* Named entity: first matching table row wins. */
    for (entityIndex = 0; entityIndex < EREADER_ARRAY_LENGTH(g_namedEntities); entityIndex += 1)
    {
        if (0 == Platform_ComparePrefixIgnoreCaseA(entityBody, g_namedEntities[entityIndex].name, entityLength))
        {
            decodedCharacter = g_namedEntities[entityIndex].character;
            goto CLEANUP;
        }
    }

CLEANUP:
    return decodedCharacter;
}

/* Returns TRUE when tagName (lower-cased prefix) is a block-level element. */
static BOOL Document_IsBlockTag(const char *tagName)
{
    static const char *blockTags[] =
    {
        "p", "div", "br", "li", "tr", "hr", "h1", "h2", "h3", "h4",
        "h5", "h6", "blockquote", "section", "article", "header",
        "footer", "ul", "ol", "table", "figure"
    };
    size_t blockIndex = 0;
    size_t tagNameLength = 0;
    BOOL   isBlockResult = FALSE;

    tagNameLength = Platform_LengthA(tagName, DOCUMENT_MAX_NAME_LENGTH);
    for (blockIndex = 0; blockIndex < EREADER_ARRAY_LENGTH(blockTags); blockIndex += 1)
    {
        if ((tagNameLength == Platform_LengthA(blockTags[blockIndex], DOCUMENT_MAX_NAME_LENGTH)) &&
            (FALSE != Document_OrdinalEqualIgnoreCase(tagName, blockTags[blockIndex])))
        {
            isBlockResult = TRUE;
            goto CLEANUP;
        }
    }

CLEANUP:
    return isBlockResult;
}

/* Returns TRUE for elements whose entire body is discarded (script/style/head). */
static BOOL Document_TagDiscardsBody(const char *tagName)
{
    return ((FALSE != Document_OrdinalEqualIgnoreCase(tagName, "script")) ||
            (FALSE != Document_OrdinalEqualIgnoreCase(tagName, "style")) ||
            (FALSE != Document_OrdinalEqualIgnoreCase(tagName, "head"))) ? TRUE : FALSE;
}

/* Advances cursor past the matching closing tag of a discarded element. */
static const char *Document_SkipElementBody(const char *cursor, const char *tagName, size_t tagNameLength)
{
    while ('\0' != *cursor)
    {
        if (('<' == cursor[0]) && ('/' == cursor[1]) &&
            (0 == Platform_ComparePrefixIgnoreCaseA(cursor + 2, tagName, tagNameLength)))
        {
            break;
        }
        cursor += 1;
    }
    /* Step over the closing tag's '>' (no-op at end of string). */
    while (('>' != *cursor) && ('\0' != *cursor))
    {
        cursor += 1;
    }
    if ('>' == *cursor)
    {
        cursor += 1;
    }

    return cursor;
}

/* Emits a single newline at a block-level boundary. Returns TRUE on success. */
static BOOL Document_StripBreakLine(STRIP_CONTEXT *context)
{
    BOOL successResult = TRUE;

    if (FALSE == context->atLineStart)
    {
        if (FALSE == Document_AppendByte(context->builder, '\n'))
        {
            successResult = FALSE;
            goto CLEANUP;
        }
        context->atLineStart = TRUE;
        context->pendingSpace = FALSE;
    }

CLEANUP:
    return successResult;
}

/* Emits one literal character, flushing any pending collapsed space first. */
static BOOL Document_StripEmitChar(STRIP_CONTEXT *context, char character)
{
    BOOL successResult = FALSE;

    if ((FALSE != context->pendingSpace) && (FALSE == context->atLineStart))
    {
        if (FALSE == Document_AppendByte(context->builder, ' '))
        {
            goto CLEANUP;
        }
    }
    context->pendingSpace = FALSE;
    if (FALSE == Document_AppendByte(context->builder, character))
    {
        goto CLEANUP;
    }
    context->atLineStart = FALSE;
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/*
 * Handles a '<...>' tag at cursor: parses its name, discards the bodies of
 * script/style/head, emits a newline for block-level tags, and skips
 * everything else. Returns the cursor advanced past the tag (and any
 * discarded body); *success is FALSE only on an allocation failure.
 */
static const char *Document_StripTag(STRIP_CONTEXT *context, const char *cursor, BOOL *success)
{
    size_t      tagNameLength = 0;
    const char *tagCursor = NULL;

    *success = TRUE;
    context->tagName[0] = '\0';
    tagCursor = cursor + 1;
    if ('/' == *tagCursor)
    {
        tagCursor += 1;
    }
    while ((FALSE == Document_IsXmlWhitespace(*tagCursor)) && ('>' != *tagCursor) &&
           ('\0' != *tagCursor) && (tagNameLength < (DOCUMENT_TAG_NAME_CAPACITY - 1)))
    {
        context->tagName[tagNameLength] = *tagCursor;
        tagNameLength += 1;
        tagCursor += 1;
    }
    context->tagName[tagNameLength] = '\0';

    /* Advance past the end of this opening tag. */
    while (('>' != *cursor) && ('\0' != *cursor))
    {
        cursor += 1;
    }
    if ('>' == *cursor)
    {
        cursor += 1;
    }

    if (FALSE != Document_TagDiscardsBody(context->tagName))
    {
        cursor = Document_SkipElementBody(cursor, context->tagName, tagNameLength);
    }
    else if (FALSE != Document_IsBlockTag(context->tagName))
    {
        *success = Document_StripBreakLine(context);
    }

    return cursor;
}

/*
 * Reduces one NUL-terminated XHTML chapter to UTF-8 plain text in builder,
 * inserting newlines at block boundaries and collapsing whitespace.
 * Returns TRUE on success.
 */
static BOOL Document_StripXhtml(const char *xhtmlContent, BYTE_BUILDER *builder)
{
    STRIP_CONTEXT *context = NULL;
    const char    *cursor = NULL;
    BOOL           successResult = FALSE;

    context = (STRIP_CONTEXT *)Platform_AllocateZeroed(1, sizeof(STRIP_CONTEXT));
    if (NULL == context)
    {
        goto CLEANUP;
    }
    context->builder = builder;
    context->atLineStart = TRUE;
    context->tagName = (char *)Platform_Allocate(DOCUMENT_TAG_NAME_CAPACITY);
    if (NULL == context->tagName)
    {
        goto CLEANUP;
    }

    cursor = xhtmlContent;
    while ('\0' != *cursor)
    {
        if ('<' == *cursor)
        {
            BOOL tagSuccess = TRUE;
            cursor = Document_StripTag(context, cursor, &tagSuccess);
            if (FALSE == tagSuccess)
            {
                goto CLEANUP;
            }
        }
        else if ('&' == *cursor)
        {
            const char *entityEnd = NULL;
            char        decodedCharacter = ' ';

            entityEnd = Platform_FindCharA(cursor, ';');
            if ((NULL != entityEnd) && ((entityEnd - cursor) <= DOCUMENT_MAX_ENTITY_LENGTH))
            {
                decodedCharacter = Document_DecodeEntity(cursor + 1, (size_t)(entityEnd - cursor - 1));
                cursor = entityEnd + 1;
            }
            else
            {
                decodedCharacter = '&';
                cursor += 1;
            }

            if (FALSE == Document_AppendByte(context->builder, decodedCharacter))
            {
                goto CLEANUP;
            }
            context->pendingSpace = FALSE;
            context->atLineStart = FALSE;
        }
        else if (FALSE != Document_IsXmlWhitespace(*cursor))
        {
            context->pendingSpace = TRUE;
            cursor += 1;
        }
        else
        {
            if (FALSE == Document_StripEmitChar(context, *cursor))
            {
                goto CLEANUP;
            }
            cursor += 1;
        }
    }

    successResult = TRUE;

CLEANUP:
    if (NULL != context)
    {
        SAFE_FREE(context->tagName);
        SAFE_FREE(context);
    }
    return successResult;
}

/* Records a chapter boundary at the current text length. Returns TRUE on success. */
static BOOL Document_AddChapter(EPUB_BUILD_CONTEXT *context, const wchar_t *chapterTitle)
{
    DOCUMENT_CHAPTER *grownChapters = NULL;
    size_t            newCapacity = 0;
    size_t            titleLengthWithNul = 0;
    BOOL              successResult = FALSE;

    if (context->chapterCount >= context->chapterCapacity)
    {
        newCapacity = (0 == context->chapterCapacity) ? DOCUMENT_CHAPTER_INITIAL_CAPACITY : (context->chapterCapacity * DOCUMENT_GROWTH_FACTOR);
        grownChapters = (DOCUMENT_CHAPTER *)Platform_Reallocate(context->chapters, newCapacity * sizeof(DOCUMENT_CHAPTER));
        if (NULL == grownChapters)
        {
            goto CLEANUP;
        }
        context->chapters = grownChapters;
        context->chapterCapacity = (unsigned long)newCapacity;
    }

    titleLengthWithNul = Platform_LengthW(chapterTitle, DOCUMENT_MAX_NAME_LENGTH) + DOCUMENT_NUL_TERMINATOR_SIZE;
    context->chapters[context->chapterCount].chapterTitle = (wchar_t *)Platform_Allocate(titleLengthWithNul * sizeof(wchar_t));
    if (NULL == context->chapters[context->chapterCount].chapterTitle)
    {
        goto CLEANUP;
    }
    (void)Platform_CopyW(context->chapters[context->chapterCount].chapterTitle, titleLengthWithNul, chapterTitle);
    context->chapters[context->chapterCount].startCharacterOffset = (unsigned long)context->textBuilder.length;
    context->chapterCount += 1UL;
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/* Loads, strips, and appends a single spine chapter. Returns TRUE on success. */
static BOOL Document_AppendChapter(EPUB_BUILD_CONTEXT *context, const char *chapterHref)
{
    ARCHIVE_ENTRY  *chapterEntry = NULL;
    ARCHIVE_BUFFER *chapterBuffer = NULL;
    BYTE_BUILDER   *strippedText = NULL;
    char           *chapterString = NULL;
    char           *resolvedPath = NULL;
    wchar_t        *wideText = NULL;
    wchar_t        *chapterTitle = NULL;
    int             wideLength = 0;
    BOOL            successResult = FALSE;

    chapterBuffer = (ARCHIVE_BUFFER *)Platform_AllocateZeroed(1, sizeof(ARCHIVE_BUFFER));
    strippedText = (BYTE_BUILDER *)Platform_AllocateZeroed(1, sizeof(BYTE_BUILDER));
    chapterTitle = (wchar_t *)Platform_AllocateZeroed(DOCUMENT_CHAPTER_TITLE_CAPACITY, sizeof(wchar_t));
    if ((NULL == chapterBuffer) || (NULL == strippedText) || (NULL == chapterTitle))
    {
        goto CLEANUP;
    }

    resolvedPath = Document_ResolvePath(context->opfDirectory, chapterHref);
    if (NULL == resolvedPath)
    {
        goto CLEANUP;
    }

    chapterEntry = Archive_FindEntry(context->archive, resolvedPath);
    if (NULL == chapterEntry)
    {
        successResult = TRUE;       /* missing chapter: skip rather than fail */
        goto CLEANUP;
    }
    if (FALSE == Archive_ExtractEntry(context->archive, chapterEntry, chapterBuffer))
    {
        goto CLEANUP;
    }

    chapterString = Document_BufferToString(chapterBuffer);
    if (NULL == chapterString)
    {
        goto CLEANUP;
    }

    if (FALSE == Document_StripXhtml(chapterString, strippedText))
    {
        goto CLEANUP;
    }
    if (0 == strippedText->length)
    {
        successResult = TRUE;       /* empty chapter: nothing to add */
        goto CLEANUP;
    }

    /* Record a chapter marker before the text so navigation can jump to it. */
    (void)wsprintfW(chapterTitle, L"Chapter %lu", context->chapterCount + 1UL);
    if (FALSE == Document_AddChapter(context, chapterTitle))
    {
        goto CLEANUP;
    }

    wideLength = MultiByteToWideChar(CP_UTF8, 0, strippedText->data, (int)strippedText->length, NULL, 0);
    if (0 < wideLength)
    {
        wideText = (wchar_t *)Platform_Allocate((size_t)wideLength * sizeof(wchar_t));
        if (NULL == wideText)
        {
            goto CLEANUP;
        }
        (void)MultiByteToWideChar(CP_UTF8, 0, strippedText->data, (int)strippedText->length, wideText, wideLength);
        if (FALSE == Document_AppendWide(&context->textBuilder, wideText, (size_t)wideLength))
        {
            goto CLEANUP;
        }
    }

    /* Separate chapters with a blank line. */
    if (FALSE == Document_AppendWide(&context->textBuilder, g_chapterSeparator, EREADER_ARRAY_LENGTH(g_chapterSeparator) - 1))
    {
        goto CLEANUP;
    }

    successResult = TRUE;

CLEANUP:
    SAFE_FREE(wideText);
    SAFE_FREE(chapterString);
    SAFE_FREE(resolvedPath);
    SAFE_FREE(chapterTitle);
    if (NULL != strippedText)
    {
        SAFE_FREE(strippedText->data);
        SAFE_FREE(strippedText);
    }
    if (NULL != chapterBuffer)
    {
        SAFE_FREE(chapterBuffer->data);
        SAFE_FREE(chapterBuffer);
    }
    return successResult;
}

/* Parses the OPF manifest into the parallel id/href arrays. Returns TRUE on success. */
static BOOL Document_ParseManifest(EPUB_BUILD_CONTEXT *context, const char *opfContent)
{
    const char *manifestStart = NULL;
    const char *manifestEnd = NULL;
    const char *cursor = NULL;
    unsigned long capacity = 0UL;
    BOOL successResult = FALSE;

    manifestStart = Platform_FindSubstringA(opfContent, "<manifest");
    if (NULL == manifestStart)
    {
        goto CLEANUP;
    }
    manifestEnd = Platform_FindSubstringA(manifestStart, "</manifest>");
    if (NULL == manifestEnd)
    {
        manifestEnd = manifestStart + Platform_LengthA(manifestStart, DOCUMENT_MAX_CONTENT_LENGTH);
    }

    capacity = DOCUMENT_MANIFEST_CAPACITY;
    context->manifestIds = (char **)Platform_AllocateZeroed((size_t)capacity, sizeof(char *));
    context->manifestHrefs = (char **)Platform_AllocateZeroed((size_t)capacity, sizeof(char *));
    if ((NULL == context->manifestIds) || (NULL == context->manifestHrefs))
    {
        goto CLEANUP;
    }

    cursor = manifestStart;
    while (NULL != (cursor = Platform_FindSubstringA(cursor, "<item")))
    {
        char *itemId = NULL;
        char *itemHref = NULL;

        if (cursor >= manifestEnd)
        {
            break;
        }
        /* Reject "<itemref" and similar by requiring a whitespace boundary. */
        if (FALSE == Document_IsXmlWhitespace(cursor[DOCUMENT_ITEM_TAG_LENGTH]))
        {
            cursor += DOCUMENT_ITEM_TAG_LENGTH;
            continue;
        }

        itemId = Document_ExtractAttribute(cursor, "id");
        itemHref = Document_ExtractAttribute(cursor, "href");
        if ((NULL != itemId) && (NULL != itemHref) && (context->manifestCount < capacity))
        {
            Document_PercentDecode(itemHref);
            context->manifestIds[context->manifestCount] = itemId;
            context->manifestHrefs[context->manifestCount] = itemHref;
            context->manifestCount += 1UL;
            itemId = NULL;
            itemHref = NULL;
        }
        SAFE_FREE(itemId);
        SAFE_FREE(itemHref);
        cursor += DOCUMENT_ITEM_TAG_LENGTH;
    }

    successResult = TRUE;

CLEANUP:
    return successResult;
}

/* Looks up a manifest href by item id. Returns the href or NULL (owned by context). */
static const char *Document_HrefForId(EPUB_BUILD_CONTEXT *context, const char *itemId)
{
    const char  *foundHref = NULL;
    unsigned long manifestIndex = 0UL;

    for (manifestIndex = 0UL; manifestIndex < context->manifestCount; manifestIndex += 1UL)
    {
        if (FALSE != Document_OrdinalEqual(context->manifestIds[manifestIndex], itemId))
        {
            foundHref = context->manifestHrefs[manifestIndex];
            goto CLEANUP;
        }
    }

CLEANUP:
    return foundHref;
}

/* Walks the OPF spine in order, appending each referenced chapter. Returns TRUE on success. */
static BOOL Document_ParseSpine(EPUB_BUILD_CONTEXT *context, const char *opfContent)
{
    const char *spineStart = NULL;
    const char *spineEnd = NULL;
    const char *cursor = NULL;
    BOOL successResult = FALSE;

    spineStart = Platform_FindSubstringA(opfContent, "<spine");
    if (NULL == spineStart)
    {
        goto CLEANUP;
    }
    spineEnd = Platform_FindSubstringA(spineStart, "</spine>");
    if (NULL == spineEnd)
    {
        spineEnd = spineStart + Platform_LengthA(spineStart, DOCUMENT_MAX_CONTENT_LENGTH);
    }

    cursor = spineStart;
    while (NULL != (cursor = Platform_FindSubstringA(cursor, "<itemref")))
    {
        char       *referencedId = NULL;
        const char *referencedHref = NULL;
        BOOL        appendSuccess = TRUE;

        if (cursor >= spineEnd)
        {
            break;
        }

        referencedId = Document_ExtractAttribute(cursor, "idref");
        referencedHref = (NULL != referencedId) ? Document_HrefForId(context, referencedId) : NULL;
        if (NULL != referencedHref)
        {
            appendSuccess = Document_AppendChapter(context, referencedHref);
        }
        SAFE_FREE(referencedId);
        if (FALSE == appendSuccess)
        {
            goto CLEANUP;
        }
        cursor += DOCUMENT_ITEMREF_TAG_LENGTH;
    }

    successResult = TRUE;

CLEANUP:
    return successResult;
}

/* Reads container.xml and returns the OPF package path. Returns NULL on failure. */
static char *Document_FindOpfPath(ARCHIVE *archive)
{
    ARCHIVE_ENTRY  *containerEntry = NULL;
    ARCHIVE_BUFFER *containerBuffer = NULL;
    char           *containerString = NULL;
    char           *rootFileElement = NULL;
    char           *opfPath = NULL;

    containerBuffer = (ARCHIVE_BUFFER *)Platform_AllocateZeroed(1, sizeof(ARCHIVE_BUFFER));
    if (NULL == containerBuffer)
    {
        goto CLEANUP;
    }

    containerEntry = Archive_FindEntry(archive, "META-INF/container.xml");
    if (NULL == containerEntry)
    {
        goto CLEANUP;
    }
    if (FALSE == Archive_ExtractEntry(archive, containerEntry, containerBuffer))
    {
        goto CLEANUP;
    }
    containerString = Document_BufferToString(containerBuffer);
    if (NULL == containerString)
    {
        goto CLEANUP;
    }

    /* Search for "<rootfile" but skip the enclosing "<rootfiles>" wrapper by
     * requiring a tag boundary (whitespace) immediately after the name. */
    rootFileElement = containerString;
    while (NULL != (rootFileElement = Platform_FindSubstringA(rootFileElement, "<rootfile")))
    {
        if (FALSE != Document_IsXmlWhitespace(rootFileElement[DOCUMENT_ROOTFILE_TAG_LENGTH]))
        {
            break;
        }
        rootFileElement += DOCUMENT_ROOTFILE_TAG_LENGTH;
    }
    if (NULL == rootFileElement)
    {
        goto CLEANUP;
    }
    opfPath = Document_ExtractAttribute(rootFileElement, "full-path");
    if (NULL != opfPath)
    {
        Document_PercentDecode(opfPath);
    }

CLEANUP:
    SAFE_FREE(containerString);
    if (NULL != containerBuffer)
    {
        SAFE_FREE(containerBuffer->data);
        SAFE_FREE(containerBuffer);
    }
    return opfPath;
}

/* Computes the directory prefix (with trailing '/') of a member path. */
static char *Document_DirectoryOf(const char *memberPath)
{
    char               *directoryPath = NULL;
    char               *lastSlash = NULL;
    size_t              directoryLength = 0;
    MEMORY_COPY_REQUEST directoryCopy = {0};

    lastSlash = Platform_FindLastCharA(memberPath, '/');
    if (NULL == lastSlash)
    {
        directoryPath = Document_DuplicateUtf8("");
        goto CLEANUP;
    }

    directoryLength = (size_t)(lastSlash - memberPath) + 1;     /* keep the slash */
    directoryPath = (char *)Platform_Allocate(directoryLength + DOCUMENT_NUL_TERMINATOR_SIZE);
    if (NULL == directoryPath)
    {
        goto CLEANUP;
    }

    directoryCopy.destination = directoryPath;
    directoryCopy.destinationSize = directoryLength + DOCUMENT_NUL_TERMINATOR_SIZE;
    directoryCopy.source = memberPath;
    directoryCopy.byteCount = directoryLength;
    (void)Platform_CopyMemory(&directoryCopy);

    directoryPath[directoryLength] = '\0';

CLEANUP:
    return directoryPath;
}

/* Frees an EPUB build context and every intermediate buffer it still owns. */
static void Document_FreeBuildContext(EPUB_BUILD_CONTEXT *context)
{
    unsigned long index = 0UL;

    if (NULL == context)
    {
        goto CLEANUP;
    }

    if (NULL != context->manifestIds)
    {
        for (index = 0UL; index < context->manifestCount; index += 1UL)
        {
            SAFE_FREE(context->manifestIds[index]);
        }
        SAFE_FREE(context->manifestIds);
    }
    if (NULL != context->manifestHrefs)
    {
        for (index = 0UL; index < context->manifestCount; index += 1UL)
        {
            SAFE_FREE(context->manifestHrefs[index]);
        }
        SAFE_FREE(context->manifestHrefs);
    }
    if (NULL != context->chapters)
    {
        for (index = 0UL; index < context->chapterCount; index += 1UL)
        {
            SAFE_FREE(context->chapters[index].chapterTitle);
        }
        SAFE_FREE(context->chapters);
    }
    SAFE_FREE(context->textBuilder.data);
    SAFE_FREE(context->opfDirectory);
    SAFE_FREE(context);

CLEANUP:
    return;
}

/* Assembles the EPUB text model from the archive. Returns TRUE on success. */
static BOOL Document_BuildEpub(DOCUMENT *document)
{
    EPUB_BUILD_CONTEXT *context = NULL;
    ARCHIVE_BUFFER     *opfBuffer = NULL;
    char               *opfPath = NULL;
    char               *opfString = NULL;
    ARCHIVE_ENTRY      *opfEntry = NULL;
    BOOL                successResult = FALSE;

    context = (EPUB_BUILD_CONTEXT *)Platform_AllocateZeroed(1, sizeof(EPUB_BUILD_CONTEXT));
    opfBuffer = (ARCHIVE_BUFFER *)Platform_AllocateZeroed(1, sizeof(ARCHIVE_BUFFER));
    if ((NULL == context) || (NULL == opfBuffer))
    {
        goto CLEANUP;
    }
    context->archive = document->backingArchive;

    opfPath = Document_FindOpfPath(document->backingArchive);
    if (NULL == opfPath)
    {
        goto CLEANUP;
    }
    context->opfDirectory = Document_DirectoryOf(opfPath);
    if (NULL == context->opfDirectory)
    {
        goto CLEANUP;
    }

    opfEntry = Archive_FindEntry(document->backingArchive, opfPath);
    if (NULL == opfEntry)
    {
        goto CLEANUP;
    }
    if (FALSE == Archive_ExtractEntry(document->backingArchive, opfEntry, opfBuffer))
    {
        goto CLEANUP;
    }
    opfString = Document_BufferToString(opfBuffer);
    if (NULL == opfString)
    {
        goto CLEANUP;
    }

    if (FALSE == Document_ParseManifest(context, opfString))
    {
        goto CLEANUP;
    }
    if (FALSE == Document_ParseSpine(context, opfString))
    {
        goto CLEANUP;
    }
    if ((NULL == context->textBuilder.data) || (0 == context->textBuilder.length))
    {
        goto CLEANUP;       /* nothing readable was produced */
    }

    document->documentType = DOCUMENT_TYPE_EPUB;
    document->combinedText = context->textBuilder.data;
    document->combinedTextLength = (unsigned long)context->textBuilder.length;
    document->chapters = context->chapters;
    document->chapterCount = context->chapterCount;
    context->textBuilder.data = NULL;        /* ownership transferred */
    context->chapters = NULL;
    successResult = TRUE;

CLEANUP:
    Document_FreeBuildContext(context);
    SAFE_FREE(opfString);
    if (NULL != opfBuffer)
    {
        SAFE_FREE(opfBuffer->data);
        SAFE_FREE(opfBuffer);
    }
    SAFE_FREE(opfPath);
    return successResult;
}

/*---------------------------------------------------------------------------
 * Public interface
 *--------------------------------------------------------------------------*/

BOOL Document_Open(const wchar_t *documentFilePath, DOCUMENT **openedDocument)
{
    DOCUMENT *document = NULL;
    BOOL      successResult = FALSE;

    if ((NULL == documentFilePath) || (NULL == openedDocument))
    {
        goto CLEANUP;
    }
    *openedDocument = NULL;

    document = (DOCUMENT *)Platform_AllocateZeroed(1, sizeof(DOCUMENT));
    if (NULL == document)
    {
        goto CLEANUP;
    }

    if (FALSE == Archive_Open(documentFilePath, &document->backingArchive))
    {
        goto CLEANUP;
    }

    /* An EPUB is identified by its container; everything else is treated as CBZ. */
    if (NULL != Archive_FindEntry(document->backingArchive, "META-INF/container.xml"))
    {
        if (FALSE == Document_BuildEpub(document))
        {
            goto CLEANUP;
        }
    }
    else
    {
        if (FALSE == Document_BuildCbz(document))
        {
            goto CLEANUP;
        }
    }

    *openedDocument = document;
    document = NULL;        /* ownership transferred to the caller */
    successResult = TRUE;

CLEANUP:
    if (NULL != document)
    {
        Document_Close(document);
    }
    return successResult;
}

void Document_Close(DOCUMENT *documentToClose)
{
    unsigned long index = 0UL;

    if (NULL == documentToClose)
    {
        goto CLEANUP;
    }

    if (NULL != documentToClose->imageEntryNames)
    {
        for (index = 0UL; index < documentToClose->imageCount; index += 1UL)
        {
            SAFE_FREE(documentToClose->imageEntryNames[index]);
        }
        SAFE_FREE(documentToClose->imageEntryNames);
    }

    if (NULL != documentToClose->chapters)
    {
        for (index = 0UL; index < documentToClose->chapterCount; index += 1UL)
        {
            SAFE_FREE(documentToClose->chapters[index].chapterTitle);
        }
        SAFE_FREE(documentToClose->chapters);
    }

    SAFE_FREE(documentToClose->combinedText);

    if (NULL != documentToClose->backingArchive)
    {
        Archive_Close(documentToClose->backingArchive);
        documentToClose->backingArchive = NULL;
    }

    Platform_Free(documentToClose);

CLEANUP:
    return;
}
