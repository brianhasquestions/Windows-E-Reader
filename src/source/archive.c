/*****************************************************************************
 * archive.c
 *
 * Read-only ZIP reader. The whole file is memory-mapped with the Win32 file
 * mapping API so that members can be located and copied without buffered
 * reads. The End Of Central Directory record is found by scanning backward
 * from the end of the file; the central directory it points to yields the
 * member list. Members are extracted into freshly allocated buffers, with
 * DEFLATE members handed to the inflate module.
 *****************************************************************************/

#include "archive.h"
#include "inflate.h"

/* ZIP record signatures. */
#define ZIP_SIGNATURE_END_OF_CENTRAL_DIRECTORY 0x06054b50UL
#define ZIP_SIGNATURE_CENTRAL_DIRECTORY        0x02014b50UL
#define ZIP_SIGNATURE_LOCAL_HEADER             0x04034b50UL

/* Compression methods this reader understands. */
#define ZIP_COMPRESSION_STORED                 0
#define ZIP_COMPRESSION_DEFLATE                8

/* Low-level layout constants. */
#define ZIP_BITS_PER_BYTE                      8
#define ZIP_MAX_COMMENT_LENGTH                 0xFFFFUL
#define ZIP_MAX_32BIT_FILE_SIZE                0xFFFFFFFFLL

/* Fixed-size record lengths (excluding the variable name/extra/comment tails). */
#define ZIP_EOCD_MINIMUM_SIZE                  22
#define ZIP_CENTRAL_HEADER_SIZE                46
#define ZIP_LOCAL_HEADER_SIZE                  30

/* Field byte offsets inside the End Of Central Directory record. */
#define ZIP_EOCD_TOTAL_ENTRIES_OFFSET          10
#define ZIP_EOCD_DIRECTORY_OFFSET_FIELD        16

/* Field byte offsets inside a central-directory file header. */
#define ZIP_CENTRAL_METHOD_OFFSET              10
#define ZIP_CENTRAL_COMPRESSED_SIZE_OFFSET     20
#define ZIP_CENTRAL_UNCOMPRESSED_SIZE_OFFSET   24
#define ZIP_CENTRAL_NAME_LENGTH_OFFSET         28
#define ZIP_CENTRAL_EXTRA_LENGTH_OFFSET        30
#define ZIP_CENTRAL_COMMENT_LENGTH_OFFSET      32
#define ZIP_CENTRAL_LOCAL_HEADER_OFFSET_FIELD  42
#define ZIP_CENTRAL_NAME_OFFSET                46

/* Field byte offsets inside a local file header. */
#define ZIP_LOCAL_NAME_LENGTH_OFFSET           26
#define ZIP_LOCAL_EXTRA_LENGTH_OFFSET          28

/* The most bytes that must be scanned backward to find the EOCD signature. */
#define ZIP_MAX_COMMENT_SCAN \
    (ZIP_MAX_COMMENT_LENGTH + (unsigned long)ZIP_EOCD_MINIMUM_SIZE)

/* One-byte NUL terminator allowance for allocated name strings. */
#define ARCHIVE_NUL_TERMINATOR_SIZE            1U

/* Reads a little-endian 16-bit value from a mapped location. */
static unsigned short Archive_ReadUInt16(const unsigned char *location)
{
    unsigned short readValue = 0;

    readValue = (unsigned short)((unsigned short)location[0] | ((unsigned short)location[1] << ZIP_BITS_PER_BYTE));

    goto CLEANUP;

CLEANUP:
    return readValue;
}

/* Reads a little-endian 32-bit value from a mapped location. */
static unsigned long Archive_ReadUInt32(const unsigned char *location)
{
    unsigned long readValue = 0UL;

    readValue = (unsigned long)location[0] |
                ((unsigned long)location[1] << ZIP_BITS_PER_BYTE) |
                ((unsigned long)location[2] << (ZIP_BITS_PER_BYTE * 2)) |
                ((unsigned long)location[3] << (ZIP_BITS_PER_BYTE * 3));

    goto CLEANUP;

CLEANUP:
    return readValue;
}

