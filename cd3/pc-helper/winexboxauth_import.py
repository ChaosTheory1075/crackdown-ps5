#!/usr/bin/env python3
"""winexboxauth (wine-uwp-src dlls/winexboxauth/util.c): take a sign-in the Crackdown hub made on the console.

The hub's payload does Microsoft's device-code sign-in itself (code + QR code on the TV) and leaves the refresh token
as plain "key=value" lines in xboxauth-import.txt beside xboxauth.bin. store_read() merges that file into the store,
saves it the usual way (DPAPI, as the console's Wine user) and deletes it, so the token is in plain text only until
the game's first start.

    winexboxauth_import.py /root/cd3/wine-uwp-src/dlls/winexboxauth/util.c
"""
import sys

p = sys.argv[1]
s = open(p).read()
if "xboxauth-import.txt" in s:
    print("already patched")
    sys.exit(0)

old_path = """static WCHAR *store_path( BOOL create_dir )
{"""
new_path = """static WCHAR *store_path_named( BOOL create_dir, const WCHAR *name );

static WCHAR *store_path( BOOL create_dir )
{
    return store_path_named( create_dir, L"xboxauth.bin" );
}

static WCHAR *store_path_named( BOOL create_dir, const WCHAR *name )
{"""
assert s.count(old_path) == 1, "store_path"
s = s.replace(old_path, new_path)
old_tail = """    if (!(path = malloc( (wcslen( base ) + 16) * sizeof(WCHAR) ))) return NULL;
    wcscpy( path, base );
    wcscat( path, L"\\\\xboxauth.bin" );
    return path;
}"""
new_tail = """    if (!(path = malloc( (wcslen( base ) + wcslen( name ) + 2) * sizeof(WCHAR) ))) return NULL;
    wcscpy( path, base );
    wcscat( path, L"\\\\" );
    wcscat( path, name );
    return path;
}

/* A sign-in made outside Wine (the Crackdown hub on the PS5): plain key=value lines, merged into the store and
 * deleted. */
static void store_import( struct kv *kv )
{
    WCHAR *path = store_path_named( FALSE, L"xboxauth-import.txt" );
    char *text, *p, *nl, *eq;
    DWORD size, read = 0;
    HANDLE file;
    BOOL any = FALSE;

    if (!path) return;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        free( path );
        return;
    }
    size = GetFileSize( file, NULL );
    text = size != INVALID_FILE_SIZE && size < 65536 ? malloc( size + 1 ) : NULL;
    if (text && !ReadFile( file, text, size, &read, NULL )) read = 0;
    CloseHandle( file );
    if (text)
    {
        text[read] = 0;
        for (p = text; *p; p = nl + 1)
        {
            if (!(nl = strchr( p, '\\n' ))) nl = p + strlen( p );
            if (nl > p && nl[-1] == '\\r') nl[-1] = 0;
            if ((eq = memchr( p, '=', nl - p )))
            {
                char *key = str_dupn( p, eq - p ), *val = str_dupn( eq + 1, nl - eq - 1 );
                if (*key)
                {
                    kv_set( kv, key, val );
                    any = TRUE;
                }
                free( key );
                str_wipe_free( val );
            }
            if (!*nl) break;
        }
        SecureZeroMemory( text, read );
        free( text );
    }
    if (any && store_write( kv )) MESSAGE( "xboxauth: sign-in imported from xboxauth-import.txt\\n" );
    else if (any) WARN( "xboxauth-import.txt read but the store could not be written\\n" );
    if (any) DeleteFileW( path );
    free( path );
}"""
assert s.count(old_tail) == 1, "store_path tail"
s = s.replace(old_tail, new_tail)

# store_read: after the store is read (or found missing), merge a pending import
old_read_end = """    if (!read || !CryptUnprotectData( &in, NULL, NULL, NULL, NULL, 0, &out ))
    {
        WARN( "stored sign-in could not be read (error %lu)\\n", GetLastError() );
        free( blob );
        return;
    }"""
new_read_end = """    if (!read || !CryptUnprotectData( &in, NULL, NULL, NULL, NULL, 0, &out ))
    {
        WARN( "stored sign-in could not be read (error %lu)\\n", GetLastError() );
        free( blob );
        store_import( kv );
        return;
    }"""
assert s.count(old_read_end) == 1, "store_read decrypt"
s = s.replace(old_read_end, new_read_end)
old_nofile = """    free( path );
    if (file == INVALID_HANDLE_VALUE) return;
    size = GetFileSize( file, NULL );"""
new_nofile = """    free( path );
    if (file == INVALID_HANDLE_VALUE)
    {
        store_import( kv );
        return;
    }
    size = GetFileSize( file, NULL );"""
assert s.count(old_nofile) == 1, "store_read open"
s = s.replace(old_nofile, new_nofile)
# a store that was read fine still takes a newer import (a new sign-in from the hub)
old_done = """            str_wipe_free( val );
        }
        if (!*nl) break;
    }
    str_wipe_free( blob );
}"""
new_done = """            str_wipe_free( val );
        }
        if (!*nl) break;
    }
    str_wipe_free( blob );
    store_import( kv );
}"""
assert s.count(old_done) == 1, "store_read end"
s = s.replace(old_done, new_done)
open(p, "w").write(s)
print("patched")
