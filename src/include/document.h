/*****************************************************************************
 * document.h
 *
 * Document abstraction layer. A DOCUMENT hides whether the opened file is a
 * comic book archive (CBZ: an ordered set of page images) or an electronic
 * book (EPUB: an ordered set of XHTML chapters reduced to reflowable text).
 * The renderer talks to this abstraction rather than to the raw archive.
 *****************************************************************************/

#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "ereader.h"
#include "archive.h"

/* The kind of content a DOCUMENT carries. */
typedef enum DOCUMENT_TYPE
{
    DOCUMENT_TYPE_NONE = 0,
    DOCUMENT_TYPE_CBZ,                  /* image-per-page comic archive        */
    DOCUMENT_TYPE_EPUB                  /* reflowable text e-book              */
} DOCUMENT_TYPE;

/* One chapter boundary inside the combined EPUB text stream. */
typedef struct DOCUMENT_CHAPTER
{
    wchar_t      *chapterTitle;         /* display title (may be a fallback)   */
    unsigned long startCharacterOffset; /* offset into combinedText            */
} DOCUMENT_CHAPTER;

/* An opened document, owning the backing archive and the parsed model. */
typedef struct DOCUMENT
{
    DOCUMENT_TYPE     documentType;
    ARCHIVE          *backingArchive;

    /* CBZ model: ordered list of image member names. */
    char            **imageEntryNames;
    unsigned long     imageCount;

    /* EPUB model: a single combined wide-character text stream plus chapters. */
    wchar_t          *combinedText;
    unsigned long     combinedTextLength;
    DOCUMENT_CHAPTER *chapters;
    unsigned long     chapterCount;
} DOCUMENT;

/*
 * Internal build/scratch structures. These are implementation details used
 * only while constructing a DOCUMENT, but per project convention all struct
 * definitions live in the headers rather than the source files.
 */

/* A growable UTF-8 byte buffer used while stripping XHTML markup. */
typedef struct BYTE_BUILDER
{
    char  *data;
    size_t length;
    size_t capacity;
} BYTE_BUILDER;

/* A growable wide-character buffer that accumulates the whole book's text. */
typedef struct WIDE_BUILDER
{
    wchar_t *data;
    size_t   length;
    size_t   capacity;
} WIDE_BUILDER;

/*
 * One CBZ image name paired with its pre-widened UTF-16 sort key, so the
 * page-ordering sort compares cached keys instead of re-converting and
 * re-allocating on every comparison.
 */
typedef struct IMAGE_SORT_ITEM
{
    char    *originalName;   /* borrowed: element of the caller's name array */
    wchar_t *wideKey;        /* owned: UTF-16 key for ordinal comparison     */
} IMAGE_SORT_ITEM;

/* One named XHTML entity and the character it decodes to (e.g. "amp" -> '&'). */
typedef struct DOCUMENT_NAMED_ENTITY
{
    const char *name;
    char        character;
} DOCUMENT_NAMED_ENTITY;

/* Threaded state for the XHTML-to-text reduction, shared by its helpers. */
typedef struct STRIP_CONTEXT
{
    BYTE_BUILDER *builder;       /* receives the emitted plain text          */
    char         *tagName;       /* scratch buffer, DOCUMENT_TAG_NAME_CAPACITY */
    BOOL          atLineStart;   /* TRUE while no text has followed a newline */
    BOOL          pendingSpace;  /* collapsed whitespace awaiting a flush     */
} STRIP_CONTEXT;

/* Everything needed while assembling an EPUB document model. */
typedef struct EPUB_BUILD_CONTEXT
{
    ARCHIVE         *archive;
    char            *opfDirectory;      /* directory prefix of the OPF, may be "" */
    char           **manifestIds;       /* parallel id / href arrays            */
    char           **manifestHrefs;
    unsigned long    manifestCount;
    WIDE_BUILDER     textBuilder;
    DOCUMENT_CHAPTER *chapters;
    unsigned long    chapterCount;
    unsigned long    chapterCapacity;
} EPUB_BUILD_CONTEXT;

/*
 * Opens documentFilePath, auto-detecting EPUB versus CBZ from the archive
 * contents. On success *openedDocument receives a freshly allocated DOCUMENT
 * that must be released with Document_Close. Returns TRUE on success.
 */
BOOL Document_Open(const wchar_t *documentFilePath, DOCUMENT **openedDocument);

/* Releases a document and everything it owns. Safe to call with NULL. */
void Document_Close(DOCUMENT *documentToClose);

#endif /* DOCUMENT_H */