/*
 * Locates the End Of Central Directory record by scanning backward from the
 * end of the mapped file. Returns the offset, or 0 when not found (a real
 * EOCD never sits at offset 0 because of the preceding member data).
 */
static unsigned long Archive_FindEndOfCentralDirectory(ARCHIVE *archive)
{
    unsigned long scanLimit = 0UL;
    unsigned long scanOffset = 0UL;
    unsigned long foundOffset = 0UL;

    if ((unsigned long)ZIP_EOCD_MINIMUM_SIZE > archive->fileSize)
    {
        goto CLEANUP;
    }

    scanLimit = archive->fileSize;
    if (scanLimit > ZIP_MAX_COMMENT_SCAN)
    {
        scanLimit = ZIP_MAX_COMMENT_SCAN;
    }

    /* Walk backward from the latest position an EOCD signature could begin. */
    for (scanOffset = archive->fileSize - (unsigned long)ZIP_EOCD_MINIMUM_SIZE; (archive->fileSize - scanOffset) <= scanLimit; scanOffset -= 1UL)
    {
        if (ZIP_SIGNATURE_END_OF_CENTRAL_DIRECTORY == Archive_ReadUInt32(archive->baseAddress + scanOffset))
        {
            foundOffset = scanOffset;
            goto CLEANUP;
        }

        if (0UL == scanOffset)
        {
            break;
        }
    }

CLEANUP:
    return foundOffset;
}

