/*****************************************************************************
 * renderer.c
 *
 * Presentation layer.
 *
 *  - EPUB: the combined text is word-wrapped to the current viewport width
 *    using GDI text measurement, the wrapped lines are cached, and one
 *    screenful of lines is painted per page.
 *  - CBZ: the current page image is decoded with the Windows Imaging
 *    Component into a 32bpp DIB and blitted with the requested fit mode,
 *    zoom, and pan. The most recently decoded image is cached so repeated
 *    repaints of the same page do not re-decode.
 *****************************************************************************/

#define COBJMACROS

#include "renderer.h"
#include "application.h"
#include "document.h"

#include <objbase.h>
#include <wincodec.h>
#include <shlwapi.h>       /* StrChrW / StrRChrW for loop-free line scanning */

/* Layout geometry. */
#define RENDERER_TEXT_MARGIN              28   /* pixels of margin around EPUB text */
#define RENDERER_TOTAL_HORIZONTAL_MARGIN (RENDERER_TEXT_MARGIN + RENDERER_TEXT_MARGIN)
#define RENDERER_TOTAL_VERTICAL_MARGIN   (RENDERER_TEXT_MARGIN + RENDERER_TEXT_MARGIN)
#define RENDERER_POINTS_PER_INCH         72   /* font point-to-pixel conversion   */
#define RENDERER_LINE_LEADING_DIVISOR    4    /* extra leading = lineHeight / 4    */
#define RENDERER_CENTER_DIVISOR          2    /* halve a span to centre content    */
#define RENDERER_MINIMUM_DIMENSION       1    /* clamp widths/heights to >= 1 px   */

/* Line-cache growth. */
#define RENDERER_LINE_INITIAL_CAPACITY   1024
#define RENDERER_GROWTH_FACTOR           2

/* CBZ scaling limits. */
#define RENDERER_DEFAULT_SCALE           1.0
#define RENDERER_MINIMUM_SCALE           0.01

/* DIB / WIC pixel layout. */
#define RENDERER_BYTES_PER_PIXEL         4
#define RENDERER_BITS_PER_PIXEL          32
#define RENDERER_BITMAP_PLANES           1
#define RENDERER_FIRST_FRAME_INDEX       0

/*
 * Largest image dimension we will decode. A crafted CBZ could declare an
 * enormous width/height; capping both keeps the stride and total-byte math
 * from overflowing the 32-bit arguments of CreateDIBSection / CopyPixels,
 * which would otherwise lead to an undersized buffer and a heap overflow.
 */
#define RENDERER_MAX_IMAGE_DIMENSION     30000

/* Theme colours. */
#define RENDERER_COLOR_LIGHT_BACKGROUND  RGB(250, 250, 250)
#define RENDERER_COLOR_LIGHT_TEXT        RGB(20, 20, 20)
#define RENDERER_COLOR_SEPIA_BACKGROUND  RGB(245, 235, 210)
#define RENDERER_COLOR_SEPIA_TEXT        RGB(60, 45, 30)
#define RENDERER_COLOR_DARK_BACKGROUND   RGB(24, 24, 24)
#define RENDERER_COLOR_DARK_TEXT         RGB(220, 220, 220)

/* The discoverable welcome hint shown when no document is open. */
static const wchar_t g_welcomeHint[] =
    L"Open an EPUB or CBZ file\n\n"
    L"Press Ctrl+O, use File \x25B8 Open, or drag a file onto this window";

/*---------------------------------------------------------------------------
 * Theme colours
 *--------------------------------------------------------------------------*/

/* Background colour for a theme. */
COLORREF Renderer_BackgroundColor(THEME theme)
{
    COLORREF backgroundColor = RENDERER_COLOR_LIGHT_BACKGROUND;

    if (THEME_SEPIA == theme)
    {
        backgroundColor = RENDERER_COLOR_SEPIA_BACKGROUND;
    }
    else if (THEME_DARK == theme)
    {
        backgroundColor = RENDERER_COLOR_DARK_BACKGROUND;
    }

    return backgroundColor;
}

/* Foreground (text) colour for a theme. */
COLORREF Renderer_TextColor(THEME theme)
{
    COLORREF textColor = RENDERER_COLOR_LIGHT_TEXT;

    if (THEME_SEPIA == theme)
    {
        textColor = RENDERER_COLOR_SEPIA_TEXT;
    }
    else if (THEME_DARK == theme)
    {
        textColor = RENDERER_COLOR_DARK_TEXT;
    }

    return textColor;
}

