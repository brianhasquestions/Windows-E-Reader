/*****************************************************************************
 * window.c
 *
 * Window plumbing. This module owns the window class and the window
 * procedure; it translates raw Win32 input messages into calls on the
 * application core (App_*). It deliberately holds no reading logic of its
 * own. Painting is double-buffered through an off-screen bitmap to keep page
 * turns and resizes flicker-free.
 *****************************************************************************/

#include "window.h"
#include "renderer.h"

#include <shellapi.h>
#include <shlwapi.h>       /* StrChrW for mnemonic / accelerator parsing */
#include <dwmapi.h>        /* DwmSetWindowAttribute for the dark title bar */

/*
 * Dark-title-bar attribute. Defined here for toolchains whose dwmapi.h predates
 * it; the value (20) is stable on Windows 10 2004+ and Windows 11.
 */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* Command identifiers for the programmatically built menu bar. */
#define ID_FILE_OPEN          1001
#define ID_FILE_EXIT          1002
#define ID_VIEW_FIT_PAGE      1010
#define ID_VIEW_FIT_WIDTH     1011
#define ID_VIEW_FIT_HEIGHT    1012
#define ID_VIEW_FIT_ORIGINAL  1013
#define ID_VIEW_ZOOM_IN       1014
#define ID_VIEW_ZOOM_OUT      1015
#define ID_VIEW_THEME         1016
#define ID_VIEW_FULLSCREEN    1017
#define ID_GO_NEXT            1020
#define ID_GO_PREVIOUS        1021
#define ID_GO_FIRST           1022
#define ID_GO_LAST            1023
#define ID_GO_PREVIOUS_CHAPTER 1024
#define ID_GO_NEXT_CHAPTER    1025
#define ID_HELP_ABOUT         1030

/* Input step sizes and the GetKeyState "key down" bit. */
#define WINDOW_PAN_STEP_PIXELS    60
#define WINDOW_ZOOM_STEP          0.1
#define WINDOW_FONT_STEP_POINTS   1
#define WINDOW_KEY_PRESSED_MASK   0x8000

/* Chapter navigation directions. */
#define WINDOW_DIRECTION_PREVIOUS (-1)
#define WINDOW_DIRECTION_NEXT     (1)

/* Default window geometry and the origin used for blits. */
#define WINDOW_DEFAULT_WIDTH      900
#define WINDOW_DEFAULT_HEIGHT     1000
#define WINDOW_ORIGIN             0
#define WINDOW_MINIMUM_DIMENSION  1

/* The first file in a multi-file drag-and-drop operation. */
#define WINDOW_FIRST_DROPPED_FILE 0

/* Owner-drawn menu metrics (device pixels). */
#define WINDOW_MENU_PADDING_X        14   /* left/right text inset per item     */
#define WINDOW_MENU_PADDING_Y        4    /* top/bottom text inset per item     */
#define WINDOW_MENU_ACCEL_GAP        28   /* gap between label and accelerator  */
#define WINDOW_MENU_SEPARATOR_HEIGHT 7    /* height of a separator item         */
#define WINDOW_MENU_SEPARATOR_THICKNESS 1 /* separator rule thickness           */
#define WINDOW_MENU_SEPARATOR_ID     0    /* command id reserved for separators */
#define WINDOW_MENU_CENTER_DIVISOR   2    /* halve a span to find its centre    */

/* Per-theme menu highlight (selection) colours, paired light/dark by theme. */
#define WINDOW_MENU_LIGHT_HIGHLIGHT      RGB(200, 220, 250)
#define WINDOW_MENU_LIGHT_HIGHLIGHT_TEXT RGB(20, 20, 20)
#define WINDOW_MENU_LIGHT_SEPARATOR      RGB(200, 200, 200)
#define WINDOW_MENU_SEPIA_HIGHLIGHT      RGB(220, 200, 160)
#define WINDOW_MENU_SEPIA_HIGHLIGHT_TEXT RGB(40, 30, 20)
#define WINDOW_MENU_SEPIA_SEPARATOR      RGB(205, 190, 158)
#define WINDOW_MENU_DARK_HIGHLIGHT       RGB(64, 64, 64)
#define WINDOW_MENU_DARK_HIGHLIGHT_TEXT  RGB(255, 255, 255)
#define WINDOW_MENU_DARK_SEPARATOR       RGB(80, 80, 80)

/* Sentinel item data marking an owner-drawn separator (it has no label). */
#define WINDOW_MENU_SEPARATOR_DATA   NULL

/* Retrieves the application context stashed in the window's user data slot. */
static APP_CONTEXT *Window_GetContext(HWND windowHandle)
{
    APP_CONTEXT *applicationContext = NULL;

    applicationContext = (APP_CONTEXT *)GetWindowLongPtrW(windowHandle, GWLP_USERDATA);

    return applicationContext;
}