/* Parses the central directory into archive->entries. Returns TRUE on success. */
static BOOL Archive_ParseCentralDirectory(ARCHIVE *archive)
{
    unsigned long endOfCentralDirectoryOffset = 0UL;
    unsigned long centralDirectoryOffset = 0UL;
    unsigned long walkingOffset = 0UL;
    unsigned long entryIndex = 0UL;
    unsigned short totalEntryCount = 0;
    BOOL successResult = FALSE;

    endOfCentralDirectoryOffset = Archive_FindEndOfCentralDirectory(archive);
    if (0UL == endOfCentralDirectoryOffset)
    {
        goto CLEANUP;
    }

    totalEntryCount = Archive_ReadUInt16(archive->baseAddress + endOfCentralDirectoryOffset + ZIP_EOCD_TOTAL_ENTRIES_OFFSET);
    centralDirectoryOffset = Archive_ReadUInt32(archive->baseAddress + endOfCentralDirectoryOffset + ZIP_EOCD_DIRECTORY_OFFSET_FIELD);
    if ((0 == totalEntryCount) || (centralDirectoryOffset >= archive->fileSize))
    {
        goto CLEANUP;
    }

    archive->entries = (ARCHIVE_ENTRY *)Platform_AllocateZeroed((size_t)totalEntryCount, sizeof(ARCHIVE_ENTRY));
    if (NULL == archive->entries)
    {
        goto CLEANUP;
    }

    walkingOffset = centralDirectoryOffset;
    for (entryIndex = 0UL; entryIndex < (unsigned long)totalEntryCount; entryIndex += 1UL)
    {
        const unsigned char *recordBase = NULL;
        unsigned short nameLength = 0;
        unsigned short extraLength = 0;
        unsigned short commentLength = 0;
        ARCHIVE_ENTRY *currentEntry = NULL;
        MEMORY_COPY_REQUEST nameCopy = {0};

        if ((walkingOffset + (unsigned long)ZIP_CENTRAL_HEADER_SIZE) > archive->fileSize)
        {
            goto CLEANUP;
        }

        recordBase = archive->baseAddress + walkingOffset;
        if (ZIP_SIGNATURE_CENTRAL_DIRECTORY != Archive_ReadUInt32(recordBase))
        {
            goto CLEANUP;
        }

        nameLength = Archive_ReadUInt16(recordBase + ZIP_CENTRAL_NAME_LENGTH_OFFSET);
        extraLength = Archive_ReadUInt16(recordBase + ZIP_CENTRAL_EXTRA_LENGTH_OFFSET);
        commentLength = Archive_ReadUInt16(recordBase + ZIP_CENTRAL_COMMENT_LENGTH_OFFSET);

        if ((walkingOffset + (unsigned long)ZIP_CENTRAL_HEADER_SIZE +
             (unsigned long)nameLength) > archive->fileSize)
        {
            goto CLEANUP;
        }

        currentEntry = &archive->entries[entryIndex];
        currentEntry->compressionMethod = Archive_ReadUInt16(recordBase + ZIP_CENTRAL_METHOD_OFFSET);
        currentEntry->compressedSize = Archive_ReadUInt32(recordBase + ZIP_CENTRAL_COMPRESSED_SIZE_OFFSET);
        currentEntry->uncompressedSize = Archive_ReadUInt32(recordBase + ZIP_CENTRAL_UNCOMPRESSED_SIZE_OFFSET);
        currentEntry->localHeaderOffset = Archive_ReadUInt32(recordBase + ZIP_CENTRAL_LOCAL_HEADER_OFFSET_FIELD);

        currentEntry->entryName = (char *)Platform_Allocate((size_t)nameLength + ARCHIVE_NUL_TERMINATOR_SIZE);
        if (NULL == currentEntry->entryName)
        {
            goto CLEANUP;
        }
        if (0 != nameLength)
        {
            nameCopy.destination = currentEntry->entryName;
            nameCopy.destinationSize = (size_t)nameLength + ARCHIVE_NUL_TERMINATOR_SIZE;
            nameCopy.source = recordBase + ZIP_CENTRAL_NAME_OFFSET;
            nameCopy.byteCount = (size_t)nameLength;
            (void)Platform_CopyMemory(&nameCopy);
        }
        currentEntry->entryName[nameLength] = '\0';

        archive->entryCount += 1UL;
        walkingOffset += (unsigned long)ZIP_CENTRAL_HEADER_SIZE + (unsigned long)nameLength + (unsigned long)extraLength + (unsigned long)commentLength;
    }

    successResult = TRUE;

CLEANUP:
    return successResult;
}