/* Fills the whole client area of deviceContext with the theme background. */
static void Renderer_FillBackground(struct APP_CONTEXT *applicationContext, HDC deviceContext, RECT *clientRectangle)
{
    HBRUSH backgroundBrush = NULL;

    backgroundBrush = CreateSolidBrush(Renderer_BackgroundColor(applicationContext->theme));
    if (NULL != backgroundBrush)
    {
        (void)FillRect(deviceContext, clientRectangle, backgroundBrush);
        (void)DeleteObject(backgroundBrush);
    }
}

/*---------------------------------------------------------------------------
 * Lifetime
 *--------------------------------------------------------------------------*/

BOOL Renderer_Create(RENDERER **createdRenderer)
{
    RENDERER *renderer = NULL;
    IWICImagingFactory *imagingFactory = NULL;
    HRESULT createResult = E_FAIL;
    BOOL successResult = FALSE;

    if (NULL == createdRenderer)
    {
        goto CLEANUP;
    }
    *createdRenderer = NULL;

    renderer = (RENDERER *)Platform_AllocateZeroed(1, sizeof(RENDERER));
    if (NULL == renderer)
    {
        goto CLEANUP;
    }
    renderer->cachedImageIndex = -1;

    createResult = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&imagingFactory);
    if (FAILED(createResult))
    {
        goto CLEANUP;
    }
    renderer->wicFactory = imagingFactory;
    imagingFactory = NULL;

    *createdRenderer = renderer;
    renderer = NULL;        /* ownership transferred to the caller */
    successResult = TRUE;

CLEANUP:
    SAFE_RELEASE(imagingFactory);
    if (NULL != renderer)
    {
        Renderer_Destroy(renderer);
    }
    return successResult;
}

void Renderer_Destroy(RENDERER *rendererToDestroy)
{
    IWICImagingFactory *imagingFactory = NULL;

    if (NULL == rendererToDestroy)
    {
        goto CLEANUP;
    }

    if (NULL != rendererToDestroy->cachedBitmap)
    {
        (void)DeleteObject(rendererToDestroy->cachedBitmap);
        rendererToDestroy->cachedBitmap = NULL;
    }
    if (NULL != rendererToDestroy->textFont)
    {
        (void)DeleteObject(rendererToDestroy->textFont);
        rendererToDestroy->textFont = NULL;
    }
    SAFE_FREE(rendererToDestroy->layoutLines);

    imagingFactory = (IWICImagingFactory *)rendererToDestroy->wicFactory;
    SAFE_RELEASE(imagingFactory);
    rendererToDestroy->wicFactory = NULL;

    Platform_Free(rendererToDestroy);

CLEANUP:
    return;
}

/*---------------------------------------------------------------------------
 * EPUB text layout
 *--------------------------------------------------------------------------*/

/* Rebuilds the body font from the current settings and measures line height. */
static void Renderer_RebuildFont(struct APP_CONTEXT *applicationContext, HDC deviceContext)
{
    RENDERER    *renderer = NULL;
    HFONT        previousFont = NULL;
    TEXTMETRICW *textMetrics = NULL;
    int          fontHeightPixels = 0;

    renderer = applicationContext->renderer;

    textMetrics = (TEXTMETRICW *)Platform_AllocateZeroed(1, sizeof(TEXTMETRICW));
    if (NULL == textMetrics)
    {
        goto CLEANUP;
    }

    if (NULL != renderer->textFont)
    {
        (void)DeleteObject(renderer->textFont);
        renderer->textFont = NULL;
    }

    fontHeightPixels = -MulDiv(applicationContext->fontPointSize, GetDeviceCaps(deviceContext, LOGPIXELSY), RENDERER_POINTS_PER_INCH);
    renderer->textFont = CreateFontW(fontHeightPixels, 0, 0, 0, FW_NORMAL,
                                     FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FF_ROMAN, L"Georgia");

    previousFont = (HFONT)SelectObject(deviceContext, renderer->textFont);
    (void)GetTextMetricsW(deviceContext, textMetrics);
    (void)SelectObject(deviceContext, previousFont);

    /* Add a quarter-line of leading for comfortable reading. */
    renderer->lineHeight = textMetrics->tmHeight + textMetrics->tmExternalLeading;
    renderer->lineHeight += (renderer->lineHeight / RENDERER_LINE_LEADING_DIVISOR);
    if (RENDERER_MINIMUM_DIMENSION > renderer->lineHeight)
    {
        renderer->lineHeight = RENDERER_MINIMUM_DIMENSION;
    }

CLEANUP:
    SAFE_FREE(textMetrics);
    return;
}

