/*****************************************************************************
 * application.c
 *
 * Application core. Owns the single APP_CONTEXT, drives the message loop, and
 * implements every reading command routed in from the window procedure. This
 * is where all of the actual behaviour lives; main.c merely calls App_Run.
 *****************************************************************************/

#include "application.h"
#include "window.h"
#include "renderer.h"
#include "document.h"

#include <objbase.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wchar.h>

/* Reading-setting limits and defaults. */
#define APP_MINIMUM_FONT_POINTS 8
#define APP_MAXIMUM_FONT_POINTS 48
#define APP_DEFAULT_FONT_POINTS 16
#define APP_MINIMUM_ZOOM        0.10
#define APP_MAXIMUM_ZOOM        8.00
#define APP_DEFAULT_ZOOM        1.00
#define APP_THEME_COUNT         3

/* Title-bar buffer sizing. */
#define APP_TITLE_EXTRA_CHARS   64
#define APP_TITLE_BUFFER_LENGTH (MAX_PATH + APP_TITLE_EXTRA_CHARS)

/* Display uses 1-based page numbers; storage is 0-based. */
#define APP_PAGE_DISPLAY_BIAS   1

/* Command-line: argv[0] is the program, argv[1] (if present) is the file. */
#define APP_MINIMUM_ARGS_WITH_FILE 2
#define APP_FILE_ARGUMENT_INDEX    1

/* Updates the title bar to reflect the open file and current page. */
static void App_UpdateTitle(APP_CONTEXT *applicationContext)
{
    wchar_t       *titleText = NULL;
    const wchar_t *fileName = NULL;

    if (NULL == applicationContext->document)
    {
        (void)SetWindowTextW(applicationContext->windowHandle, L"Windows E-Reader");
        goto CLEANUP;
    }

    titleText = (wchar_t *)Platform_AllocateZeroed(APP_TITLE_BUFFER_LENGTH, sizeof(wchar_t));
    if (NULL == titleText)
    {
        goto CLEANUP;
    }

    fileName = PathFindFileNameW(applicationContext->currentFilePath);
    (void)wsprintfW(titleText,
                    L"%s  \x2014  Page %d / %d  \x2014  Windows E-Reader",
                    fileName,
                    applicationContext->currentPageIndex + APP_PAGE_DISPLAY_BIAS,
                    applicationContext->totalPageCount);
    (void)SetWindowTextW(applicationContext->windowHandle, titleText);

CLEANUP:
    SAFE_FREE(titleText);
    return;
}

void App_Relayout(APP_CONTEXT *applicationContext)
{
    if ((NULL == applicationContext) || (NULL == applicationContext->document))
    {
        goto CLEANUP;
    }

    applicationContext->totalPageCount = Renderer_Layout(applicationContext);
    if (applicationContext->currentPageIndex >= applicationContext->totalPageCount)
    {
        applicationContext->currentPageIndex = applicationContext->totalPageCount - 1;
    }
    if (0 > applicationContext->currentPageIndex)
    {
        applicationContext->currentPageIndex = 0;
    }

    (void)InvalidateRect(applicationContext->windowHandle, NULL, FALSE);
    App_UpdateTitle(applicationContext);

CLEANUP:
    return;
}

void App_GoToPage(APP_CONTEXT *applicationContext, int targetPageIndex)
{
    if ((NULL == applicationContext) || (NULL == applicationContext->document))
    {
        goto CLEANUP;
    }

    if (0 > targetPageIndex)
    {
        targetPageIndex = 0;
    }
    if (targetPageIndex >= applicationContext->totalPageCount)
    {
        targetPageIndex = applicationContext->totalPageCount - 1;
    }

    applicationContext->currentPageIndex = targetPageIndex;
    applicationContext->panOffsetX = 0;     /* a fresh page starts un-panned */
    applicationContext->panOffsetY = 0;

    (void)InvalidateRect(applicationContext->windowHandle, NULL, FALSE);
    App_UpdateTitle(applicationContext);

CLEANUP:
    return;
}

void App_NextPage(APP_CONTEXT *applicationContext)
{
    if (NULL == applicationContext)
    {
        goto CLEANUP;
    }
    App_GoToPage(applicationContext, applicationContext->currentPageIndex + 1);

CLEANUP:
    return;
}

void App_PreviousPage(APP_CONTEXT *applicationContext)
{
    if (NULL == applicationContext)
    {
        goto CLEANUP;
    }
    App_GoToPage(applicationContext, applicationContext->currentPageIndex - 1);

CLEANUP:
    return;
}

void App_ScrollBy(APP_CONTEXT *applicationContext, int deltaUnits)
{
    if (NULL == applicationContext)
    {
        goto CLEANUP;
    }

    if (0 < deltaUnits)
    {
        App_NextPage(applicationContext);
    }
    else if (0 > deltaUnits)
    {
        App_PreviousPage(applicationContext);
    }

CLEANUP:
    return;
}

