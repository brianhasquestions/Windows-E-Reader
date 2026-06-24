/*****************************************************************************
 * application.h
 *
 * Application core. main.c does nothing but hand control to this module,
 * which owns the single APP_CONTEXT instance, the message loop, and all of
 * the reading/navigation behaviour. The window procedure routes user input
 * back into the App_* command functions declared here.
 *****************************************************************************/

#ifndef APPLICATION_H
#define APPLICATION_H

#include "ereader.h"
#include "document.h"

/* Forward declaration: the renderer is owned by APP_CONTEXT by pointer. */
struct RENDERER;

/* How a CBZ page image is scaled to the viewport. */
typedef enum FIT_MODE
{
    FIT_MODE_PAGE = 0,                  /* whole page visible                  */
    FIT_MODE_WIDTH,                     /* fill width, scroll vertically       */
    FIT_MODE_HEIGHT,                    /* fill height                         */
    FIT_MODE_ORIGINAL                   /* native pixels, pan freely           */
} FIT_MODE;

/* Reading colour scheme, applied to both image background and text. */
typedef enum THEME
{
    THEME_LIGHT = 0,
    THEME_SEPIA,
    THEME_DARK
} THEME;

/*
 * The one and only shared state object. Passing a single pointer to this
 * struct keeps almost every function within the three-parameter rule.
 */
typedef struct APP_CONTEXT
{
    HINSTANCE         instanceHandle;
    HWND              windowHandle;
    DOCUMENT         *document;
    struct RENDERER  *renderer;

    int               currentPageIndex;     /* zero-based page being viewed   */
    int               totalPageCount;        /* updated after each layout     */

    FIT_MODE          fitMode;
    THEME             theme;
    int               fontPointSize;         /* EPUB body text size in points */
    double            zoomFactor;            /* CBZ manual zoom multiplier    */
    int               panOffsetX;            /* CBZ pan when zoomed in        */
    int               panOffsetY;

    BOOL              isFullscreen;
    WINDOWPLACEMENT   previousPlacement;     /* restore point for fullscreen  */

    HFONT             menuFont;              /* font for owner-drawn menu      */
    HBRUSH            menuBackgroundBrush;   /* themed menu background brush   */

    wchar_t           currentFilePath[MAX_PATH];
} APP_CONTEXT;

/*
 * Entry point for the application core, invoked by wWinMain. Creates the
 * window, optionally opens a file passed on the command line, and runs the
 * message loop. Returns the process exit code.
 */
int App_Run(HINSTANCE instanceHandle, const wchar_t *commandLine);

/* Opens (or replaces) the document shown in the window. Returns TRUE on success. */
BOOL App_OpenDocument(APP_CONTEXT *applicationContext, const wchar_t *documentFilePath);

/* Shows the standard open-file dialog and loads the chosen document. */
void App_PromptOpenDocument(APP_CONTEXT *applicationContext);

/* Page navigation commands. */
void App_GoToPage(APP_CONTEXT *applicationContext, int targetPageIndex);
void App_NextPage(APP_CONTEXT *applicationContext);
void App_PreviousPage(APP_CONTEXT *applicationContext);

/* Continuous scrolling, used by the mouse wheel; deltaUnits is signed. */
void App_ScrollBy(APP_CONTEXT *applicationContext, int deltaUnits);

/* Pans the current CBZ page when it is zoomed larger than the viewport. */
void App_Pan(APP_CONTEXT *applicationContext, int deltaX, int deltaY);

/* Jumps to the previous (direction < 0) or next (direction > 0) EPUB chapter. */
void App_GoToChapter(APP_CONTEXT *applicationContext, int direction);

/* View commands. */
void App_SetFitMode(APP_CONTEXT *applicationContext, FIT_MODE newFitMode);
void App_CycleTheme(APP_CONTEXT *applicationContext);
void App_AdjustFontSize(APP_CONTEXT *applicationContext, int pointDelta);
void App_AdjustZoom(APP_CONTEXT *applicationContext, double zoomDelta);
void App_ToggleFullscreen(APP_CONTEXT *applicationContext);

/* Recomputes pagination after a resize or setting change, then repaints. */
void App_Relayout(APP_CONTEXT *applicationContext);

#endif /* APPLICATION_H */
