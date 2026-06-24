/*****************************************************************************
 * ereader.h
 *
 * Common definitions, includes, and helper macros shared across the
 * Windows E-Reader application. Every translation unit includes this header
 * first so that the Win32 surface and the project-wide conventions are
 * available in one place.
 *****************************************************************************/

#ifndef EREADER_H
#define EREADER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include "platform.h"   /* no-CRT memory/string layer (replaces <stdlib.h> etc.) */

/*
 * SAFE_FREE releases a heap block and resets the pointer to NULL so that a
 * dangling pointer can never survive past CLEANUP.
 */
#define SAFE_FREE(pointerToRelease)             \
    do {                                        \
        if (NULL != (pointerToRelease)) {       \
            Platform_Free(pointerToRelease);    \
            (pointerToRelease) = NULL;          \
        }                                       \
    } while (0)

/*
 * SAFE_RELEASE releases a COM interface (WIC, OLE) and resets it to NULL.
 */
#define SAFE_RELEASE(comInterface)                                  \
    do {                                                            \
        if (NULL != (comInterface)) {                               \
            (comInterface)->lpVtbl->Release(comInterface);          \
            (comInterface) = NULL;                                  \
        }                                                           \
    } while (0)

/* Convenience element-count macro for fixed-size arrays. */
#define EREADER_ARRAY_LENGTH(fixedArray) (sizeof(fixedArray) / sizeof((fixedArray)[0]))

#endif /* EREADER_H */
