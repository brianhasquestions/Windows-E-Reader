/*****************************************************************************
 * renderer.h
 *
 * Presentation layer. The renderer turns the active DOCUMENT into pixels on
 * the window's device context. For EPUB documents it word-wraps the combined
 * text into lines and groups those lines into pages; for CBZ documents it
 * decodes each page image with the Windows Imaging Component and blits it
 * with the requested fit mode. It also computes how many pages exist so the
 * application core can clamp navigation.
 *****************************************************************************/

#ifndef RENDERER_H
#define RENDERER_H

#include "ereader.h"
#include "application.h"   /* THEME, and the APP_CONTEXT shared state */

/* The application context is referenced by pointer to avoid a header cycle. */
struct APP_CONTEXT;

/* One word-wrapped line of EPUB text, expressed as a slice of combinedText. */
typedef struct TEXT_LINE
{
    unsigned long startOffset;          /* offset into DOCUMENT.combinedText   */
    unsigned long characterCount;       /* characters on this line             */
} TEXT_LINE;

/*
 * Immutable inputs shared by every line-break measurement during one layout
 * pass. The single SIZE scratch buffer is allocated once and reused for every
 * GetTextExtentExPointW call so the wrap loop performs no per-line allocation.
 */
typedef struct TEXT_WRAP_CONTEXT
{
    HDC            deviceContext;       /* DC with the body font selected      */
    const wchar_t *text;               /* the document's combined text         */
    SIZE          *measureSize;        /* reused GDI measurement scratch       */
    unsigned long  textLength;         /* characters in text (NUL-terminated)  */
    int            availableWidth;     /* usable text width in pixels          */
} TEXT_WRAP_CONTEXT;

/* Where one wrapped line ends and the next one begins. */
typedef struct LINE_BREAK_RESULT
{
    unsigned long lineLength;          /* characters to emit on this line      */
    unsigned long nextStart;           /* offset where the next line begins    */
} LINE_BREAK_RESULT;

/* All renderer-owned resources and the cached layout for the current view. */
typedef struct RENDERER
{
    /* Shared text resources. */
    HFONT      textFont;                /* current EPUB body font              */
    int        lineHeight;              /* pixels per text line                */
    int        linesPerPage;            /* text lines that fit in the viewport */

    /* EPUB layout cache. */
    TEXT_LINE *layoutLines;             /* dynamically allocated line table    */
    unsigned long layoutLineCount;
    unsigned long layoutLineCapacity;   /* allocated slots in layoutLines      */

    /* CBZ image resources (WIC factory kept as void* to keep this header light). */
    void      *wicFactory;             /* IWICImagingFactory *                 */
    HBITMAP    cachedBitmap;           /* decoded image for cachedImageIndex   */
    int        cachedBitmapWidth;
    int        cachedBitmapHeight;
    int        cachedImageIndex;       /* which page the cache holds, or -1    */
} RENDERER;

/*
 * Theme palette accessors. These are the single source of truth for the
 * reading background and text colours; the window chrome (menu bar) reuses
 * them so the menu matches the page.
 */
COLORREF Renderer_BackgroundColor(THEME theme);
COLORREF Renderer_TextColor(THEME theme);

/* Creates the renderer and its COM resources. Returns TRUE on success. */
BOOL Renderer_Create(RENDERER **createdRenderer);

/* Releases the renderer and everything it owns. Safe to call with NULL. */
void Renderer_Destroy(RENDERER *rendererToDestroy);

/*
 * Recomputes pagination for the current document, window size, and view
 * settings, returning the total number of pages. Must be called whenever the
 * document, window size, font, or fit mode changes.
 */
int Renderer_Layout(struct APP_CONTEXT *applicationContext);

/* Paints the current page onto deviceContext. */
void Renderer_Paint(struct APP_CONTEXT *applicationContext, HDC deviceContext);

#endif /* RENDERER_H */