/* Returns the full menu colour set for a theme (page colours + highlights). */
static MENU_COLORS Window_MenuColors(THEME theme)
{
    MENU_COLORS colors = { 0, 0, 0, 0, 0 };

    colors.background = Renderer_BackgroundColor(theme);
    colors.text = Renderer_TextColor(theme);

    switch (theme)
    {
        case THEME_DARK:
            colors.highlight = WINDOW_MENU_DARK_HIGHLIGHT;
            colors.highlightText = WINDOW_MENU_DARK_HIGHLIGHT_TEXT;
            colors.separatorLine = WINDOW_MENU_DARK_SEPARATOR;
            break;

        case THEME_SEPIA:
            colors.highlight = WINDOW_MENU_SEPIA_HIGHLIGHT;
            colors.highlightText = WINDOW_MENU_SEPIA_HIGHLIGHT_TEXT;
            colors.separatorLine = WINDOW_MENU_SEPIA_SEPARATOR;
            break;

        case THEME_LIGHT:
        default:
            colors.highlight = WINDOW_MENU_LIGHT_HIGHLIGHT;
            colors.highlightText = WINDOW_MENU_LIGHT_HIGHLIGHT_TEXT;
            colors.separatorLine = WINDOW_MENU_LIGHT_SEPARATOR;
            break;
    }

    return colors;
}

/* Creates the system menu font so owner-drawn items match native metrics. */
static HFONT Window_CreateMenuFont(void)
{
    NONCLIENTMETRICSW *metrics = NULL;
    HFONT              menuFont = NULL;

    metrics = (NONCLIENTMETRICSW *)Platform_AllocateZeroed(1, sizeof(NONCLIENTMETRICSW));
    if (NULL == metrics)
    {
        goto CLEANUP;
    }

    metrics->cbSize = sizeof(NONCLIENTMETRICSW);
    if (0 != SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), metrics, 0))
    {
        menuFont = CreateFontIndirectW(&metrics->lfMenuFont);
    }

CLEANUP:
    SAFE_FREE(metrics);
    return menuFont;
}

void Window_ApplyTheme(APP_CONTEXT *applicationContext)
{
    MENUINFO *menuInformation = NULL;
    HMENU     menuBar = NULL;
    HBRUSH    newBackgroundBrush = NULL;
    BOOL      useDarkTitleBar = FALSE;

    if ((NULL == applicationContext) || (NULL == applicationContext->windowHandle))
    {
        goto CLEANUP;
    }

    /* The owner-draw handlers need the menu font; create it once, lazily. */
    if (NULL == applicationContext->menuFont)
    {
        applicationContext->menuFont = Window_CreateMenuFont();
    }

    /* Title bar: dark caption for the dark theme, light otherwise. */
    useDarkTitleBar = (THEME_DARK == applicationContext->theme) ? TRUE : FALSE;
    (void)DwmSetWindowAttribute(applicationContext->windowHandle, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkTitleBar, sizeof(useDarkTitleBar));

    /* Menu bar: recolour the background brush shared by the bar and popups. */
    menuBar = GetMenu(applicationContext->windowHandle);
    if (NULL != menuBar)
    {
        newBackgroundBrush = CreateSolidBrush(Renderer_BackgroundColor(applicationContext->theme));
        menuInformation = (MENUINFO *)Platform_AllocateZeroed(1, sizeof(MENUINFO));
        if ((NULL != newBackgroundBrush) && (NULL != menuInformation))
        {
            menuInformation->cbSize = sizeof(MENUINFO);
            menuInformation->fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
            menuInformation->hbrBack = newBackgroundBrush;
            (void)SetMenuInfo(menuBar, menuInformation);

            /* The menu now owns a reference to the new brush; retire the old. */
            if (NULL != applicationContext->menuBackgroundBrush)
            {
                (void)DeleteObject(applicationContext->menuBackgroundBrush);
            }
            applicationContext->menuBackgroundBrush = newBackgroundBrush;
            newBackgroundBrush = NULL;
        }
    }

    (void)DrawMenuBar(applicationContext->windowHandle);

CLEANUP:
    if (NULL != newBackgroundBrush)
    {
        (void)DeleteObject(newBackgroundBrush);     /* never handed to the menu */
    }
    SAFE_FREE(menuInformation);
    return;
}

/* Appends one owner-drawn command item; the label is carried as item data. */
static void Window_AppendMenuItem(HMENU menu, UINT commandId, const wchar_t *label)
{
    (void)AppendMenuW(menu, MF_OWNERDRAW, commandId, label);
}

/* Appends an owner-drawn separator (disabled so it can never be selected). */
static void Window_AppendMenuSeparator(HMENU menu)
{
    (void)AppendMenuW(menu, MF_OWNERDRAW | MF_DISABLED, WINDOW_MENU_SEPARATOR_ID, WINDOW_MENU_SEPARATOR_DATA);
}

/* Appends an owner-drawn submenu to the bar; the label is carried as item data. */
static void Window_AppendMenuPopup(HMENU bar, HMENU submenu, const wchar_t *label)
{
    (void)AppendMenuW(bar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)submenu, label);
}

