/*
 * black.exe: address-space owner for the Xbox image.
 *
 * Linked at base 0x10000 (/FIXED, no CRT). The uninitialized array below makes this image span
 * 0x10000 up to about 0x800000, which covers the XBE (0x10000-0x587a40). Windows maps the exe before
 * it creates the process heap, the main stack or anything else, so the range is guaranteed free.
 * blackrt.dll (loaded through the import table) copies the XBE over this whole image, including these
 * PE headers and this code, so nothing here may run after BlackMain is entered. BlackMain never returns.
 */
#include <windows.h>

#define XBOX_RESERVE_SIZE 0x007F0000u

__declspec(dllimport) int __cdecl BlackMain(void *reserve, unsigned reserve_size);

static unsigned char g_xbox_image_reserve[XBOX_RESERVE_SIZE];

void __cdecl StubEntry(void)
{
    ExitProcess((UINT)BlackMain(g_xbox_image_reserve, XBOX_RESERVE_SIZE));
}
