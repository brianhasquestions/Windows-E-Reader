/*****************************************************************************
 * main.c
 *
 * Program entry point ONLY. The application is built without the C runtime,
 * so there is no CRT startup to call the usual wWinMain. Instead the linker
 * is pointed (via <EntryPointSymbol>) at EReaderEntryPoint below, which the
 * loader invokes directly. It performs the tiny amount of setup the CRT would
 * normally do — obtain the module handle — routes into the application core,
 * and terminates the process with ExitProcess. No application logic lives
 * here; everything is in application.c.
 *****************************************************************************/

#include "ereader.h"
#include "application.h"

void __stdcall EReaderEntryPoint(void)
{
    HINSTANCE instanceHandle = NULL;
    int       applicationExitCode = 0;

    /* Without the CRT there is no cached hInstance; fetch it from the loader. */
    instanceHandle = GetModuleHandleW(NULL);
    applicationExitCode = App_Run(instanceHandle, NULL);

    /* Returning would have nowhere to go without CRT startup; exit explicitly. */
    ExitProcess((UINT)applicationExitCode);
}