/* Builds and attaches the application menu bar. Returns TRUE on success. */
static BOOL Window_CreateMenuBar(HWND windowHandle)
{
    HMENU menuBar = NULL;
    HMENU fileMenu = NULL;
    HMENU viewMenu = NULL;
    HMENU goMenu = NULL;
    HMENU helpMenu = NULL;
    BOOL  successResult = FALSE;

    menuBar = CreateMenu();
    fileMenu = CreatePopupMenu();
    viewMenu = CreatePopupMenu();
    goMenu = CreatePopupMenu();
    helpMenu = CreatePopupMenu();
    if ((NULL == menuBar) || (NULL == fileMenu) || (NULL == viewMenu) ||
        (NULL == goMenu) || (NULL == helpMenu))
    {
        goto CLEANUP;
    }

    Window_AppendMenuItem(fileMenu, ID_FILE_OPEN, L"&Open...\tCtrl+O");
    Window_AppendMenuSeparator(fileMenu);
    Window_AppendMenuItem(fileMenu, ID_FILE_EXIT, L"E&xit");

    Window_AppendMenuItem(viewMenu, ID_VIEW_FIT_PAGE, L"Fit &Page\tP");
    Window_AppendMenuItem(viewMenu, ID_VIEW_FIT_WIDTH, L"Fit &Width\tW");
    Window_AppendMenuItem(viewMenu, ID_VIEW_FIT_HEIGHT, L"Fit &Height\tH");
    Window_AppendMenuItem(viewMenu, ID_VIEW_FIT_ORIGINAL, L"O&riginal Size\tR");
    Window_AppendMenuSeparator(viewMenu);
    Window_AppendMenuItem(viewMenu, ID_VIEW_ZOOM_IN, L"Zoom &In / Larger Font\t+");
    Window_AppendMenuItem(viewMenu, ID_VIEW_ZOOM_OUT, L"Zoom &Out / Smaller Font\t-");
    Window_AppendMenuSeparator(viewMenu);
    Window_AppendMenuItem(viewMenu, ID_VIEW_THEME, L"Cycle &Theme\tT");
    Window_AppendMenuItem(viewMenu, ID_VIEW_FULLSCREEN, L"&Fullscreen\tF11");

    Window_AppendMenuItem(goMenu, ID_GO_NEXT, L"&Next Page\t\x2192 / Space");
    Window_AppendMenuItem(goMenu, ID_GO_PREVIOUS, L"&Previous Page\t\x2190 / Backspace");
    Window_AppendMenuSeparator(goMenu);
    Window_AppendMenuItem(goMenu, ID_GO_FIRST, L"&First Page\tHome");
    Window_AppendMenuItem(goMenu, ID_GO_LAST, L"&Last Page\tEnd");
    Window_AppendMenuSeparator(goMenu);
    Window_AppendMenuItem(goMenu, ID_GO_PREVIOUS_CHAPTER, L"Previous &Chapter\t[");
    Window_AppendMenuItem(goMenu, ID_GO_NEXT_CHAPTER, L"Next C&hapter\t]");

    Window_AppendMenuItem(helpMenu, ID_HELP_ABOUT, L"&Controls...");

    Window_AppendMenuPopup(menuBar, fileMenu, L"&File");
    Window_AppendMenuPopup(menuBar, viewMenu, L"&View");
    Window_AppendMenuPopup(menuBar, goMenu, L"&Go");
    Window_AppendMenuPopup(menuBar, helpMenu, L"&Help");

    if (FALSE == SetMenu(windowHandle, menuBar))
    {
        goto CLEANUP;
    }
    successResult = TRUE;

CLEANUP:
    /* On failure none of the popups were attached, so release them all. */
    if (FALSE == successResult)
    {
        if (NULL != helpMenu) { (void)DestroyMenu(helpMenu); }
        if (NULL != goMenu) { (void)DestroyMenu(goMenu); }
        if (NULL != viewMenu) { (void)DestroyMenu(viewMenu); }
        if (NULL != fileMenu) { (void)DestroyMenu(fileMenu); }
        if (NULL != menuBar) { (void)DestroyMenu(menuBar); }
    }
    return successResult;
}

/* Shows a modal summary of the available controls. */
static void Window_ShowControls(HWND windowHandle)
{
    (void)MessageBoxW(windowHandle,
        L"Open\tCtrl+O, File \x25B8 Open, or drag a file onto the window\n"
        L"Next page\tRight / Down arrow, Space, Page Down, wheel down, left click\n"
        L"Previous page\tLeft / Up arrow, Backspace, Page Up, wheel up, right click\n"
        L"First / last\tHome / End\n"
        L"Pan a zoomed comic\tShift + arrow keys\n"
        L"Zoom / font size\t+ and -\n"
        L"Fit page/width/height/original\tP / W / H / R\n"
        L"Cycle theme\tT\n"
        L"Fullscreen\tF or F11\n"
        L"Previous / next chapter\t[ and ]",
        L"Windows E-Reader \x2014 Controls", MB_OK | MB_ICONINFORMATION);
}