/* Appends one wrapped line to the renderer's line cache. Returns TRUE on success. */
static BOOL Renderer_AppendLine(RENDERER *renderer, unsigned long startOffset, unsigned long characterCount)
{
    TEXT_LINE    *grownLines = NULL;
    unsigned long newCapacity = 0UL;
    BOOL          successResult = FALSE;

    if (renderer->layoutLineCount >= renderer->layoutLineCapacity)
    {
        newCapacity = (0UL == renderer->layoutLineCapacity) ? RENDERER_LINE_INITIAL_CAPACITY : (renderer->layoutLineCapacity * RENDERER_GROWTH_FACTOR);
        grownLines = (TEXT_LINE *)Platform_Reallocate(renderer->layoutLines, newCapacity * sizeof(TEXT_LINE));
        if (NULL == grownLines)
        {
            goto CLEANUP;
        }
        renderer->layoutLines = grownLines;
        renderer->layoutLineCapacity = newCapacity;
    }

    renderer->layoutLines[renderer->layoutLineCount].startOffset = startOffset;
    renderer->layoutLines[renderer->layoutLineCount].characterCount = characterCount;
    renderer->layoutLineCount += 1UL;
    successResult = TRUE;

CLEANUP:
    return successResult;
}

/*
 * Measures the next wrapped line starting at lineStart, reporting how many
 * characters to emit and where the following line begins. All three wrapping
 * cases are decided here so the caller stays a single flat loop. The paragraph
 * and break scans use the length-bounded StrChrNW / StrRChrW rather than
 * hand-rolled loops; every scan is capped by an explicit length, so correctness
 * never depends on a trailing NUL:
 *
 *   - a newline at lineStart is an empty paragraph -> a zero-length line;
 *   - a run that fits the width is emitted whole, consuming the paragraph's
 *     terminating newline so no trailing blank line appears;
 *   - an overlong run breaks at its last space, or, when a single word is wider
 *     than the line, is kept intact and allowed to overflow.
 *
 * Newlines never reach GDI: the measured run is always bounded by its paragraph.
 */
static LINE_BREAK_RESULT Renderer_MeasureNextLine(const TEXT_WRAP_CONTEXT *wrapContext, unsigned long lineStart)
{
    LINE_BREAK_RESULT lineBreak = { 0UL, 0UL };
    const wchar_t    *text = NULL;
    const wchar_t    *lineText = NULL;
    const wchar_t    *newlinePosition = NULL;
    const wchar_t    *breakPosition = NULL;
    unsigned long     paragraphEnd = 0UL;
    unsigned long     wordEnd = 0UL;
    unsigned long     remaining = 0UL;
    int               charactersThatFit = 0;

    text = wrapContext->text;
    lineText = text + lineStart;
    lineBreak.nextStart = lineStart + 1UL;          /* default: step one character */

    /* An empty paragraph: emit a blank line and step over the newline. */
    if (L'\n' == *lineText)
    {
        goto CLEANUP;
    }

    /* Bound the line to its paragraph so no newline enters measurement. The
     * length-bounded StrChrNW cannot read past the text even without a NUL. */
    newlinePosition = StrChrNW(lineText, L'\n', (UINT)(wrapContext->textLength - lineStart));
    paragraphEnd = (NULL != newlinePosition) ? (unsigned long)(newlinePosition - text) : wrapContext->textLength;
    remaining = paragraphEnd - lineStart;

    (void)GetTextExtentExPointW(wrapContext->deviceContext, lineText, (int)remaining, wrapContext->availableWidth, &charactersThatFit, NULL, wrapContext->measureSize);

    /* The whole remainder fits; consume the paragraph's terminating newline. */
    if ((unsigned long)charactersThatFit >= remaining)
    {
        lineBreak.lineLength = remaining;
        lineBreak.nextStart = (NULL != newlinePosition) ? (paragraphEnd + 1UL) : paragraphEnd;
        goto CLEANUP;
    }

    /* Prefer the last space within the fitting run so whole words stay together. */
    breakPosition = StrRChrW(lineText, lineText + charactersThatFit + 1, L' ');
    if (NULL != breakPosition)
    {
        lineBreak.lineLength = (unsigned long)(breakPosition - lineText);    /* drop the space */
        lineBreak.nextStart = (unsigned long)(breakPosition - text) + 1UL;  /* skip the space */
        goto CLEANUP;
    }

    /* A single word wider than the line: keep it whole and let it overflow,
     * extending to the next space or the paragraph end, whichever comes first.
     * Bounding the search to the paragraph removes the need for a clamp. */
    breakPosition = StrChrNW(lineText + charactersThatFit, L' ',
                             (UINT)(remaining - (unsigned long)charactersThatFit));
    wordEnd = (NULL != breakPosition) ? (unsigned long)(breakPosition - text) : paragraphEnd;
    lineBreak.lineLength = wordEnd - lineStart;
    lineBreak.nextStart = (wordEnd < wrapContext->textLength) ? (wordEnd + 1UL) : wordEnd;

CLEANUP:
    return lineBreak;
}

