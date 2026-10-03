/*
 * winesailover.drv entry points (PE side)
 *
 * The display driver of SailOver: windows live in the native window server wsd (macOS), reached
 * through an AF_UNIX socket. See voile-lkd wsd/PROTOCOL.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sailover);

static BOOL CALLBACK load_first_icon( HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param )
{
    *(HICON *)param = LoadImageW( module, name, IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR );
    return FALSE;  /* the first icon group is the program's icon */
}

/* Gives wsd the program's icon for the Dock and Cmd-Tab. Wine's own programs (explorer, services, ...) do not:
 * the icon is the game's. */
static void send_app_icon(void)
{
    struct sailoverdrv_app_icon_params params;
    WCHAR path[MAX_PATH], windows[MAX_PATH];
    BITMAPINFO info = {{ sizeof(BITMAPINFOHEADER) }};
    UINT len = GetWindowsDirectoryW( windows, ARRAY_SIZE(windows) ), count, i;
    DWORD *bits = NULL, *mask = NULL;
    ICONINFO icon_info;
    HICON icon = NULL;
    BOOL has_alpha = FALSE;
    BITMAP bm;
    HDC hdc;

    if (!len || !GetModuleFileNameW( NULL, path, ARRAY_SIZE(path) ) || !_wcsnicmp( path, windows, len )) return;
    EnumResourceNamesW( GetModuleHandleW( NULL ), (LPCWSTR)RT_GROUP_ICON, load_first_icon, (LONG_PTR)&icon );
    if (!icon) return;
    if (!GetIconInfo( icon, &icon_info ))
    {
        DestroyIcon( icon );
        return;
    }
    if (icon_info.hbmColor && GetObjectW( icon_info.hbmColor, sizeof(bm), &bm ))
    {
        info.bmiHeader.biWidth = bm.bmWidth;
        info.bmiHeader.biHeight = -bm.bmHeight;  /* top-down */
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        count = bm.bmWidth * bm.bmHeight;
        bits = malloc( count * 4 );
        mask = malloc( count * 4 );
        hdc = CreateCompatibleDC( 0 );
        if (bits && mask && GetDIBits( hdc, icon_info.hbmColor, 0, bm.bmHeight, bits, &info, DIB_RGB_COLORS ))
        {
            for (i = 0; i < count && !has_alpha; i++) has_alpha = (bits[i] >> 24) != 0;
            /* an icon without alpha: transparent where its mask is set */
            if (!has_alpha && GetDIBits( hdc, icon_info.hbmMask, 0, bm.bmHeight, mask, &info, DIB_RGB_COLORS ))
                for (i = 0; i < count; i++) bits[i] = (bits[i] & 0xffffff) | ((mask[i] & 0xffffff) ? 0 : 0xff000000);
            params.width = bm.bmWidth;
            params.height = bm.bmHeight;
            params.bits = bits;
            if (WINE_UNIX_CALL( sailoverdrv_unix_func_set_app_icon, &params ))
                WARN( "the window server refused the program's icon\n" );
        }
        DeleteDC( hdc );
        free( bits );
        free( mask );
    }
    if (icon_info.hbmColor) DeleteObject( icon_info.hbmColor );
    DeleteObject( icon_info.hbmMask );
    DestroyIcon( icon );
}

/* Reads wsd events and injects input; runs until the connection breaks. */
static DWORD WINAPI read_events_thread( void *arg )
{
    send_app_icon();
    WINE_UNIX_CALL( sailoverdrv_unix_func_read_events, NULL );
    ERR( "lost the connection to the window server wsd\n" );
    return 0;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    DWORD tid;

    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls( instance );
    if (__wine_init_unix_call()) return FALSE;
    if (WINE_UNIX_CALL( sailoverdrv_unix_func_init, NULL )) return FALSE;
    CloseHandle( CreateThread( NULL, 0, read_events_thread, NULL, 0, &tid ));
    return TRUE;
}