/* Routes a menu command to the application core. */
static void Window_OnCommand(APP_CONTEXT *applicationContext, HWND windowHandle, WORD commandId)
{
    BOOL isComic = FALSE;

    if ((NULL != applicationContext->document) &&
        (DOCUMENT_TYPE_CBZ == applicationContext->document->documentType))
    {
        isComic = TRUE;
    }

    switch (commandId)
    {
        case ID_FILE_OPEN:
            App_PromptOpenDocument(applicationContext);
            break;

        case ID_FILE_EXIT:
            (void)DestroyWindow(windowHandle);
            break;

        case ID_VIEW_FIT_PAGE:
            App_SetFitMode(applicationContext, FIT_MODE_PAGE);
            break;

        case ID_VIEW_FIT_WIDTH:
            App_SetFitMode(applicationContext, FIT_MODE_WIDTH);
            break;

        case ID_VIEW_FIT_HEIGHT:
            App_SetFitMode(applicationContext, FIT_MODE_HEIGHT);
            break;

        case ID_VIEW_FIT_ORIGINAL:
            App_SetFitMode(applicationContext, FIT_MODE_ORIGINAL);
            break;

        case ID_VIEW_ZOOM_IN:
            if (FALSE != isComic)
            {
                App_AdjustZoom(applicationContext, WINDOW_ZOOM_STEP);
            }
            else
            {
                App_AdjustFontSize(applicationContext, WINDOW_FONT_STEP_POINTS);
            }
            break;

        case ID_VIEW_ZOOM_OUT:
            if (FALSE != isComic)
            {
                App_AdjustZoom(applicationContext, -WINDOW_ZOOM_STEP);
            }
            else
            {
                App_AdjustFontSize(applicationContext, -WINDOW_FONT_STEP_POINTS);
            }
            break;

        case ID_VIEW_THEME:
            App_CycleTheme(applicationContext);
            break;

        case ID_VIEW_FULLSCREEN:
            App_ToggleFullscreen(applicationContext);
            break;

        case ID_GO_NEXT:
            App_NextPage(applicationContext);
            break;

        case ID_GO_PREVIOUS:
            App_PreviousPage(applicationContext);
            break;

        case ID_GO_FIRST:
            App_GoToPage(applicationContext, 0);
            break;

        case ID_GO_LAST:
            App_GoToPage(applicationContext, applicationContext->totalPageCount - 1);
            break;

        case ID_GO_PREVIOUS_CHAPTER:
            App_GoToChapter(applicationContext, WINDOW_DIRECTION_PREVIOUS);
            break;

        case ID_GO_NEXT_CHAPTER:
            App_GoToChapter(applicationContext, WINDOW_DIRECTION_NEXT);
            break;

        case ID_HELP_ABOUT:
            Window_ShowControls(windowHandle);
            break;

        default:
            break;
    }
}

/* Double-buffered paint: render into an off-screen DC then blit it once. */
static void Window_OnPaint(APP_CONTEXT *applicationContext, HWND windowHandle)
{
    PAINTSTRUCT *paintStructure = NULL;
    RECT        *clientRectangle = NULL;
    HDC          windowDeviceContext = NULL;
    HDC          memoryDeviceContext = NULL;
    HBITMAP      memoryBitmap = NULL;
    HBITMAP      previousBitmap = NULL;
    int          clientWidth = 0;
    int          clientHeight = 0;

    paintStructure = (PAINTSTRUCT *)Platform_AllocateZeroed(1, sizeof(PAINTSTRUCT));
    clientRectangle = (RECT *)Platform_AllocateZeroed(1, sizeof(RECT));
    if ((NULL == paintStructure) || (NULL == clientRectangle))
    {
        goto CLEANUP;
    }

    windowDeviceContext = BeginPaint(windowHandle, paintStructure);
    if (NULL == windowDeviceContext)
    {
        goto CLEANUP;
    }

    (void)GetClientRect(windowHandle, clientRectangle);
    clientWidth = clientRectangle->right - clientRectangle->left;
    clientHeight = clientRectangle->bottom - clientRectangle->top;
    if ((WINDOW_MINIMUM_DIMENSION > clientWidth) || (WINDOW_MINIMUM_DIMENSION > clientHeight))
    {
        goto CLEANUP;
    }

    memoryDeviceContext = CreateCompatibleDC(windowDeviceContext);
    if (NULL == memoryDeviceContext)
    {
        goto CLEANUP;
    }
    memoryBitmap = CreateCompatibleBitmap(windowDeviceContext, clientWidth, clientHeight);
    if (NULL == memoryBitmap)
    {
        goto CLEANUP;
    }
    previousBitmap = (HBITMAP)SelectObject(memoryDeviceContext, memoryBitmap);

    Renderer_Paint(applicationContext, memoryDeviceContext);

    (void)BitBlt(windowDeviceContext, WINDOW_ORIGIN, WINDOW_ORIGIN, clientWidth, clientHeight, memoryDeviceContext, WINDOW_ORIGIN, WINDOW_ORIGIN, SRCCOPY);

CLEANUP:
    if (NULL != memoryDeviceContext)
    {
        if (NULL != previousBitmap)
        {
            (void)SelectObject(memoryDeviceContext, previousBitmap);
        }
        (void)DeleteDC(memoryDeviceContext);
    }
    if (NULL != memoryBitmap)
    {
        (void)DeleteObject(memoryBitmap);
    }
    if ((NULL != windowDeviceContext) && (NULL != paintStructure))
    {
        (void)EndPaint(windowHandle, paintStructure);
    }
    SAFE_FREE(clientRectangle);
    SAFE_FREE(paintStructure);
    return;
}