/* Word-wraps the whole document into the line cache. Returns the page count. */
static int Renderer_LayoutText(struct APP_CONTEXT *applicationContext)
{
    RENDERER          *renderer = NULL;
    DOCUMENT          *document = NULL;
    HDC                deviceContext = NULL;
    HFONT              previousFont = NULL;
    RECT              *clientRectangle = NULL;
    TEXT_WRAP_CONTEXT *wrapContext = NULL;
    const wchar_t     *text = NULL;
    unsigned long      textLength = 0UL;
    unsigned long      position = 0UL;
    int                usableHeight = 0;
    int                pageCount = 1;

    renderer = applicationContext->renderer;
    document = applicationContext->document;
    text = document->combinedText;
    textLength = document->combinedTextLength;

    clientRectangle = (RECT *)Platform_AllocateZeroed(1, sizeof(RECT));
    wrapContext = (TEXT_WRAP_CONTEXT *)Platform_AllocateZeroed(1, sizeof(TEXT_WRAP_CONTEXT));
    if ((NULL == clientRectangle) || (NULL == wrapContext))
    {
        goto CLEANUP;
    }
    wrapContext->measureSize = (SIZE *)Platform_AllocateZeroed(1, sizeof(SIZE));
    if (NULL == wrapContext->measureSize)
    {
        goto CLEANUP;
    }

    renderer->layoutLineCount = 0UL;

    deviceContext = GetDC(applicationContext->windowHandle);
    if (NULL == deviceContext)
    {
        goto CLEANUP;
    }
    Renderer_RebuildFont(applicationContext, deviceContext);
    previousFont = (HFONT)SelectObject(deviceContext, renderer->textFont);

    (void)GetClientRect(applicationContext->windowHandle, clientRectangle);
    wrapContext->deviceContext = deviceContext;
    wrapContext->text = text;
    wrapContext->textLength = textLength;
    wrapContext->availableWidth = (clientRectangle->right - clientRectangle->left) - RENDERER_TOTAL_HORIZONTAL_MARGIN;
    if (RENDERER_MINIMUM_DIMENSION > wrapContext->availableWidth)
    {
        wrapContext->availableWidth = RENDERER_MINIMUM_DIMENSION;
    }

    /* Greedy word wrap: each pass emits one line and reports where the next
     * begins, so the whole document wraps in a single flat loop. */
    position = 0UL;
    while (position < textLength)
    {
        LINE_BREAK_RESULT lineBreak = Renderer_MeasureNextLine(wrapContext, position);

        (void)Renderer_AppendLine(renderer, position, lineBreak.lineLength);
        position = lineBreak.nextStart;
    }

    /* How many wrapped lines fit on a page. */
    usableHeight = (clientRectangle->bottom - clientRectangle->top) - RENDERER_TOTAL_VERTICAL_MARGIN;
    renderer->linesPerPage = usableHeight / renderer->lineHeight;
    if (RENDERER_MINIMUM_DIMENSION > renderer->linesPerPage)
    {
        renderer->linesPerPage = RENDERER_MINIMUM_DIMENSION;
    }

    pageCount = (int)((renderer->layoutLineCount + (unsigned long)renderer->linesPerPage - 1UL) / (unsigned long)renderer->linesPerPage);
    if (RENDERER_MINIMUM_DIMENSION > pageCount)
    {
        pageCount = RENDERER_MINIMUM_DIMENSION;
    }

CLEANUP:
    if (NULL != deviceContext)
    {
        if (NULL != previousFont)
        {
            (void)SelectObject(deviceContext, previousFont);
        }
        (void)ReleaseDC(applicationContext->windowHandle, deviceContext);
    }
    if (NULL != wrapContext)
    {
        SAFE_FREE(wrapContext->measureSize);
    }
    SAFE_FREE(wrapContext);
    SAFE_FREE(clientRectangle);
    return pageCount;
}