void App_Pan(APP_CONTEXT *applicationContext, int deltaX, int deltaY)
{
    if ((NULL == applicationContext) || (NULL == applicationContext->document))
    {
        goto CLEANUP;
    }

    applicationContext->panOffsetX += deltaX;
    applicationContext->panOffsetY += deltaY;
    (void)InvalidateRect(applicationContext->windowHandle, NULL, FALSE);

CLEANUP:
    return;
}

void App_SetFitMode(APP_CONTEXT *applicationContext, FIT_MODE newFitMode)
{
    if (NULL == applicationContext)
    {
        goto CLEANUP;
    }

    applicationContext->fitMode = newFitMode;
    applicationContext->panOffsetX = 0;
    applicationContext->panOffsetY = 0;
    (void)InvalidateRect(applicationContext->windowHandle, NULL, FALSE);

CLEANUP:
    return;
}

void App_CycleTheme(APP_CONTEXT *applicationContext)
{
    if (NULL == applicationContext)
    {
        goto CLEANUP;
    }

    applicationContext->theme = (THEME)(((int)applicationContext->theme + 1) % APP_THEME_COUNT);
    Window_ApplyTheme(applicationContext);      /* recolour the title and menu bars */
    (void)InvalidateRect(applicationContext->windowHandle, NULL, FALSE);

CLEANUP:
    return;
}

void App_AdjustFontSize(APP_CONTEXT *applicationContext, int pointDelta)
{
    if ((NULL == applicationContext) || (NULL == applicationContext->document))
    {
        goto CLEANUP;
    }

    applicationContext->fontPointSize += pointDelta;
    if (APP_MINIMUM_FONT_POINTS > applicationContext->fontPointSize)
    {
        applicationContext->fontPointSize = APP_MINIMUM_FONT_POINTS;
    }
    if (APP_MAXIMUM_FONT_POINTS < applicationContext->fontPointSize)
    {
        applicationContext->fontPointSize = APP_MAXIMUM_FONT_POINTS;
    }

    /* Changing the font re-flows the text, so re-paginate. */
    App_Relayout(applicationContext);

CLEANUP:
    return;
}

void App_AdjustZoom(APP_CONTEXT *applicationContext, double zoomDelta)
{
    if ((NULL == applicationContext) || (NULL == applicationContext->document))
    {
        goto CLEANUP;
    }

    applicationContext->zoomFactor += zoomDelta;
    if (APP_MINIMUM_ZOOM > applicationContext->zoomFactor)
    {
        applicationContext->zoomFactor = APP_MINIMUM_ZOOM;
    }
    if (APP_MAXIMUM_ZOOM < applicationContext->zoomFactor)
    {
        applicationContext->zoomFactor = APP_MAXIMUM_ZOOM;
    }
    (void)InvalidateRect(applicationContext->windowHandle, NULL, FALSE);

CLEANUP:
    return;
}