/* Case-insensitive comparison of two wide characters (ASCII letters only). */
static BOOL Window_CharsEqualIgnoreCase(wchar_t left, wchar_t right)
{
    if ((L'a' <= left) && (L'z' >= left))
    {
        left = (wchar_t)(left - (L'a' - L'A'));
    }
    if ((L'a' <= right) && (L'z' >= right))
    {
        right = (wchar_t)(right - (L'a' - L'A'));
    }

    return (left == right) ? TRUE : FALSE;
}

/* Sizes an owner-drawn menu item: text plus padding, or a thin separator. */
static void Window_OnMeasureItem(APP_CONTEXT *applicationContext, MEASUREITEMSTRUCT *measureItem)
{
    HDC            deviceContext = NULL;
    HFONT          previousFont = NULL;
    SIZE          *textSize = NULL;
    const wchar_t *label = NULL;
    const wchar_t *accelerator = NULL;
    int            itemWidth = 0;
    int            itemHeight = 0;

    if ((NULL == applicationContext) || (NULL == measureItem) || (ODT_MENU != measureItem->CtlType))
    {
        goto CLEANUP;
    }

    label = (const wchar_t *)measureItem->itemData;
    if (WINDOW_MENU_SEPARATOR_DATA == label)
    {
        measureItem->itemWidth = WINDOW_MENU_PADDING_X;
        measureItem->itemHeight = WINDOW_MENU_SEPARATOR_HEIGHT;
        goto CLEANUP;
    }

    if (NULL == applicationContext->menuFont)
    {
        applicationContext->menuFont = Window_CreateMenuFont();
    }

    deviceContext = GetDC(applicationContext->windowHandle);
    textSize = (SIZE *)Platform_AllocateZeroed(1, sizeof(SIZE));
    if ((NULL == deviceContext) || (NULL == textSize))
    {
        goto CLEANUP;
    }

    previousFont = (HFONT)SelectObject(deviceContext, applicationContext->menuFont);

    /* Items split into "label \t accelerator"; size both parts plus the gap. */
    accelerator = StrChrW(label, L'\t');
    if (NULL != accelerator)
    {
        (void)GetTextExtentPoint32W(deviceContext, label, (int)(accelerator - label), textSize);
        itemWidth = textSize->cx;
        itemHeight = textSize->cy;
        (void)GetTextExtentPoint32W(deviceContext, accelerator + 1, (int)Platform_LengthW(accelerator + 1, MAX_PATH), textSize);
        itemWidth += WINDOW_MENU_ACCEL_GAP + textSize->cx;
        if (textSize->cy > itemHeight)
        {
            itemHeight = textSize->cy;
        }
    }
    else
    {
        (void)GetTextExtentPoint32W(deviceContext, label, (int)Platform_LengthW(label, MAX_PATH), textSize);
        itemWidth = textSize->cx;
        itemHeight = textSize->cy;
    }

    measureItem->itemWidth = (UINT)(itemWidth + (WINDOW_MENU_PADDING_X * 2));
    measureItem->itemHeight = (UINT)(itemHeight + (WINDOW_MENU_PADDING_Y * 2));
    (void)SelectObject(deviceContext, previousFont);

CLEANUP:
    if (NULL != deviceContext)
    {
        (void)ReleaseDC(applicationContext->windowHandle, deviceContext);
    }
    SAFE_FREE(textSize);
    return;
}