/* Paints the current page of EPUB text. */
static void Renderer_PaintText(struct APP_CONTEXT *applicationContext, HDC deviceContext)
{
    RENDERER     *renderer = NULL;
    DOCUMENT     *document = NULL;
    HFONT         previousFont = NULL;
    RECT         *clientRectangle = NULL;
    unsigned long firstLineIndex = 0UL;
    unsigned long lineIndex = 0UL;
    int           verticalPosition = 0;

    renderer = applicationContext->renderer;
    document = applicationContext->document;

    clientRectangle = (RECT *)Platform_AllocateZeroed(1, sizeof(RECT));
    if (NULL == clientRectangle)
    {
        goto CLEANUP;
    }

    (void)GetClientRect(applicationContext->windowHandle, clientRectangle);
    Renderer_FillBackground(applicationContext, deviceContext, clientRectangle);

    if ((NULL == renderer->textFont) || (NULL == renderer->layoutLines))
    {
        goto CLEANUP;
    }

    previousFont = (HFONT)SelectObject(deviceContext, renderer->textFont);
    (void)SetBkMode(deviceContext, TRANSPARENT);
    (void)SetTextColor(deviceContext, Renderer_TextColor(applicationContext->theme));

    firstLineIndex = (unsigned long)applicationContext->currentPageIndex * (unsigned long)renderer->linesPerPage;
    verticalPosition = RENDERER_TEXT_MARGIN;

    for (lineIndex = firstLineIndex; (lineIndex < renderer->layoutLineCount) && (lineIndex < (firstLineIndex + (unsigned long)renderer->linesPerPage)); lineIndex += 1UL)
    {
        if (0UL != renderer->layoutLines[lineIndex].characterCount)
        {
            (void)TextOutW(deviceContext, RENDERER_TEXT_MARGIN, verticalPosition, document->combinedText + renderer->layoutLines[lineIndex].startOffset, (int)renderer->layoutLines[lineIndex].characterCount);
        }
        verticalPosition += renderer->lineHeight;
    }

    (void)SelectObject(deviceContext, previousFont);

CLEANUP:
    SAFE_FREE(clientRectangle);
    return;
}

/*---------------------------------------------------------------------------
 * CBZ image decoding and painting
 *--------------------------------------------------------------------------*/

