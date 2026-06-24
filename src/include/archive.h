/*****************************************************************************
 * archive.h
 *
 * Minimal read-only ZIP archive reader built directly on the Win32 file
 * mapping API. EPUB and CBZ files are both ZIP containers, so this module is
 * the shared foundation underneath the document layer. It parses the central
 * directory, exposes the member list, and extracts individual members into
 * freshly allocated buffers (transparently inflating DEFLATE members).
 *****************************************************************************/

#ifndef ARCHIVE_H
#define ARCHIVE_H

#include "ereader.h"

/* Description of one member inside the ZIP central directory. */
typedef struct ARCHIVE_ENTRY
{
    char         *entryName;            /* NUL-terminated UTF-8 member name    */
    unsigned long localHeaderOffset;    /* byte offset of the local header     */
    unsigned long compressedSize;       /* stored size in the archive          */
    unsigned long uncompressedSize;     /* size after decompression            */
    unsigned short compressionMethod;   /* 0 = stored, 8 = DEFLATE             */
} ARCHIVE_ENTRY;

/* An opened ZIP archive together with its memory-mapped backing store. */
typedef struct ARCHIVE
{
    HANDLE         fileHandle;          /* open file handle                    */
    HANDLE         mappingHandle;       /* file-mapping object                 */
    unsigned char *baseAddress;         /* mapped view of the whole file       */
    unsigned long  fileSize;            /* total bytes in the file             */
    ARCHIVE_ENTRY *entries;             /* dynamically allocated member array  */
    unsigned long  entryCount;          /* number of members                   */
} ARCHIVE;

/* Result of extracting a single member: a heap buffer the caller must free. */
typedef struct ARCHIVE_BUFFER
{
    unsigned char *data;                /* dynamically allocated bytes         */
    unsigned long  size;                /* number of valid bytes               */
} ARCHIVE_BUFFER;

/*
 * Opens the ZIP archive at archiveFilePath and parses its central directory.
 * On success *openedArchive points to a freshly allocated ARCHIVE that must
 * be released with Archive_Close. Returns TRUE on success.
 */
BOOL Archive_Open(const wchar_t *archiveFilePath, ARCHIVE **openedArchive);

/* Releases an archive and all owned resources. Safe to call with NULL. */
void Archive_Close(ARCHIVE *archiveToClose);

/*
 * Returns the member whose name matches entryName (case sensitive), or NULL
 * when no such member exists. The returned pointer is owned by the archive.
 */
ARCHIVE_ENTRY *Archive_FindEntry(ARCHIVE *archiveToSearch, const char *entryName);

/*
 * Extracts the given member, allocating extractedBuffer->data and filling
 * extractedBuffer->size. The caller frees the buffer. Returns TRUE on success.
 */
BOOL Archive_ExtractEntry(ARCHIVE *sourceArchive, ARCHIVE_ENTRY *entryToExtract, ARCHIVE_BUFFER *extractedBuffer);

#endif /* ARCHIVE_H */