/* Draws one owner-drawn menu item (or separator) in the active theme colours. */
static void Window_OnDrawItem(APP_CONTEXT *applicationContext, const DRAWITEMSTRUCT *drawItem)
{
    MENU_COLORS    colors = { 0, 0, 0, 0, 0 };
    HBRUSH         backgroundBrush = NULL;
    HPEN           separatorPen = NULL;
    HPEN           previousPen = NULL;
    HFONT          previousFont = NULL;
    RECT          *textRectangle = NULL;
    const wchar_t *label = NULL;
    const wchar_t *accelerator = NULL;
    COLORREF       backgroundColor = 0;
    COLORREF       textColor = 0;
    UINT           prefixFlags = 0;
    int            separatorY = 0;
    BOOL           isSelected = FALSE;

    if ((NULL == applicationContext) || (NULL == drawItem) || (ODT_MENU != drawItem->CtlType))
    {
        goto CLEANUP;
    }

    colors = Window_MenuColors(applicationContext->theme);
    label = (const wchar_t *)drawItem->itemData;
    isSelected = (0 != (drawItem->itemState & (ODS_SELECTED | ODS_HOTLIGHT))) ? TRUE : FALSE;
    backgroundColor = (FALSE != isSelected) ? colors.highlight : colors.background;
    textColor = (FALSE != isSelected) ? colors.highlightText : colors.text;

    backgroundBrush = CreateSolidBrush(backgroundColor);
    if (NULL != backgroundBrush)
    {
        (void)FillRect(drawItem->hDC, &drawItem->rcItem, backgroundBrush);
    }

    if (WINDOW_MENU_SEPARATOR_DATA == label)
    {
        separatorY = (drawItem->rcItem.top + drawItem->rcItem.bottom) / WINDOW_MENU_CENTER_DIVISOR;
        separatorPen = CreatePen(PS_SOLID, WINDOW_MENU_SEPARATOR_THICKNESS, colors.separatorLine);
        if (NULL != separatorPen)
        {
            previousPen = (HPEN)SelectObject(drawItem->hDC, separatorPen);
            (void)MoveToEx(drawItem->hDC, drawItem->rcItem.left + WINDOW_MENU_PADDING_X, separatorY, NULL);
            (void)LineTo(drawItem->hDC, drawItem->rcItem.right - WINDOW_MENU_PADDING_X, separatorY);
            (void)SelectObject(drawItem->hDC, previousPen);
        }
        goto CLEANUP;
    }

    if (NULL == applicationContext->menuFont)
    {
        applicationContext->menuFont = Window_CreateMenuFont();
    }

    textRectangle = (RECT *)Platform_AllocateZeroed(1, sizeof(RECT));
    if (NULL == textRectangle)
    {
        goto CLEANUP;
    }
    *textRectangle = drawItem->rcItem;
    textRectangle->left += WINDOW_MENU_PADDING_X;
    textRectangle->right -= WINDOW_MENU_PADDING_X;

    previousFont = (HFONT)SelectObject(drawItem->hDC, applicationContext->menuFont);
    (void)SetBkMode(drawItem->hDC, TRANSPARENT);
    (void)SetTextColor(drawItem->hDC, textColor);
    prefixFlags = (0 != (drawItem->itemState & ODS_NOACCEL)) ? DT_HIDEPREFIX : 0;

    /* Left-align the label; right-align the accelerator hint after the tab. */
    accelerator = StrChrW(label, L'\t');
    if (NULL != accelerator)
    {
        (void)DrawTextW(drawItem->hDC, label, (int)(accelerator - label), textRectangle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | prefixFlags);
        (void)DrawTextW(drawItem->hDC, accelerator + 1, -1, textRectangle, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | prefixFlags);
    }
    else
    {
        (void)DrawTextW(drawItem->hDC, label, -1, textRectangle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | prefixFlags);
    }
    (void)SelectObject(drawItem->hDC, previousFont);

CLEANUP:
    SAFE_FREE(textRectangle);
    if (NULL != separatorPen)
    {
        (void)DeleteObject(separatorPen);
    }
    if (NULL != backgroundBrush)
    {
        (void)DeleteObject(backgroundBrush);
    }
    return;
}

/*
 * Owner-drawn items carry no menu string, so the system can no longer match
 * Alt+letter access keys. This restores them by scanning each item's label for
 * the character after its '&' and asking the system to act on the match.
 */
static LRESULT Window_OnMenuChar(WPARAM wordParameter, LPARAM longParameter)
{
    MENUITEMINFOW *itemInformation = NULL;
    HMENU          menu = NULL;
    const wchar_t *label = NULL;
    const wchar_t *ampersand = NULL;
    LRESULT        result = MAKELRESULT(0, MNC_IGNORE);
    int            itemCount = 0;
    int            itemIndex = 0;
    wchar_t        pressedChar = 0;

    menu = (HMENU)longParameter;
    pressedChar = (wchar_t)LOWORD(wordParameter);
    if (NULL == menu)
    {
        goto CLEANUP;
    }

    itemInformation = (MENUITEMINFOW *)Platform_AllocateZeroed(1, sizeof(MENUITEMINFOW));
    if (NULL == itemInformation)
    {
        goto CLEANUP;
    }

    itemCount = GetMenuItemCount(menu);
    for (itemIndex = 0; itemIndex < itemCount; itemIndex += 1)
    {
        itemInformation->cbSize = sizeof(MENUITEMINFOW);
        itemInformation->fMask = MIIM_DATA;
        if (0 == GetMenuItemInfoW(menu, (UINT)itemIndex, TRUE, itemInformation))
        {
            continue;
        }
        label = (const wchar_t *)itemInformation->dwItemData;
        if (WINDOW_MENU_SEPARATOR_DATA == label)
        {
            continue;
        }
        ampersand = StrChrW(label, L'&');
        if ((NULL == ampersand) || (L'\0' == ampersand[1]))
        {
            continue;
        }
        if (FALSE != Window_CharsEqualIgnoreCase(ampersand[1], pressedChar))
        {
            result = MAKELRESULT(itemIndex, MNC_EXECUTE);
            goto CLEANUP;
        }
    }

CLEANUP:
    SAFE_FREE(itemInformation);
    return result;
}