/* Decodes the page image into a 32bpp DIB cached on the renderer. Returns TRUE on success. */
static BOOL Renderer_DecodeImage(struct APP_CONTEXT *applicationContext, int imageIndex)
{
    RENDERER             *renderer = NULL;
    DOCUMENT             *document = NULL;
    IWICImagingFactory   *imagingFactory = NULL;
    IWICStream           *imageStream = NULL;
    IWICBitmapDecoder    *imageDecoder = NULL;
    IWICBitmapFrameDecode *imageFrame = NULL;
    IWICFormatConverter  *formatConverter = NULL;
    ARCHIVE_ENTRY        *imageEntry = NULL;
    ARCHIVE_BUFFER       *imageBuffer = NULL;
    BITMAPINFO           *bitmapInformation = NULL;
    HBITMAP               deviceBitmap = NULL;
    void                 *bitmapBits = NULL;
    UINT                  imageWidth = 0;
    UINT                  imageHeight = 0;
    UINT64                bytesPerRow = 0;
    UINT64                totalByteCount = 0;
    HRESULT               operationResult = E_FAIL;
    BOOL                  successResult = FALSE;

    renderer = applicationContext->renderer;
    document = applicationContext->document;
    imagingFactory = (IWICImagingFactory *)renderer->wicFactory;

    if (renderer->cachedImageIndex == imageIndex)
    {
        successResult = TRUE;       /* already decoded */
        goto CLEANUP;
    }

    imageBuffer = (ARCHIVE_BUFFER *)Platform_AllocateZeroed(1, sizeof(ARCHIVE_BUFFER));
    bitmapInformation = (BITMAPINFO *)Platform_AllocateZeroed(1, sizeof(BITMAPINFO));
    if ((NULL == imageBuffer) || (NULL == bitmapInformation))
    {
        goto CLEANUP;
    }

    imageEntry = Archive_FindEntry(document->backingArchive, document->imageEntryNames[imageIndex]);
    if (NULL == imageEntry)
    {
        goto CLEANUP;
    }
    if (FALSE == Archive_ExtractEntry(document->backingArchive, imageEntry, imageBuffer))
    {
        goto CLEANUP;
    }

    operationResult = IWICImagingFactory_CreateStream(imagingFactory, &imageStream);
    if (FAILED(operationResult))
    {
        goto CLEANUP;
    }
    operationResult = IWICStream_InitializeFromMemory(imageStream, imageBuffer->data, (DWORD)imageBuffer->size);
    if (FAILED(operationResult))
    {
        goto CLEANUP;
    }

    operationResult = IWICImagingFactory_CreateDecoderFromStream(imagingFactory, (IStream *)imageStream, NULL, WICDecodeMetadataCacheOnLoad, &imageDecoder);
    if (FAILED(operationResult))
    {
        goto CLEANUP;
    }
    operationResult = IWICBitmapDecoder_GetFrame(imageDecoder, RENDERER_FIRST_FRAME_INDEX, &imageFrame);
    if (FAILED(operationResult))
    {
        goto CLEANUP;
    }

    operationResult = IWICImagingFactory_CreateFormatConverter(imagingFactory, &formatConverter);
    if (FAILED(operationResult))
    {
        goto CLEANUP;
    }
    operationResult = IWICFormatConverter_Initialize(formatConverter, (IWICBitmapSource *)imageFrame, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(operationResult))
    {
        goto CLEANUP;
    }

    operationResult = IWICFormatConverter_GetSize(formatConverter, &imageWidth, &imageHeight);
    if (FAILED(operationResult) || (0 == imageWidth) || (0 == imageHeight))
    {
        goto CLEANUP;
    }

    /* Bound the dimensions, then size the buffer in 64-bit so a malicious
     * image cannot overflow the 32-bit stride/byte-count arguments below. */
    if ((imageWidth > RENDERER_MAX_IMAGE_DIMENSION) || (imageHeight > RENDERER_MAX_IMAGE_DIMENSION))
    {
        goto CLEANUP;
    }
    bytesPerRow = (UINT64)imageWidth * RENDERER_BYTES_PER_PIXEL;
    totalByteCount = bytesPerRow * (UINT64)imageHeight;
    if (totalByteCount > MAXDWORD)
    {
        goto CLEANUP;
    }

    /* Top-down 32bpp DIB so WIC's row order maps straight into the bits. */
    bitmapInformation->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInformation->bmiHeader.biWidth = (LONG)imageWidth;
    bitmapInformation->bmiHeader.biHeight = -(LONG)imageHeight;
    bitmapInformation->bmiHeader.biPlanes = RENDERER_BITMAP_PLANES;
    bitmapInformation->bmiHeader.biBitCount = RENDERER_BITS_PER_PIXEL;
    bitmapInformation->bmiHeader.biCompression = BI_RGB;

    deviceBitmap = CreateDIBSection(NULL, bitmapInformation, DIB_RGB_COLORS, &bitmapBits, NULL, 0);
    if ((NULL == deviceBitmap) || (NULL == bitmapBits))
    {
        goto CLEANUP;
    }

    operationResult = IWICFormatConverter_CopyPixels(formatConverter, NULL, (UINT)bytesPerRow, (UINT)totalByteCount, (BYTE *)bitmapBits);
    if (FAILED(operationResult))
    {
        goto CLEANUP;
    }

    /* Replace the cache. */
    if (NULL != renderer->cachedBitmap)
    {
        (void)DeleteObject(renderer->cachedBitmap);
    }
    renderer->cachedBitmap = deviceBitmap;
    renderer->cachedBitmapWidth = (int)imageWidth;
    renderer->cachedBitmapHeight = (int)imageHeight;
    renderer->cachedImageIndex = imageIndex;
    deviceBitmap = NULL;        /* ownership moved into the cache */
    successResult = TRUE;

CLEANUP:
    if (NULL != deviceBitmap)
    {
        (void)DeleteObject(deviceBitmap);
    }
    SAFE_RELEASE(formatConverter);
    SAFE_RELEASE(imageFrame);
    SAFE_RELEASE(imageDecoder);
    SAFE_RELEASE(imageStream);
    SAFE_FREE(bitmapInformation);
    if (NULL != imageBuffer)
    {
        SAFE_FREE(imageBuffer->data);
        SAFE_FREE(imageBuffer);
    }
    return successResult;
}

