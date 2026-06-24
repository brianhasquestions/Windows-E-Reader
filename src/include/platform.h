/*****************************************************************************
 * platform.h
 *
 * No-CRT platform layer. The application links without the C runtime, so this
 * module supplies everything the rest of the code would normally pull from
 * the CRT, implemented directly on the Win32 API (the process heap, the
 * string/character routines, and the handful of freestanding primitives the
 * compiler still emits such as memset/memcpy). Every other module allocates
 * and manipulates strings exclusively through the helpers declared here.
 *****************************************************************************/

#ifndef PLATFORM_H
#define PLATFORM_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stddef.h>     /* compiler-provided: size_t, wchar_t, NULL (no CRT) */

/*
 * Bounded memory-copy request. Bundled into a struct so the bounded copy
 * stays within the three-parameter rule while still carrying the destination
 * size needed for the overflow check.
 */
typedef struct MEMORY_COPY_REQUEST
{
    void       *destination;
    size_t      destinationSize;
    const void *source;
    size_t      byteCount;
} MEMORY_COPY_REQUEST;

/*---------------------------------------------------------------------------
 * Heap memory (process heap via HeapAlloc / HeapReAlloc / HeapFree).
 * Every block is zeroed with SecureZeroMemory on allocation and scrubbed
 * with SecureZeroMemory on free.
 *--------------------------------------------------------------------------*/

/* Allocates byteCount bytes, zeroed with SecureZeroMemory. Returns NULL on failure. */
void *Platform_Allocate(size_t byteCount);

/* Allocates elementCount * elementSize bytes, zeroed with SecureZeroMemory; guards the multiply. */
void *Platform_AllocateZeroed(size_t elementCount, size_t elementSize);

/* Grows/shrinks a block (NULL acts like a fresh allocation); any grown tail is zeroed. */
void *Platform_Reallocate(void *existingBlock, size_t newByteCount);

/* Scrubs the block with SecureZeroMemory, then releases it. Safe to call with NULL. */
void  Platform_Free(void *blockToFree);

/* Bounded copy; returns FALSE if the bytes would not fit the destination. */
BOOL  Platform_CopyMemory(const MEMORY_COPY_REQUEST *copyRequest);

/*---------------------------------------------------------------------------
 * Narrow (UTF-8 / ASCII) strings
 *--------------------------------------------------------------------------*/

/* Bounded length: counts up to maximumLength; returns 0 for a NULL pointer. */
size_t Platform_LengthA(const char *text, size_t maximumLength);

/* Bounded copy (like strcpy_s). Returns FALSE if source does not fit. */
BOOL   Platform_CopyA(char *destination, size_t destinationCount, const char *source);

/* Bounded append (like strcat_s). Returns FALSE if the result does not fit. */
BOOL   Platform_AppendA(char *destination, size_t destinationCount, const char *source);

/* Returns the first occurrence of needle in haystack, or NULL (like strstr). */
char *Platform_FindSubstringA(const char *haystack, const char *needle);

/* Returns the first occurrence of target in text, or NULL (like strchr). */
char *Platform_FindCharA(const char *text, char target);

/* Returns the last occurrence of target in text, or NULL (like strrchr). */
char *Platform_FindLastCharA(const char *text, char target);

/* Case-insensitive prefix compare of count chars; 0 when equal (like _strnicmp). */
int    Platform_ComparePrefixIgnoreCaseA(const char *left, const char *right, size_t count);

/* TRUE when character is an ASCII decimal digit. */
BOOL   Platform_IsDigitA(char character);

/* Value (0..15) of an ASCII hex digit, or 0 for a non-hex character. */
int    Platform_HexValueA(char character);

/*---------------------------------------------------------------------------
 * Wide strings
 *--------------------------------------------------------------------------*/

/* Bounded length: counts up to maximumLength; returns 0 for a NULL pointer. */
size_t Platform_LengthW(const wchar_t *text, size_t maximumLength);

/* Bounded copy (like wcscpy_s). Returns FALSE if source does not fit. */
BOOL   Platform_CopyW(wchar_t *destination, size_t destinationCount, const wchar_t *source);

#endif /* PLATFORM_H */
