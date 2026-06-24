/*****************************************************************************
 * window.h
 *
 * Window management: registration of the main window class and the window
 * procedure that translates Win32 messages (keyboard, mouse, resize, paint)
 * into calls on the application core. Keeping this separate from the core
 * keeps the message plumbing isolated from the reading behaviour.
 *****************************************************************************/

#ifndef WINDOW_H
#define WINDOW_H

#include "ereader.h"
#include "application.h"

/* The window-class name registered for the main reading window. */
#define WINDOW_CLASS_NAME L"WindowsEReaderMainWindow"

/* The full set of colours used to owner-draw the themed menu. */
typedef struct MENU_COLORS
{
    COLORREF background;        /* normal item / menu background               */
    COLORREF text;             /* normal item text                            */
    COLORREF highlight;        /* selected/hot item background                */
    COLORREF highlightText;    /* selected/hot item text                      */
    COLORREF separatorLine;    /* separator rule colour                       */
} MENU_COLORS;

/* Registers the main window class. Returns TRUE on success. */
BOOL Window_RegisterClass(HINSTANCE instanceHandle);

/*
 * Creates the main reading window, storing the application context pointer so
 * the window procedure can reach it. On success *createdWindow receives the
 * handle. Returns TRUE on success.
 */
BOOL Window_Create(APP_CONTEXT *applicationContext, HWND *createdWindow);

/* The window procedure for the main window class. */
LRESULT CALLBACK Window_Procedure(HWND windowHandle, UINT messageId, WPARAM wordParameter, LPARAM longParameter);

/*
 * Applies the current theme to the window chrome: the title bar (DWM dark
 * mode) and the menu bar (themed background brush + owner-draw refresh).
 * Called once at window creation and again whenever the theme changes.
 */
void Window_ApplyTheme(APP_CONTEXT *applicationContext);

#endif /* WINDOW_H */