/* Computes the base fit scale for the current fit mode and image/viewport sizes. */
static double Renderer_FitScale(struct APP_CONTEXT *applicationContext, int clientWidth, int clientHeight)
{
    RENDERER *renderer = NULL;
    double    fitScale = RENDERER_DEFAULT_SCALE;

    renderer = applicationContext->renderer;

    if (FIT_MODE_WIDTH == applicationContext->fitMode)
    {
        fitScale = (double)clientWidth / (double)renderer->cachedBitmapWidth;
    }
    else if (FIT_MODE_HEIGHT == applicationContext->fitMode)
    {
        fitScale = (double)clientHeight / (double)renderer->cachedBitmapHeight;
    }
    else if (FIT_MODE_ORIGINAL == applicationContext->fitMode)
    {
        fitScale = RENDERER_DEFAULT_SCALE;
    }
    else /* FIT_MODE_PAGE */
    {
        double widthScale = 0.0;
        double heightScale = 0.0;
        widthScale = (double)clientWidth / (double)renderer->cachedBitmapWidth;
        heightScale = (double)clientHeight / (double)renderer->cachedBitmapHeight;
        fitScale = (widthScale < heightScale) ? widthScale : heightScale;
    }

    return fitScale;
}

/* Paints the current CBZ page image with the active fit mode, zoom, and pan. */
static void Renderer_PaintImage(struct APP_CONTEXT *applicationContext, HDC deviceContext)
{
    RENDERER *renderer = NULL;
    HDC       memoryDeviceContext = NULL;
    HBITMAP   previousBitmap = NULL;
    RECT     *clientRectangle = NULL;
    double    finalScale = RENDERER_DEFAULT_SCALE;
    int       clientWidth = 0;
    int       clientHeight = 0;
    int       scaledWidth = 0;
    int       scaledHeight = 0;
    int       destinationX = 0;
    int       destinationY = 0;

    renderer = applicationContext->renderer;

    clientRectangle = (RECT *)Platform_AllocateZeroed(1, sizeof(RECT));
    if (NULL == clientRectangle)
    {
        goto CLEANUP;
    }

    (void)GetClientRect(applicationContext->windowHandle, clientRectangle);
    clientWidth = clientRectangle->right - clientRectangle->left;
    clientHeight = clientRectangle->bottom - clientRectangle->top;

    Renderer_FillBackground(applicationContext, deviceContext, clientRectangle);

    if (FALSE == Renderer_DecodeImage(applicationContext, applicationContext->currentPageIndex))
    {
        goto CLEANUP;
    }

    finalScale = Renderer_FitScale(applicationContext, clientWidth, clientHeight) * applicationContext->zoomFactor;
    if (RENDERER_MINIMUM_SCALE > finalScale)
    {
        finalScale = RENDERER_MINIMUM_SCALE;
    }
    scaledWidth = (int)((double)renderer->cachedBitmapWidth * finalScale);
    scaledHeight = (int)((double)renderer->cachedBitmapHeight * finalScale);
    if (RENDERER_MINIMUM_DIMENSION > scaledWidth)
    {
        scaledWidth = RENDERER_MINIMUM_DIMENSION;
    }
    if (RENDERER_MINIMUM_DIMENSION > scaledHeight)
    {
        scaledHeight = RENDERER_MINIMUM_DIMENSION;
    }

    /* Centre when the image is smaller than the viewport; otherwise apply pan. */
    destinationX = (clientWidth - scaledWidth) / RENDERER_CENTER_DIVISOR;
    if (scaledWidth > clientWidth)
    {
        destinationX += applicationContext->panOffsetX;
    }
    destinationY = (clientHeight - scaledHeight) / RENDERER_CENTER_DIVISOR;
    if (scaledHeight > clientHeight)
    {
        destinationY += applicationContext->panOffsetY;
    }

    memoryDeviceContext = CreateCompatibleDC(deviceContext);
    if (NULL == memoryDeviceContext)
    {
        goto CLEANUP;
    }
    previousBitmap = (HBITMAP)SelectObject(memoryDeviceContext, renderer->cachedBitmap);

    (void)SetStretchBltMode(deviceContext, HALFTONE);
    (void)SetBrushOrgEx(deviceContext, 0, 0, NULL);
    (void)StretchBlt(deviceContext, destinationX, destinationY, scaledWidth, scaledHeight, memoryDeviceContext, 0, 0, renderer->cachedBitmapWidth, renderer->cachedBitmapHeight, SRCCOPY);

    (void)SelectObject(memoryDeviceContext, previousBitmap);

CLEANUP:
    if (NULL != memoryDeviceContext)
    {
        (void)DeleteDC(memoryDeviceContext);
    }
    SAFE_FREE(clientRectangle);
    return;
}