/* Routes a key press to the appropriate application command. */
static void Window_OnKeyDown(APP_CONTEXT *applicationContext, WPARAM virtualKey)
{
    BOOL isComic = FALSE;
    BOOL controlHeld = FALSE;
    BOOL shiftHeld = FALSE;
    BOOL hasDocument = FALSE;

    controlHeld = (0 != (GetKeyState(VK_CONTROL) & WINDOW_KEY_PRESSED_MASK)) ? TRUE : FALSE;
    shiftHeld = (0 != (GetKeyState(VK_SHIFT) & WINDOW_KEY_PRESSED_MASK)) ? TRUE : FALSE;
    hasDocument = (NULL != applicationContext->document) ? TRUE : FALSE;
    isComic = ((FALSE != hasDocument) && (DOCUMENT_TYPE_CBZ == applicationContext->document->documentType)) ? TRUE : FALSE;

    /* Global shortcuts: these work whether or not a document is open, so they
     * are handled before the "needs a document" guard below. */
    switch (virtualKey)
    {
        case 'O':
            /* Ctrl+O always opens; a bare O opens from the welcome screen. */
            if ((FALSE != controlHeld) || (FALSE == hasDocument))
            {
                App_PromptOpenDocument(applicationContext);
            }
            goto CLEANUP;

        case 'T':
            App_CycleTheme(applicationContext);
            goto CLEANUP;

        case 'F':
        case VK_F11:
            App_ToggleFullscreen(applicationContext);
            goto CLEANUP;

        default:
            break;
    }

    /* Everything past here operates on the open document. */
    if (FALSE == hasDocument)
    {
        goto CLEANUP;
    }

    switch (virtualKey)
    {
        case VK_SPACE:
        case VK_NEXT:                       /* Page Down */
            App_NextPage(applicationContext);
            break;

        case VK_BACK:
        case VK_PRIOR:                      /* Page Up */
            App_PreviousPage(applicationContext);
            break;

        /* Plain arrow keys always turn pages. Shift+arrows pan a zoomed comic. */
        case VK_RIGHT:
            if ((FALSE != isComic) && (FALSE != shiftHeld))
            {
                App_Pan(applicationContext, -WINDOW_PAN_STEP_PIXELS, 0);
            }
            else
            {
                App_NextPage(applicationContext);
            }
            break;

        case VK_LEFT:
            if ((FALSE != isComic) && (FALSE != shiftHeld))
            {
                App_Pan(applicationContext, WINDOW_PAN_STEP_PIXELS, 0);
            }
            else
            {
                App_PreviousPage(applicationContext);
            }
            break;

        case VK_DOWN:
            if ((FALSE != isComic) && (FALSE != shiftHeld))
            {
                App_Pan(applicationContext, 0, -WINDOW_PAN_STEP_PIXELS);
            }
            else
            {
                App_NextPage(applicationContext);
            }
            break;

        case VK_UP:
            if ((FALSE != isComic) && (FALSE != shiftHeld))
            {
                App_Pan(applicationContext, 0, WINDOW_PAN_STEP_PIXELS);
            }
            else
            {
                App_PreviousPage(applicationContext);
            }
            break;

        case VK_HOME:
            App_GoToPage(applicationContext, 0);
            break;

        case VK_END:
            App_GoToPage(applicationContext, applicationContext->totalPageCount - 1);
            break;

        case VK_OEM_PLUS:
        case VK_ADD:
            if (FALSE != isComic)
            {
                App_AdjustZoom(applicationContext, WINDOW_ZOOM_STEP);
            }
            else
            {
                App_AdjustFontSize(applicationContext, WINDOW_FONT_STEP_POINTS);
            }
            break;

        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            if (FALSE != isComic)
            {
                App_AdjustZoom(applicationContext, -WINDOW_ZOOM_STEP);
            }
            else
            {
                App_AdjustFontSize(applicationContext, -WINDOW_FONT_STEP_POINTS);
            }
            break;

        case 'W':
            App_SetFitMode(applicationContext, FIT_MODE_WIDTH);
            break;

        case 'H':
            App_SetFitMode(applicationContext, FIT_MODE_HEIGHT);
            break;

        case 'P':
            App_SetFitMode(applicationContext, FIT_MODE_PAGE);
            break;

        case 'R':
            App_SetFitMode(applicationContext, FIT_MODE_ORIGINAL);
            break;

        case VK_OEM_4:                      /* '[' previous chapter */
            App_GoToChapter(applicationContext, WINDOW_DIRECTION_PREVIOUS);
            break;

        case VK_OEM_6:                      /* ']' next chapter */
            App_GoToChapter(applicationContext, WINDOW_DIRECTION_NEXT);
            break;

        default:
            break;
    }

CLEANUP:
    return;
}

/* Handles a drag-and-drop of one or more files; opens the first. */
static void Window_OnDropFiles(APP_CONTEXT *applicationContext, HDROP dropHandle)
{
    wchar_t *droppedPath = NULL;

    droppedPath = (wchar_t *)Platform_AllocateZeroed(MAX_PATH, sizeof(wchar_t));
    if (NULL == droppedPath)
    {
        goto CLEANUP;
    }

    if (0 != DragQueryFileW(dropHandle, WINDOW_FIRST_DROPPED_FILE, droppedPath, MAX_PATH))
    {
        (void)App_OpenDocument(applicationContext, droppedPath);
    }

CLEANUP:
    DragFinish(dropHandle);
    SAFE_FREE(droppedPath);
    return;
}