void App_ToggleFullscreen(APP_CONTEXT *applicationContext)
{
    DWORD        windowStyle = 0;
    MONITORINFO *monitorInfo = NULL;

    if (NULL == applicationContext)
    {
        goto CLEANUP;
    }

    monitorInfo = (MONITORINFO *)Platform_AllocateZeroed(1, sizeof(MONITORINFO));
    if (NULL == monitorInfo)
    {
        goto CLEANUP;
    }
    monitorInfo->cbSize = sizeof(MONITORINFO);
    applicationContext->previousPlacement.length = sizeof(applicationContext->previousPlacement);

    windowStyle = (DWORD)GetWindowLongW(applicationContext->windowHandle, GWL_STYLE);
    if (0 != (windowStyle & WS_OVERLAPPEDWINDOW))
    {
        HMONITOR monitorHandle = NULL;
        monitorHandle = MonitorFromWindow(applicationContext->windowHandle, MONITOR_DEFAULTTOPRIMARY);

        if ((0 != GetWindowPlacement(applicationContext->windowHandle, &applicationContext->previousPlacement)) && (0 != GetMonitorInfoW(monitorHandle, monitorInfo)))
        {
            (void)SetWindowLongW(applicationContext->windowHandle, GWL_STYLE, windowStyle & ~WS_OVERLAPPEDWINDOW);
            (void)SetWindowPos(applicationContext->windowHandle, HWND_TOP,
                               monitorInfo->rcMonitor.left, monitorInfo->rcMonitor.top,
                               monitorInfo->rcMonitor.right - monitorInfo->rcMonitor.left,
                               monitorInfo->rcMonitor.bottom - monitorInfo->rcMonitor.top,
                               SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            applicationContext->isFullscreen = TRUE;
        }
    }
    else
    {
        (void)SetWindowLongW(applicationContext->windowHandle, GWL_STYLE, windowStyle | WS_OVERLAPPEDWINDOW);
        (void)SetWindowPlacement(applicationContext->windowHandle,&applicationContext->previousPlacement);
        (void)SetWindowPos(applicationContext->windowHandle, NULL, 0, 0, 0, 0, 
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                           SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        applicationContext->isFullscreen = FALSE;
    }

CLEANUP:
    SAFE_FREE(monitorInfo);
    return;
}

void App_GoToChapter(APP_CONTEXT *applicationContext, int direction)
{
    RENDERER     *renderer = NULL;
    DOCUMENT     *document = NULL;
    unsigned long firstLineIndex = 0UL;
    unsigned long currentOffset = 0UL;
    unsigned long lineIndex = 0UL;
    unsigned long targetOffset = 0UL;
    unsigned long targetLineIndex = 0UL;
    int           currentChapter = 0;
    int           targetChapter = 0;
    unsigned long chapterIndex = 0UL;

    if ((NULL == applicationContext) || (NULL == applicationContext->document))
    {
        goto CLEANUP;
    }
    document = applicationContext->document;
    renderer = applicationContext->renderer;
    if ((DOCUMENT_TYPE_EPUB != document->documentType) ||
        (0UL == document->chapterCount) || (0UL == renderer->layoutLineCount))
    {
        goto CLEANUP;
    }

    firstLineIndex = (unsigned long)applicationContext->currentPageIndex *
                     (unsigned long)renderer->linesPerPage;
    if (firstLineIndex < renderer->layoutLineCount)
    {
        currentOffset = renderer->layoutLines[firstLineIndex].startOffset;
    }

    /* Locate the chapter that currently contains the top of the page. */
    for (chapterIndex = 0UL; chapterIndex < document->chapterCount; chapterIndex += 1UL)
    {
        if (document->chapters[chapterIndex].startCharacterOffset <= currentOffset)
        {
            currentChapter = (int)chapterIndex;
        }
        else
        {
            break;
        }
    }

    targetChapter = currentChapter + direction;
    if (0 > targetChapter)
    {
        targetChapter = 0;
    }
    if (targetChapter >= (int)document->chapterCount)
    {
        targetChapter = (int)document->chapterCount - 1;
    }
    targetOffset = document->chapters[targetChapter].startCharacterOffset;

    for (lineIndex = 0UL; lineIndex < renderer->layoutLineCount; lineIndex += 1UL)
    {
        if (renderer->layoutLines[lineIndex].startOffset >= targetOffset)
        {
            targetLineIndex = lineIndex;
            break;
        }
    }

    App_GoToPage(applicationContext, (int)(targetLineIndex / (unsigned long)renderer->linesPerPage));

CLEANUP:
    return;
}

BOOL App_OpenDocument(APP_CONTEXT *applicationContext, const wchar_t *documentFilePath)
{
    DOCUMENT *newDocument = NULL;
    BOOL      successResult = FALSE;

    if ((NULL == applicationContext) || (NULL == documentFilePath))
    {
        goto CLEANUP;
    }

    if (FALSE == Document_Open(documentFilePath, &newDocument))
    {
        (void)MessageBoxW(applicationContext->windowHandle,
                          L"This file could not be opened. Supported formats are EPUB and CBZ.",
                          L"Windows E-Reader", MB_OK | MB_ICONWARNING);
        goto CLEANUP;
    }

    /* Replace any previously open document. */
    if (NULL != applicationContext->document)
    {
        Document_Close(applicationContext->document);
        applicationContext->document = NULL;
    }
    applicationContext->document = newDocument;
    newDocument = NULL;

    (void)Platform_CopyW(applicationContext->currentFilePath, EREADER_ARRAY_LENGTH(applicationContext->currentFilePath),
                         documentFilePath);

    applicationContext->currentPageIndex = 0;
    applicationContext->zoomFactor = APP_DEFAULT_ZOOM;
    applicationContext->panOffsetX = 0;
    applicationContext->panOffsetY = 0;
    if (NULL != applicationContext->renderer)
    {
        applicationContext->renderer->cachedImageIndex = -1;    /* invalidate image cache */
    }

    App_Relayout(applicationContext);
    successResult = TRUE;

CLEANUP:
    if (NULL != newDocument)
    {
        Document_Close(newDocument);
    }
    return successResult;
}

void App_PromptOpenDocument(APP_CONTEXT *applicationContext)
{
    OPENFILENAMEW *openFileName = NULL;
    wchar_t       *selectedPath = NULL;

    if (NULL == applicationContext)
    {
        goto CLEANUP;
    }

    openFileName = (OPENFILENAMEW *)Platform_AllocateZeroed(1, sizeof(OPENFILENAMEW));
    selectedPath = (wchar_t *)Platform_AllocateZeroed(MAX_PATH, sizeof(wchar_t));
    if ((NULL == openFileName) || (NULL == selectedPath))
    {
        goto CLEANUP;
    }

    openFileName->lStructSize = sizeof(OPENFILENAMEW);
    openFileName->hwndOwner = applicationContext->windowHandle;
    openFileName->lpstrFile = selectedPath;
    openFileName->nMaxFile = MAX_PATH;
    openFileName->lpstrFilter =
        L"E-Book Files (*.epub;*.cbz)\0*.epub;*.cbz\0EPUB Files (*.epub)\0*.epub\0"
        L"Comic Archives (*.cbz)\0*.cbz\0All Files (*.*)\0*.*\0";
    openFileName->nFilterIndex = 1;
    openFileName->Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (0 != GetOpenFileNameW(openFileName))
    {
        (void)App_OpenDocument(applicationContext, selectedPath);
    }

CLEANUP:
    SAFE_FREE(selectedPath);
    SAFE_FREE(openFileName);
    return;
}

/* Opens a file named on the command line, if any. */
static void App_OpenFromCommandLine(APP_CONTEXT *applicationContext)
{
    wchar_t **argumentList = NULL;
    int       argumentCount = 0;

    argumentList = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (NULL == argumentList)
    {
        goto CLEANUP;
    }
    if (APP_MINIMUM_ARGS_WITH_FILE <= argumentCount)
    {
        (void)App_OpenDocument(applicationContext, argumentList[APP_FILE_ARGUMENT_INDEX]);
    }

CLEANUP:
    if (NULL != argumentList)
    {
        (void)LocalFree(argumentList);
    }
    return;
}

int App_Run(HINSTANCE instanceHandle, const wchar_t *commandLine)
{
    APP_CONTEXT *applicationContext = NULL;
    MSG         *message = NULL;
    HRESULT      comInitResult = E_FAIL;
    BOOL         comInitialized = FALSE;
    int          exitCode = 0;

    UNREFERENCED_PARAMETER(commandLine);

    /* The renderer's WIC factory and the open dialog both require COM. */
    comInitResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(comInitResult))
    {
        goto CLEANUP;
    }
    comInitialized = TRUE;

    applicationContext = (APP_CONTEXT *)Platform_AllocateZeroed(1, sizeof(APP_CONTEXT));
    message = (MSG *)Platform_AllocateZeroed(1, sizeof(MSG));
    if ((NULL == applicationContext) || (NULL == message))
    {
        goto CLEANUP;
    }

    applicationContext->instanceHandle = instanceHandle;
    applicationContext->fitMode = FIT_MODE_PAGE;
    applicationContext->theme = THEME_LIGHT;
    applicationContext->fontPointSize = APP_DEFAULT_FONT_POINTS;
    applicationContext->zoomFactor = APP_DEFAULT_ZOOM;

    if (FALSE == Window_RegisterClass(instanceHandle))
    {
        goto CLEANUP;
    }
    if (FALSE == Window_Create(applicationContext, &applicationContext->windowHandle))
    {
        goto CLEANUP;
    }
    if (FALSE == Renderer_Create(&applicationContext->renderer))
    {
        goto CLEANUP;
    }

    App_OpenFromCommandLine(applicationContext);

    (void)ShowWindow(applicationContext->windowHandle, SW_SHOW);
    (void)UpdateWindow(applicationContext->windowHandle);
    App_UpdateTitle(applicationContext);

    while (0 != GetMessageW(message, NULL, 0, 0))
    {
        (void)TranslateMessage(message);
        (void)DispatchMessageW(message);
    }
    exitCode = (int)message->wParam;

CLEANUP:
    if (NULL != applicationContext)
    {
        if (NULL != applicationContext->renderer)
        {
            Renderer_Destroy(applicationContext->renderer);
            applicationContext->renderer = NULL;
        }
        if (NULL != applicationContext->document)
        {
            Document_Close(applicationContext->document);
            applicationContext->document = NULL;
        }
        if (NULL != applicationContext->menuFont)
        {
            (void)DeleteObject(applicationContext->menuFont);
            applicationContext->menuFont = NULL;
        }
        if (NULL != applicationContext->menuBackgroundBrush)
        {
            (void)DeleteObject(applicationContext->menuBackgroundBrush);
            applicationContext->menuBackgroundBrush = NULL;
        }
    }
    SAFE_FREE(message);
    SAFE_FREE(applicationContext);
    if (FALSE != comInitialized)
    {
        CoUninitialize();
    }
    return exitCode;
}