BOOL Archive_Open(const wchar_t *archiveFilePath, ARCHIVE **openedArchive)
{
    ARCHIVE       *archive = NULL;
    LARGE_INTEGER *fileSize = NULL;
    BOOL           successResult = FALSE;

    if ((NULL == archiveFilePath) || (NULL == openedArchive))
    {
        goto CLEANUP;
    }
    *openedArchive = NULL;

    fileSize = (LARGE_INTEGER *)Platform_AllocateZeroed(1, sizeof(LARGE_INTEGER));
    if (NULL == fileSize)
    {
        goto CLEANUP;
    }

    archive = (ARCHIVE *)Platform_AllocateZeroed(1, sizeof(ARCHIVE));
    if (NULL == archive)
    {
        goto CLEANUP;
    }
     
    archive->fileHandle = CreateFileW(archiveFilePath, GENERIC_READ, FILE_SHARE_READ,NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (INVALID_HANDLE_VALUE == archive->fileHandle)
    {
        goto CLEANUP;
    }

    if (FALSE == GetFileSizeEx(archive->fileHandle, fileSize))
    {
        goto CLEANUP;
    }
    /* A ZIP central directory uses 32-bit offsets; reject ZIP64-scale files. */
    if ((0 == fileSize->QuadPart) || (ZIP_MAX_32BIT_FILE_SIZE < fileSize->QuadPart))
    {
        goto CLEANUP;
    }
    archive->fileSize = (unsigned long)fileSize->QuadPart;

    archive->mappingHandle = CreateFileMappingW(archive->fileHandle, NULL, PAGE_READONLY, 0, 0, NULL);
    if (NULL == archive->mappingHandle)
    {
        goto CLEANUP;
    }

    archive->baseAddress = (unsigned char *)MapViewOfFile(archive->mappingHandle, FILE_MAP_READ, 0, 0, 0);
    if (NULL == archive->baseAddress)
    {
        goto CLEANUP;
    }

    if (FALSE == Archive_ParseCentralDirectory(archive))
    {
        goto CLEANUP;
    }

    *openedArchive = archive;
    archive = NULL;     /* ownership transferred to the caller */
    successResult = TRUE;

CLEANUP:
    if (NULL != archive)
    {
        Archive_Close(archive);
    }
    SAFE_FREE(fileSize);
    return successResult;
}

void Archive_Close(ARCHIVE *archiveToClose)
{
    unsigned long entryIndex = 0UL;

    if (NULL == archiveToClose)
    {
        goto CLEANUP;
    }

    if (NULL != archiveToClose->entries)
    {
        for (entryIndex = 0UL; entryIndex < archiveToClose->entryCount; entryIndex += 1UL)
        {
            SAFE_FREE(archiveToClose->entries[entryIndex].entryName);
        }
        SAFE_FREE(archiveToClose->entries);
    }

    if (NULL != archiveToClose->baseAddress)
    {
        (void)UnmapViewOfFile(archiveToClose->baseAddress);
        archiveToClose->baseAddress = NULL;
    }
    if (NULL != archiveToClose->mappingHandle)
    {
        (void)CloseHandle(archiveToClose->mappingHandle);
        archiveToClose->mappingHandle = NULL;
    }
    if ((NULL != archiveToClose->fileHandle) &&
        (INVALID_HANDLE_VALUE != archiveToClose->fileHandle))
    {
        (void)CloseHandle(archiveToClose->fileHandle);
        archiveToClose->fileHandle = NULL;
    }

    Platform_Free(archiveToClose);

CLEANUP:
    return;
}

/*
 * Win32-native, case-sensitive ordinal equality for two UTF-8 member names.
 * Uses CompareStringOrdinal (the recommended secure replacement for the CRT
 * strcmp) on widened copies. Returns TRUE only on an exact match.
 */
static BOOL Archive_OrdinalEqual(const char *leftString, const char *rightString)
{
    wchar_t *leftWide = NULL;
    wchar_t *rightWide = NULL;
    int leftWideLength = 0;
    int rightWideLength = 0;
    BOOL areEqual = FALSE;

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

    if (CSTR_EQUAL == CompareStringOrdinal(leftWide, -1, rightWide, -1, FALSE))
    {
        areEqual = TRUE;
    }

CLEANUP:
    SAFE_FREE(rightWide);
    SAFE_FREE(leftWide);
    return areEqual;
}

ARCHIVE_ENTRY *Archive_FindEntry(ARCHIVE *archiveToSearch, const char *entryName)
{
    ARCHIVE_ENTRY *foundEntry = NULL;
    unsigned long entryIndex = 0UL;

    if ((NULL == archiveToSearch) || (NULL == entryName))
    {
        goto CLEANUP;
    }

    for (entryIndex = 0UL; entryIndex < archiveToSearch->entryCount; entryIndex += 1UL)
    {
        if (FALSE != Archive_OrdinalEqual(archiveToSearch->entries[entryIndex].entryName, entryName))
        {
            foundEntry = &archiveToSearch->entries[entryIndex];
            goto CLEANUP;
        }
    }

CLEANUP:
    return foundEntry;
}

BOOL Archive_ExtractEntry(ARCHIVE *sourceArchive, ARCHIVE_ENTRY *entryToExtract, ARCHIVE_BUFFER *extractedBuffer)
{
    const unsigned char *localHeader = NULL;
    const unsigned char *compressedData = NULL;
    unsigned char       *outputData = NULL;
    INFLATE_REQUEST     *inflateRequest = NULL;
    MEMORY_COPY_REQUEST  storedCopy = {0};
    unsigned short       localNameLength = 0;
    unsigned short       localExtraLength = 0;
    unsigned long        dataOffset = 0UL;
    unsigned long        allocationSize = 0UL;
    BOOL                 successResult = FALSE;

    if ((NULL == sourceArchive) || (NULL == entryToExtract) || (NULL == extractedBuffer))
    {
        goto CLEANUP;
    }
    extractedBuffer->data = NULL;
    extractedBuffer->size = 0UL;

    if ((entryToExtract->localHeaderOffset + (unsigned long)ZIP_LOCAL_HEADER_SIZE) >
        sourceArchive->fileSize)
    {
        goto CLEANUP;
    }

    localHeader = sourceArchive->baseAddress + entryToExtract->localHeaderOffset;
    if (ZIP_SIGNATURE_LOCAL_HEADER != Archive_ReadUInt32(localHeader))
    {
        goto CLEANUP;
    }

    localNameLength = Archive_ReadUInt16(localHeader + ZIP_LOCAL_NAME_LENGTH_OFFSET);
    localExtraLength = Archive_ReadUInt16(localHeader + ZIP_LOCAL_EXTRA_LENGTH_OFFSET);
    dataOffset = entryToExtract->localHeaderOffset + (unsigned long)ZIP_LOCAL_HEADER_SIZE + (unsigned long)localNameLength + (unsigned long)localExtraLength;

    if ((dataOffset + entryToExtract->compressedSize) > sourceArchive->fileSize)
    {
        goto CLEANUP;
    }
    compressedData = sourceArchive->baseAddress + dataOffset;

    /* Always allocate at least one byte so an empty member still yields a buffer. */
    allocationSize = entryToExtract->uncompressedSize;
    if (0UL == allocationSize)
    {
        allocationSize = ARCHIVE_NUL_TERMINATOR_SIZE;
    }

    outputData = (unsigned char *)Platform_Allocate((size_t)allocationSize);
    if (NULL == outputData)
    {
        goto CLEANUP;
    }

    if (ZIP_COMPRESSION_STORED == entryToExtract->compressionMethod)
    {
        if (entryToExtract->compressedSize != entryToExtract->uncompressedSize)
        {
            goto CLEANUP;
        }
        if (0UL != entryToExtract->uncompressedSize)
        {
            storedCopy.destination = outputData;
            storedCopy.destinationSize = (size_t)allocationSize;
            storedCopy.source = compressedData;
            storedCopy.byteCount = (size_t)entryToExtract->uncompressedSize;
            (void)Platform_CopyMemory(&storedCopy);
        }
    }
    else if (ZIP_COMPRESSION_DEFLATE == entryToExtract->compressionMethod)
    {
        inflateRequest = (INFLATE_REQUEST *)Platform_AllocateZeroed(1, sizeof(INFLATE_REQUEST));
        if (NULL == inflateRequest)
        {
            goto CLEANUP;
        }

        inflateRequest->sourceData = compressedData;
        inflateRequest->sourceLength = entryToExtract->compressedSize;
        inflateRequest->destinationData = outputData;
        inflateRequest->destinationLength = entryToExtract->uncompressedSize;

        if (FALSE == Inflate_Decompress(inflateRequest))
        {
            goto CLEANUP;
        }
    }
    else
    {
        goto CLEANUP;       /* unsupported compression method */
    }

    extractedBuffer->data = outputData;
    extractedBuffer->size = entryToExtract->uncompressedSize;
    outputData = NULL;      /* ownership transferred to the caller */
    successResult = TRUE;

CLEANUP:
    SAFE_FREE(inflateRequest);
    SAFE_FREE(outputData);
    return successResult;
}