LRESULT CALLBACK Window_Procedure(HWND windowHandle, UINT messageId, WPARAM wordParameter, LPARAM longParameter)
{
    APP_CONTEXT *applicationContext = NULL;
    LRESULT      messageResult = 0;

    if (WM_CREATE == messageId)
    {
        CREATESTRUCTW *createStructure = NULL;
        APP_CONTEXT   *creationContext = NULL;
        createStructure = (CREATESTRUCTW *)longParameter;
        creationContext = (APP_CONTEXT *)createStructure->lpCreateParams;
        SetWindowLongPtrW(windowHandle, GWLP_USERDATA, (LONG_PTR)creationContext);
        if (NULL != creationContext)
        {
            creationContext->windowHandle = windowHandle;   /* needed before theming */
        }
        DragAcceptFiles(windowHandle, TRUE);
        (void)Window_CreateMenuBar(windowHandle);
        Window_ApplyTheme(creationContext);
        goto CLEANUP;
    }

    applicationContext = Window_GetContext(windowHandle);
    if (NULL == applicationContext)
    {
        messageResult = DefWindowProcW(windowHandle, messageId, wordParameter, longParameter);
        goto CLEANUP;
    }

    switch (messageId)
    {
        case WM_PAINT:
            Window_OnPaint(applicationContext, windowHandle);
            break;

        case WM_ERASEBKGND:
            messageResult = 1;              /* handled in WM_PAINT; suppress flicker */
            break;

        case WM_SIZE:
            App_Relayout(applicationContext);
            break;

        case WM_KEYDOWN:
            Window_OnKeyDown(applicationContext, wordParameter);
            break;

        case WM_COMMAND:
            Window_OnCommand(applicationContext, windowHandle, LOWORD(wordParameter));
            break;

        case WM_MEASUREITEM:
            Window_OnMeasureItem(applicationContext, (MEASUREITEMSTRUCT *)longParameter);
            messageResult = TRUE;
            break;

        case WM_DRAWITEM:
            Window_OnDrawItem(applicationContext, (const DRAWITEMSTRUCT *)longParameter);
            messageResult = TRUE;
            break;

        case WM_MENUCHAR:
            messageResult = Window_OnMenuChar(wordParameter, longParameter);
            break;

        case WM_MOUSEWHEEL:
        {
            int wheelDelta = 0;
            wheelDelta = GET_WHEEL_DELTA_WPARAM(wordParameter);
            if (0 < wheelDelta)
            {
                App_PreviousPage(applicationContext);
            }
            else if (0 > wheelDelta)
            {
                App_NextPage(applicationContext);
            }
            break;
        }

        case WM_LBUTTONDOWN:
            App_NextPage(applicationContext);
            break;

        case WM_RBUTTONDOWN:
            App_PreviousPage(applicationContext);
            break;

        case WM_DROPFILES:
            Window_OnDropFiles(applicationContext, (HDROP)wordParameter);
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        default:
            messageResult = DefWindowProcW(windowHandle, messageId, wordParameter, longParameter);
            break;
    }

CLEANUP:
    return messageResult;
}

BOOL Window_RegisterClass(HINSTANCE instanceHandle)
{
    WNDCLASSEXW *windowClass = NULL;
    BOOL         successResult = FALSE;

    windowClass = (WNDCLASSEXW *)Platform_AllocateZeroed(1, sizeof(WNDCLASSEXW));
    if (NULL == windowClass)
    {
        goto CLEANUP;
    }

    windowClass->cbSize = sizeof(WNDCLASSEXW);
    windowClass->style = CS_HREDRAW | CS_VREDRAW;
    windowClass->lpfnWndProc = Window_Procedure;
    windowClass->hInstance = instanceHandle;
    windowClass->hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass->hbrBackground = NULL;       /* the renderer paints the background */
    windowClass->lpszClassName = WINDOW_CLASS_NAME;
    windowClass->hIcon = LoadIconW(NULL, IDI_APPLICATION);

    if (0 != RegisterClassExW(windowClass))
    {
        successResult = TRUE;
    }

CLEANUP:
    SAFE_FREE(windowClass);
    return successResult;
}

BOOL Window_Create(APP_CONTEXT *applicationContext, HWND *createdWindow)
{
    HWND windowHandle = NULL;
    BOOL successResult = FALSE;

    if ((NULL == applicationContext) || (NULL == createdWindow))
    {
        goto CLEANUP;
    }
    *createdWindow = NULL;

    windowHandle = CreateWindowExW(WS_EX_ACCEPTFILES, WINDOW_CLASS_NAME,
                                   L"Windows E-Reader", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT,
                                   WINDOW_DEFAULT_WIDTH, WINDOW_DEFAULT_HEIGHT,
                                   NULL, NULL, applicationContext->instanceHandle,
                                   applicationContext);
    if (NULL == windowHandle)
    {
        goto CLEANUP;
    }

    *createdWindow = windowHandle;
    successResult = TRUE;

CLEANUP:
    return successResult;
}