/* Paints the welcome hint shown when no document is loaded. */
static void Renderer_PaintWelcome(struct APP_CONTEXT *applicationContext, HDC deviceContext)
{
    HFONT previousFont = NULL;
    RECT *clientRectangle = NULL;
    RECT *measureRectangle = NULL;
    RECT *drawRectangle = NULL;
    int   textHeight = 0;

    clientRectangle = (RECT *)Platform_AllocateZeroed(1, sizeof(RECT));
    measureRectangle = (RECT *)Platform_AllocateZeroed(1, sizeof(RECT));
    drawRectangle = (RECT *)Platform_AllocateZeroed(1, sizeof(RECT));
    if ((NULL == clientRectangle) || (NULL == measureRectangle) || (NULL == drawRectangle))
    {
        goto CLEANUP;
    }

    (void)GetClientRect(applicationContext->windowHandle, clientRectangle);
    Renderer_FillBackground(applicationContext, deviceContext, clientRectangle);

    *measureRectangle = *clientRectangle;
    previousFont = (HFONT)SelectObject(deviceContext, GetStockObject(DEFAULT_GUI_FONT));
    (void)SetBkMode(deviceContext, TRANSPARENT);
    (void)SetTextColor(deviceContext, Renderer_TextColor(applicationContext->theme));

    /* Measure the wrapped height, then draw vertically centred. */
    (void)DrawTextW(deviceContext, g_welcomeHint, -1, measureRectangle, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    textHeight = measureRectangle->bottom - measureRectangle->top;
    *drawRectangle = *clientRectangle;
    drawRectangle->top = ((clientRectangle->bottom - clientRectangle->top) - textHeight) / RENDERER_CENTER_DIVISOR;
    (void)DrawTextW(deviceContext, g_welcomeHint, -1, drawRectangle, DT_CENTER | DT_WORDBREAK);
    (void)SelectObject(deviceContext, previousFont);

CLEANUP:
    SAFE_FREE(drawRectangle);
    SAFE_FREE(measureRectangle);
    SAFE_FREE(clientRectangle);
    return;
}

/*---------------------------------------------------------------------------
 * Public interface
 *--------------------------------------------------------------------------*/

int Renderer_Layout(struct APP_CONTEXT *applicationContext)
{
    int pageCount = 1;

    if ((NULL == applicationContext) || (NULL == applicationContext->document))
    {
        goto CLEANUP;
    }

    if (DOCUMENT_TYPE_EPUB == applicationContext->document->documentType)
    {
        pageCount = Renderer_LayoutText(applicationContext);
    }
    else if (DOCUMENT_TYPE_CBZ == applicationContext->document->documentType)
    {
        pageCount = (int)applicationContext->document->imageCount;
        if (RENDERER_MINIMUM_DIMENSION > pageCount)
        {
            pageCount = RENDERER_MINIMUM_DIMENSION;
        }
    }

CLEANUP:
    return pageCount;
}

void Renderer_Paint(struct APP_CONTEXT *applicationContext, HDC deviceContext)
{
    if ((NULL == applicationContext) || (NULL == deviceContext))
    {
        goto CLEANUP;
    }

    if (NULL == applicationContext->document)
    {
        Renderer_PaintWelcome(applicationContext, deviceContext);
        goto CLEANUP;
    }

    if (DOCUMENT_TYPE_EPUB == applicationContext->document->documentType)
    {
        Renderer_PaintText(applicationContext, deviceContext);
    }
    else if (DOCUMENT_TYPE_CBZ == applicationContext->document->documentType)
    {
        Renderer_PaintImage(applicationContext, deviceContext);
    }

CLEANUP:
    return;
}
