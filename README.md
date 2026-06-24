# Windows E-Reader

A native Windows application for reading **EPUB** e-books and **CBZ** comic
archives, written in C against the **Win32 API** only and built with the
**MSVC** compiler. No third-party libraries are used: ZIP/DEFLATE decoding is
implemented from scratch, image decoding uses the Windows Imaging Component
(WIC), and text/image presentation uses GDI.

## Features

- **EPUB**: parses `META-INF/container.xml` → OPF package → spine reading
  order, reduces each XHTML chapter to reflowable text, and word-wraps it into
  real pages that re-flow when the window is resized or the font size changes.
- **CBZ**: every image member becomes a page, decoded on demand with WIC and
  drawn with high-quality (halftone) scaling.
- **Page-based navigation** with both mouse and keyboard.
- **Fit modes** for comics: whole page, fit width, fit height, original size.
- **Zoom and pan** for comics; **adjustable font size** for e-books.
- **Reading themes**: Light, Sep, Dark (cycle through them).
- **Fullscreen** reading mode.
- **Chapter navigation** for EPUB.
- **Menu bar** (File / View / Go / Help) and a welcome screen, plus
  **drag-and-drop** and command-line file opening.
- Flicker-free, double-buffered rendering.

## Opening a file

Any of these work:

- **File ▸ Open...** from the menu bar.
- **Ctrl+O** keyboard shortcut.
- **Drag and drop** an `.epub` or `.cbz` file onto the window.
- Pass a path on the command line (e.g. file association or `WindowsEReader.exe book.epub`).

## Controls

| Action | Keys / Mouse |
| --- | --- |
| Next page | `→`, `↓`, `Space`, `Page Down`, mouse wheel down, left click |
| Previous page | `←`, `↑`, `Backspace`, `Page Up`, mouse wheel up, right click |
| First / last page | `Home` / `End` |
| Pan a zoomed comic | `Shift` + arrow keys |
| Zoom comic in / out | `+` / `-` |
| Font size (e-book) | `+` / `-` |
| Fit: page / width / height / original | `P` / `W` / `H` / `R` |
| Cycle theme | `T` |
| Fullscreen | `F` or `F11` |
| Previous / next chapter (EPUB) | `[` / `]` |
| Open file | `Ctrl+O`, or **File ▸ Open** |

The same commands are available from the menu bar (**Go**, **View**), and a
list is shown under **Help ▸ Controls**.

## Project layout

```
WindowsEReader.sln
WindowsEReader.vcxproj(.filters)   Visual Studio project + logical filters
src/
  include/                         headers, grouped by concern
    ereader.h                      shared macros and Win32 surface
    platform.h                     no-CRT memory/string layer
    application.h  window.h         core + window plumbing
    archive.h      inflate.h        ZIP reader + DEFLATE decoder
    document.h     renderer.h       document model + presentation
  source/                          implementations, mirrored layout
    main.c                         custom entry point only — routes to core
    platform.c                     no-CRT support (HeapAlloc, mem/str helpers)
    application.c                  application core / message loop / commands
    window.c                       window class + window procedure
    archive.c      inflate.c        ZIP (file-mapped) + DEFLATE
    document.c                     EPUB/CBZ parsing into the document model
    renderer.c                     WIC image + GDI text rendering
testfiles/                         sample.epub and sample.cbz for trying it out
```

## Building

Open `WindowsEReader.sln` in Visual Studio and build, or from a command line:

```
msbuild WindowsEReader.sln /p:Configuration=Release /p:Platform=x64
```

The output is `x64\Release\WindowsEReader.exe`. The Release configuration is
optimized for size (`/O1 /Os`, function-level linking, COMDAT folding, whole
program optimization).

## Architecture notes & conventions

- **No C runtime.** The application links with `/NODEFAULTLIB` and a custom
  entry point (`EReaderEntryPoint` in `main.c`, which calls `App_Run` then
  `ExitProcess`). The Release binary depends only on Windows system DLLs
  (kernel32, user32, gdi32, shell32, shlwapi, ole32, comdlg32) — no
  `vcruntime`/`ucrtbase`/`msvcrt` — and is ~44 KB.
- Everything the CRT would normally provide lives in the `platform` module
  (`platform.h`/`platform.c`): heap memory on `HeapAlloc`/`HeapReAlloc`/
  `HeapFree`, bounded by-hand string/length/copy helpers, the freestanding
  `memset`/`memcpy`/`memmove`/`memcmp` the compiler still emits, and the
  `_fltused` floating-point marker.
- String comparison uses the Win32 `CompareStringOrdinal` (the recommended
  secure, locale-independent replacement for `strcmp`/`_stricmp`); formatting
  uses `wsprintfW`; sorting is an in-house Shell sort (no `qsort`).
- A single `APP_CONTEXT` is threaded through the code so that nearly every
  function stays within a three-parameter budget; where more state is needed a
  dedicated struct is passed instead.
- Each ZIP member's uncompressed size is known from the central directory, so
  the inflater writes into an exactly-sized, bounds-checked buffer.
- Memory is dynamically allocated and released along a single `goto CLEANUP`
  path in every function; copies are bounds-checked against the destination
  size.

## Known simplifications

The EPUB renderer is intentionally lightweight: it renders reflowable text and
does not apply CSS styling or display inline images. Comic (CBZ) pages are
rendered at full image fidelity.
