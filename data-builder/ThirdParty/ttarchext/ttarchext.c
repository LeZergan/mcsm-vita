/*
    Copyright 2009-2019 Luigi Auriemma

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

    http://www.gnu.org/licenses/gpl-2.0.txt
*/

#define _WIN32_WINNT    0x0601
#define _WIN32_WINDOWS  0x0601
#define WINVER          0x0601

//#define NOLFS
#ifndef NOLFS
    #define _LARGE_FILES        // if it's not supported the tool will work
    #define __USE_LARGEFILE64   // without support for large files
    #define __USE_FILE_OFFSET64
    #define _LARGEFILE_SOURCE
    #define _LARGEFILE64_SOURCE
    #define _FILE_OFFSET_BITS   64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <errno.h>
#include <zlib.h>
#include "blowfish_ttarch.h"

#ifdef WIN32
    #include <windows.h>
    #include <direct.h>
    #define PATHSLASH   '\\'
    #define make_dir(x) mkdir(x)
#else
    #include <dirent.h>
    #define stricmp     strcasecmp
    #define strnicmp    strncasecmp
    #define stristr     strcasestr
    #define PATHSLASH   '/'
    #define make_dir(x) mkdir(x, 0755)
#endif

typedef uint8_t     u8;
typedef uint32_t    u32;
typedef int64_t     i64;
typedef uint64_t    u64;



#if defined(_LARGE_FILES)
    #if defined(__APPLE__)
        #define fseek   fseeko
        #define ftell   ftello
    #elif defined(__FreeBSD__)
    #elif !defined(NOLFS)       // use -DNOLFS if this tool can't be compiled on your OS!
        #define off_t   off64_t
        #define fopen   fopen64
        #define fseek   fseeko64
        #define ftell   ftello64
        #ifndef fstat
            #ifdef WIN32
                #define fstat   _fstati64
                #define stat    _stati64
            #else
                #define fstat   fstat64
                #define stat    stat64
            #endif
        #endif
    #endif
#endif



/*
#if UINTPTR_MAX == 0xffffffff
    #error "ttarchext must be compiled as 64bit to load the Oodle DLL."
#endif
*/



#define VER             "0.3.2"
#define DEFAULT_VERSION 1
#define PRIx            "016"PRIx64
#define IS_LUA(X)       (!memcmp(X, "Lua", 3) || !memcmp(X, "\x1bLu", 3))
#define IS_LUA2(X)      (!memcmp(X, "\x1bLEn", 4))
#define IS_LUA3(X)      (!memcmp(X, "\x1bLEo", 4))
#define FREE(X)         if(X) { free(X); X = NULL; }
#define MEMMOVE_SIZE    16      // amount of additional bytes to allocate for performing memmove operations without allocating new buffer



enum {
    EXT_TTARCH,
    EXT_TTARCH2,
};



u32     ttarch_tot_idx  = 0,
        ttarch_chunks_b = 0,
        ttarch_rem      = 0,
        ttarch_chunksz  = 0x10000;
u64     ttarch_offset   = 0,
        ttarch_baseoff  = 0,
        *ttarch_chunks  = NULL;

u32     extracted_files = 0,
        xmode           = 1,
        version         = DEFAULT_VERSION;
int     list_only       = 0,
        force_overwrite = 0,
        meta_extract    = 0,
        old_mode        = 0,    // boring old mode
        verbose         = 0,
        ttgtools_fix    = 0,    // the current ttgtools is not able to handle them, even with the work-around
        gamenum         = 0;
u64     g_dbg_offset    = 0;
u8      *filter_files   = NULL,
        *mykey          = NULL,
        *dump_table     = NULL;

typedef struct {
    u8      *name;
    u64     offset; // unused at the moment
    u64     size;
    u64     name_crc;
} files_t;
typedef struct {
    int     old_mode;   // use 2 for old_mode+encryption
    u8      *key;
    u8      *name;
    int     extension;
    int     version;    // currently unused
} gamekeys_t;
gamekeys_t  gamekeys[] = {
    //{ 0, "\x96\xCA\x99\xA0\x85\xCF\x98\x8A\xE4\xDB\xE2\xCD\xA6\xA9\xB8\xC4\xD8\x81\x8E\xE3\xA9\xD6\x99\xD4\xAC\x93\xDE\xC2\xA2\xE0\xA5\x99\xCC\xB4\x89\xB0\xCD\xCF\x9A\xCF\xDA\xBB\xEA\xDB\xA4\x9B\xC6\xB3\xAA\xD0\x95\x8A\xD1\xDE\xAB", "Wallace & Gromit demo", EXT_TTARCH, -1 },
    { 0, "\x96\xCA\x99\xA0\x85\xCF\x98\x8A\xE4\xDB\xE2\xCD\xA6\x96\x83\x88\xC0\x8B\x99\xE3\x9E\xD8\x9B\xB6\xD7\x90\xDC\xBE\xAD\x9D\x91\x65\xB6\xA6\x9E\xBB\xC2\xC6\x9E\xB3\xE7\xE3\xE5\xD5\xAB\x63\x82\xA0\x9C\xC4\x92\x9F\xD1\xD5\xA4", "Wallace & Gromit: Episode 1: Fright of the Bumblebees", EXT_TTARCH, -1 },
    { 0, "\x96\xCA\x99\xA0\x85\xCF\x98\x8A\xE4\xDB\xE2\xCD\xA6\x96\x83\x89\xC0\x8B\x99\xE3\x9E\xD8\x9B\xB6\xD7\x90\xDC\xBE\xAD\x9D\x91\x66\xB6\xA6\x9E\xBB\xC2\xC6\x9E\xB3\xE7\xE3\xE5\xD5\xAB\x63\x82\xA1\x9C\xC4\x92\x9F\xD1\xD5\xA4", "Wallace & Gromit: Episode 2: The Last Resort", EXT_TTARCH, -1 },
    { 0, "\x96\xCA\x99\xA0\x85\xCF\x98\x8A\xE4\xDB\xE2\xCD\xA6\x96\x83\x8A\xC0\x8B\x99\xE3\x9E\xD8\x9B\xB6\xD7\x90\xDC\xBE\xAD\x9D\x91\x67\xB6\xA6\x9E\xBB\xC2\xC6\x9E\xB3\xE7\xE3\xE5\xD5\xAB\x63\x82\xA2\x9C\xC4\x92\x9F\xD1\xD5\xA4", "Wallace & Gromit: Episode 3: Muzzled", EXT_TTARCH, -1 },
    { 0, "\x8f\xd8\x98\x99\x96\xbc\xa2\xae\xd7\xde\xc5\xd3\x9d\xca\xc5\xa7\xd8\x95\x92\xe9\x8d\xe4\xa1\xd4\xd7\x71\xde\xc0\x9e\xde\xb1\xa3\xca\xaa\xa4\x9f\xd0\xce\x9e\xde\xc5\xe3\xe3\xd1\xa9\x82\xc1\xda\xaa\xd5\x76\xa2\xdb\xd7\xb1", "Telltale Texas Hold'em", EXT_TTARCH, -1 },
    { 0, "\x81\xd8\x9b\x99\x55\xe2\x65\x73\xb4\xdb\xe3\xc9\x63\xdb\x85\x87\xab\x99\x9b\xdc\x6e\xeb\x68\x9f\xa7\x90\xdd\xba\x6a\xe2\x93\x64\xa1\xb4\xa0\xb4\x92\xd9\x6b\x9c\xb7\xe3\xe6\xd1\x68\xa8\x84\x9f\x87\xd2\x94\x98\xa1\xe8\x71", "Bone: Out From Boneville", EXT_TTARCH, -1 },
    { 0, "\x81\xD8\x9B\x99\x56\xE2\x65\x73\xB4\xDB\xE3\xC9\x64\xDB\x85\x87\xAB\x99\x9B\xDC\x6F\xEB\x68\x9F\xA7\x90\xDD\xBA\x6B\xE2\x93\x64\xA1\xB4\xA0\xB4\x93\xD9\x6B\x9C\xB7\xE3\xE6\xD1\x69\xA8\x84\x9F\x87\xD2\x94\x98\xA2\xE8\x71", "Bone: The Great Cow Race", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x64\x73\xA3\xBF\xD6\xD1\x7F\xC6\xCB\x88\x99\x5B\x80\xD8\xAA\xC2\x97\xE7\x96\x51\xA0\xA8\x9A\xD9\xAE\x95\xD7\x76\x62\x80\xB4\xC4\xA6\xB9\xD6\xEC\xA9\x9C\x68\x85\xB3\xDC\x92\xC4\x9E\x64\xA0\xA3\x92", "Sam & Max: Episode 101 - Culture Shock", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x64\x73\xA4\xBF\xD6\xD1\x7F\xC6\xCB\x88\x99\x01\x80\xD8\xAA\xC2\x97\xE7\x96\x51\xA1\xA8\x9A\xD9\xAE\x95\xD7\x76\x62\x81\xB4\xC4\xA6\xB9\xD6\xEC\xA9\x9C\x69\x85\xB3\xDC\x92\xC4\x9E\x64\xA0\xA4\x92", "Sam & Max: Episode 102 - Situation: Comedy", EXT_TTARCH, -1 },
    { 0, "\x92\xca\x9a\x81\x85\xe4\x64\x73\xa5\xbf\xd6\xd1\x7f\xc6\xcb\x88\x99\x5d\x80\xd8\xaa\xc2\x97\xe7\x96\x51\xa2\xa8\x9a\xd9\xae\x95\xd7\x76\x62\x82\xb4\xc4\xa6\xb9\xd6\xec\xa9\x9c\x6a\x85\xb3\xdc\x92\xc4\x9e\x64\xa0\xa5\x92", "Sam & Max: Episode 103 - The Mole, The Mob, and the Meatball", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x64\x73\xA6\xBF\xD6\xD1\x7F\xC6\xCB\x88\x99\x5E\x80\xD8\xAA\xC2\x97\xE7\x96\x51\xA3\xA8\x9A\xD9\xAE\x95\xD7\x76\x62\x83\xB4\xC4\xA6\xB9\xD6\xEC\xA9\x9C\x6B\x85\xB3\xDC\x92\xC4\x9E\x64\xA0\xA6\x92", "Sam & Max: Episode 104 - Abe Lincoln Must Die!", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x64\x73\xA7\xBF\xD6\xD1\x7F\xC6\xCB\x88\x99\x5F\x80\xD8\xAA\xC2\x97\xE7\x96\x51\xA4\xA8\x9A\xD9\xAE\x95\xD7\x76\x62\x84\xB4\xC4\xA6\xB9\xD6\xEC\xA9\x9C\x6C\x85\xB3\xDC\x92\xC4\x9E\x64\xA0\xA7\x92", "Sam & Max: Episode 105 - Reality 2.0", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x64\x73\xA8\xBF\xD6\xD1\x7F\xC6\xCB\x88\x99\x60\x80\xD8\xAA\xC2\x97\xE7\x96\x51\xA5\xA8\x9A\xD9\xAE\x95\xD7\x76\x62\x85\xB4\xC4\xA6\xB9\xD6\xEC\xA9\x9C\x6D\x85\xB3\xDC\x92\xC4\x9E\x64\xA0\xA8\x92", "Sam & Max: Episode 106 - Bright Side of the Moon", EXT_TTARCH, -1 },
    { 0, "\x92\xca\x9a\x81\x85\xe4\x65\x73\xa3\xbf\xd6\xd1\x7f\xc6\xcb\x89\x99\x5b\x80\xd8\xaa\xc2\x97\xe7\x97\x51\xa0\xa8\x9a\xd9\xae\x95\xd7\x77\x62\x80\xb4\xc4\xa6\xb9\xd6\xec\xaa\x9c\x68\x85\xb3\xdc\x92\xc4\x9e\x65\xa0\xa3\x92", "Sam & Max: Episode 201 - Ice Station Santa", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x65\x73\xA4\xBF\xD6\xD1\x7F\xC6\xCB\x89\x99\x01\x80\xD8\xAA\xC2\x97\xE7\x97\x51\xA1\xA8\x9A\xD9\xAE\x95\xD7\x77\x62\x81\xB4\xC4\xA6\xB9\xD6\xEC\xAA\x9C\x69\x85\xB3\xDC\x92\xC4\x9E\x65\xA0\xA4\x92", "Sam & Max: Episode 202 - Moai Better Blues", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x65\x73\xA5\xBF\xD6\xD1\x7F\xC6\xCB\x89\x99\x5D\x80\xD8\xAA\xC2\x97\xE7\x97\x51\xA2\xA8\x9A\xD9\xAE\x95\xD7\x77\x62\x82\xB4\xC4\xA6\xB9\xD6\xEC\xAA\x9C\x6A\x85\xB3\xDC\x92\xC4\x9E\x65\xA0\xA5\x92", "Sam & Max: Episode 203 - Night of the Raving Dead", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x65\x73\xA6\xBF\xD6\xD1\x7F\xC6\xCB\x89\x99\x5E\x80\xD8\xAA\xC2\x97\xE7\x97\x51\xA3\xA8\x9A\xD9\xAE\x95\xD7\x77\x62\x83\xB4\xC4\xA6\xB9\xD6\xEC\xAA\x9C\x6B\x85\xB3\xDC\x92\xC4\x9E\x65\xA0\xA6\x92", "Sam & Max: Episode 204 - Chariots of the Dogs", EXT_TTARCH, -1 },
    { 0, "\x92\xca\x9a\x81\x85\xe4\x65\x73\xa7\xbf\xd6\xd1\x7f\xc6\xcb\x89\x99\x5f\x80\xd8\xaa\xc2\x97\xe7\x97\x51\xa4\xa8\x9a\xd9\xae\x95\xd7\x77\x62\x84\xb4\xc4\xa6\xb9\xd6\xec\xaa\x9c\x6c\x85\xb3\xdc\x92\xc4\x9e\x65\xa0\xa7\x92", "Sam & Max: Episode 205 - What's New, Beelzebub", EXT_TTARCH, -1 },
    { 0, "\x87\xD8\x9A\x99\x97\xE0\x94\xB5\xA3\x9C\xA6\xAC\xA1\xD2\xB8\xCA\xDD\x8B\x9F\xA8\x6D\xA6\x7E\xDE\xD2\x86\xE2\xC9\x9A\xDE\x92\x64\x90\x8D\xA1\xBC\xC6\xD6\xAD\xCD\xE7\xA5\xA8\x9D\x7F\xA1\xBF\xD4\xB8\xD7\x87\xA5\xA1\xA2\x70", "Strong Bad: Episode 1 - Homestar Ruiner", EXT_TTARCH, -1 },
    { 0, "\x87\xd8\x9a\x99\x97\xe0\x94\xb5\xa3\x9c\xa7\xac\xa1\xd2\xb8\xca\xdd\x8b\x9f\xa8\x6d\xa7\x7e\xde\xd2\x86\xe2\xc9\x9a\xde\x92\x64\x91\x8d\xa1\xbc\xc6\xd6\xad\xcd\xe7\xa5\xa8\x9e\x7f\xa1\xbf\xd4\xb8\xd7\x87\xa5\xa1\xa2\x71", "Strong Bad: Episode 2 - Strong Badia the Free", EXT_TTARCH, -1 },
    { 0, "\x87\xD8\x9A\x99\x97\xE0\x94\xB5\xA3\x9C\xA8\xAC\xA1\xD2\xB8\xCA\xDD\x8B\x9F\xA8\x6D\xA8\x7E\xDE\xD2\x86\xE2\xC9\x9A\xDE\x92\x64\x92\x8D\xA1\xBC\xC6\xD6\xAD\xCD\xE7\xA5\xA8\x9F\x7F\xA1\xBF\xD4\xB8\xD7\x87\xA5\xA1\xA2\x72", "Strong Bad: Episode 3 - Baddest of the Bands", EXT_TTARCH, -1 },
    { 0, "\x87\xd8\x9a\x99\x97\xe0\x94\xb5\xa3\x9c\xa9\xac\xa1\xd2\xb8\xca\xdd\x8b\x9f\xa8\x6d\xa9\x7e\xde\xd2\x86\xe2\xc9\x9a\xde\x92\x64\x93\x8d\xa1\xbc\xc6\xd6\xad\xcd\xe7\xa5\xa8\xa0\x7f\xa1\xbf\xd4\xb8\xd7\x87\xa5\xa1\xa2\x73", "Strong Bad: Episode 4 - Daneresque 3", EXT_TTARCH, -1 },
    { 0, "\x87\xd8\x9a\x99\x97\xe0\x94\xb5\xa3\x9c\xaa\xac\xa1\xd2\xb8\xca\xdd\x8b\x9f\xa8\x6d\xaa\x7e\xde\xd2\x86\xe2\xc9\x9a\xde\x92\x64\x94\x8d\xa1\xbc\xc6\xd6\xad\xcd\xe7\xa5\xa8\xa1\x7f\xa1\xbf\xd4\xb8\xd7\x87\xa5\xa1\xa2\x74", "Strong Bad: Episode 5 - 8-Bit Is Enough", EXT_TTARCH, -1 },
    { 1, "\x34\x24\x6C\x33\x43\x72\x6C\x75\x64\x32\x65\x53\x57\x69\x45\x32\x4F\x61\x63\x39\x6C\x75\x74\x78\x6C\x37\x32\x52\x2D\x2A\x38\x49\x31\x71\x4F\x34\x6F\x61\x6A\x6C\x5F\x24\x65\x23\x69\x61\x63\x70\x34\x2A\x75\x46\x6C\x65\x30", "CSI 3 - Dimensions of Murder / Bone demo", EXT_TTARCH, -1 },
    { 0, "\x82\xBC\x76\x68\x83\xB0\x78\x90\xC1\xAF\xC8\xAD\x66\xC4\x97\x9C\xB6\x79\x70\xCA\x86\xA9\x95\xB3\xAA\x6E\xBE\x98\x8C\xB5\x95\x93\xA3\x8A\x7F\x9E\xA4\xB6\x82\xA0\xD4\xB8\xBD\xB9\x86\x75\xA5\xB8\x79\xC2\x6A\x78\xBD\xC1\x82", "CSI 4 - Hard Evidence (demo)", EXT_TTARCH, -1 },
    { 0, "\x8C\xD8\x9B\x9F\x89\xE5\x7C\xB6\xDE\xCD\xE3\xC8\x63\x95\x84\xA4\xD8\x98\x98\xDC\xB6\xBE\xA9\xDB\xC6\x8F\xD3\x86\x69\x9D\xAE\xA3\xCD\xB0\x97\xC8\xAA\xD6\xA5\xCD\xE3\xD8\xA9\x9C\x68\x7F\xC1\xDD\xB0\xC8\x9F\x7C\xE3\xDE\xA0", "Tales of Monkey Island 101: Launch of the Screaming Narwhal", EXT_TTARCH, -1 },
    { 0, "\x96\xCA\x99\xA0\x85\xCF\x98\x8A\xE4\xDB\xE2\xCD\xA6\x96\x83\x8B\xC0\x8B\x99\xE3\x9E\xD8\x9B\xB6\xD7\x90\xDC\xBE\xAD\x9D\x91\x68\xB6\xA6\x9E\xBB\xC2\xC6\x9E\xB3\xE7\xE3\xE5\xD5\xAB\x63\x82\xA3\x9C\xC4\x92\x9F\xD1\xD5\xA4", "Wallace & Gromit: Episode 4: The Bogey Man", EXT_TTARCH, -1 },
    { 0, "\x8C\xD8\x9B\x9F\x89\xE5\x7C\xB6\xDE\xCD\xE3\xC8\x63\x95\x85\xA4\xD8\x98\x98\xDC\xB6\xBE\xA9\xDB\xC6\x8F\xD3\x86\x69\x9E\xAE\xA3\xCD\xB0\x97\xC8\xAA\xD6\xA5\xCD\xE3\xD8\xA9\x9C\x69\x7F\xC1\xDD\xB0\xC8\x9F\x7C\xE3\xDE\xA0", "Tales of Monkey Island 102: The Siege of Spinner Cay", EXT_TTARCH, -1 },
    { 0, "\x8C\xD8\x9B\x9F\x89\xE5\x7C\xB6\xDE\xCD\xE3\xC8\x63\x95\x86\xA4\xD8\x98\x98\xDC\xB6\xBE\xA9\xDB\xC6\x8F\xD3\x86\x69\x9F\xAE\xA3\xCD\xB0\x97\xC8\xAA\xD6\xA5\xCD\xE3\xD8\xA9\x9C\x6A\x7F\xC1\xDD\xB0\xC8\x9F\x7C\xE3\xDE\xA0", "Tales of Monkey Island 103: Lair of the Leviathan", EXT_TTARCH, -1 },
    { 0, "\x82\xBC\x76\x69\x54\x9C\x86\x90\xD7\xDA\xEA\xA7\x85\xAE\x88\x87\x99\x7D\x7A\xDC\xAB\xEA\x79\xC2\xAE\x56\x9F\x85\x8C\xB9\xC6\xA2\xD4\x88\x85\x98\x96\x93\x69\xBF\xC2\xD9\xE6\xE1\x7A\x85\x9B\xA4\x75\x93\x79\x80\xD5\xE0\xB4", "CSI 5 - Deadly Intent", EXT_TTARCH, -1 },
    { 0, "\x8c\xd8\x9b\x9f\x89\xe5\x7c\xb6\xde\xcd\xe3\xc8\x63\x95\x87\xa4\xd8\x98\x98\xdc\xb6\xbe\xa9\xdb\xc6\x8f\xd3\x86\x69\xa0\xae\xa3\xcd\xb0\x97\xc8\xaa\xd6\xa5\xcd\xe3\xd8\xa9\x9c\x6b\x7f\xc1\xdd\xb0\xc8\x9f\x7c\xe3\xde\xa0", "Tales of Monkey Island 104: The Trial and Execution of Guybrush Threepwood", EXT_TTARCH, -1 },
    { 0, "\x82\xbc\x76\x68\x67\xbf\x7c\x77\xb5\xbf\xbe\x98\x75\xb8\x9c\x8b\xac\x7d\x76\xab\x80\xc8\x7f\xa3\xa8\x74\xb8\x89\x7c\xbf\xaa\x68\xa2\x98\x7b\x83\xa4\xb6\x82\xa0\xb8\xc7\xc1\xa0\x7a\x85\x9b\xa3\x88\xb6\x6f\x67\xb3\xc5\x88", "CSI 4 - Hard Evidence", EXT_TTARCH, -1 },
    { 0, "\x8c\xd8\x9b\x9f\x89\xe5\x7c\xb6\xde\xcd\xe3\xc8\x63\x95\x88\xa4\xd8\x98\x98\xdc\xb6\xbe\xa9\xdb\xc6\x8f\xd3\x86\x69\xa1\xae\xa3\xcd\xb0\x97\xc8\xaa\xd6\xa5\xcd\xe3\xd8\xa9\x9c\x6c\x7f\xc1\xdd\xb0\xc8\x9f\x7c\xe3\xde\xa0", "Tales of Monkey Island 105: Rise of the Pirate God", EXT_TTARCH, -1 },
    { 0, "\x82\xBC\x76\x69\x68\xD1\xA0\xB2\xB5\xBF\xBE\x99\x76\xCA\xC0\xC6\xAC\x7D\x76\xAC\x81\xDA\xA3\xDE\xA8\x74\xB8\x8A\x7D\xD1\xCE\xA3\xA2\x98\x7B\x84\xA5\xC8\xA6\xDB\xB8\xC7\xC1\xA1\x7B\x97\xBF\xDE\x88\xB6\x6F\x68\xB4\xD7\xAC", "CSI 5 - Deadly Intent (demo)", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x66\x73\xA3\xBF\xD6\xD1\x7F\xC6\xCB\x8A\x99\x5B\x80\xD8\xAA\xC2\x97\xE7\x98\x51\xA0\xA8\x9A\xD9\xAE\x95\xD7\x78\x62\x80\xB4\xC4\xA6\xB9\xD6\xEC\xAB\x9C\x68\x85\xB3\xDC\x92\xC4\x9E\x66\xA0\xA3\x92", "Sam & Max: Episode 301 - The Penal Zone", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x66\x73\xA4\xBF\xD6\xD1\x7F\xC6\xCB\x8A\x99\x01\x80\xD8\xAA\xC2\x97\xE7\x98\x51\xA1\xA8\x9A\xD9\xAE\x95\xD7\x78\x62\x81\xB4\xC4\xA6\xB9\xD6\xEC\xAB\x9C\x69\x85\xB3\xDC\x92\xC4\x9E\x66\xA0\xA4\x92", "Sam & Max: Episode 302 - The Tomb of Sammun-Mak", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x66\x73\xA5\xBF\xD6\xD1\x7F\xC6\xCB\x8A\x99\x5D\x80\xD8\xAA\xC2\x97\xE7\x98\x51\xA2\xA8\x9A\xD9\xAE\x95\xD7\x78\x62\x82\xB4\xC4\xA6\xB9\xD6\xEC\xAB\x9C\x6A\x85\xB3\xDC\x92\xC4\x9E\x66\xA0\xA5\x92", "Sam & Max: Episode 303 - They Stole Max's Brain!", EXT_TTARCH, -1 },
    { 0, "\x86\xDB\x96\x97\x8F\xD8\x98\x74\xA2\x9D\xBC\xD6\x9B\xC8\xBE\xC3\xCE\x5B\x5D\xA8\x84\xE7\x9F\xD2\xD0\x8D\xD4\x86\x69\x9D\xA8\xA6\xC8\xA8\x9D\xBB\xC6\x94\x69\x9D\xBC\xE6\xE1\xCF\xA2\x9E\xB7\xA0\x75\x94\x6D\xA5\xD9\xD5\xAA", "Puzzle Agent - The Mystery of Scoggins", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x66\x73\xA6\xBF\xD6\xD1\x7F\xC6\xCB\x8A\x99\x5E\x80\xD8\xAA\xC2\x97\xE7\x98\x51\xA3\xA8\x9A\xD9\xAE\x95\xD7\x78\x62\x83\xB4\xC4\xA6\xB9\xD6\xEC\xAB\x9C\x6B\x85\xB3\xDC\x92\xC4\x9E\x66\xA0\xA6\x92", "Sam & Max: Episode 304 - Beyond the Alley of the Dolls", EXT_TTARCH, -1 },
    { 0, "\x92\xCA\x9A\x81\x85\xE4\x66\x73\xA7\xBF\xD6\xD1\x7F\xC6\xCB\x8A\x99\x5F\x80\xD8\xAA\xC2\x97\xE7\x98\x51\xA4\xA8\x9A\xD9\xAE\x95\xD7\x78\x62\x84\xB4\xC4\xA6\xB9\xD6\xEC\xAB\x9C\x6C\x85\xB3\xDC\x92\xC4\x9E\x66\xA0\xA7\x92", "Sam & Max: Episode 305 - The City That Dares Not Sleep", EXT_TTARCH, -1 },
    { 0, "\x82\xCE\x99\x99\x86\xDE\x9C\xB7\xEB\xBC\xE4\xCF\x97\xD7\x96\xBC\xD5\x8F\x8F\xE9\xA6\xE9\xAF\xBF\xD4\x8C\xD4\xC7\x7C\xD1\xCD\x99\xC1\xB7\x9B\xC3\xDA\xB3\xA8\xD7\xDA\xE6\xBB\xD1\xA3\x97\xB4\xE1\xAE\xD7\x9F\x83\xDF\xDD\xA4", "Poker Night at the Inventory", EXT_TTARCH, -1 },
    { 0, "\x82\xBC\x76\x6A\x54\x9C\x76\x96\xBB\xA2\xA5\x94\x75\xB8\x9C\x8D\x99\x5A\x70\xCA\x86\xAB\x66\x9F\xA8\x74\xB8\x8B\x69\x9C\xA4\x87\xA8\x7B\x62\x7F\xA4\xB6\x82\xA2\xA5\xA4\xBB\xBF\x80\x68\x82\x9F\x88\xB6\x6F\x69\xA0\xA2\x82", "CSI 6 - Fatal Conspiracy", EXT_TTARCH, -1 },
    { 0, "\x81\xCA\x90\x9F\x78\xDB\x87\xAB\xD7\xB2\xEA\xD8\xA7\xD7\xB8\x88\x99\x5B\x6F\xD8\xA0\xE0\x8A\xDE\xB9\x89\xD4\x9B\xAE\xE0\xD6\xA6\xC4\x76\x62\x80\xA3\xC4\x9C\xD7\xC9\xE3\xCC\xD4\x9C\x78\xC7\xE3\xBA\xD5\x8B\x64\xA0\xA3\x81", "Back To The Future: Episode 1 - It's About Time", EXT_TTARCH, -1 },
    { 0, "\x81\xca\x90\x9f\x78\xdb\x87\xab\xd7\xb2\xea\xd8\xa7\xd7\xb8\x88\x99\x01\x6f\xd8\xa0\xe0\x8a\xde\xb9\x89\xd4\x9b\xae\xe0\xd6\xa6\xc4\x76\x62\x81\xa3\xc4\x9c\xd7\xc9\xe3\xcc\xd4\x9c\x78\xc7\xe3\xba\xd5\x8b\x64\xa0\xa4\x81", "Back To The Future: Episode 2 - Get Tannen!", EXT_TTARCH, -1 },
    { 0, "\x81\xCA\x90\x9F\x78\xDB\x87\xAB\xD7\xB2\xEA\xD8\xA7\xD7\xB8\x88\x99\x5D\x6F\xD8\xA0\xE0\x8A\xDE\xB9\x89\xD4\x9B\xAE\xE0\xD6\xA6\xC4\x76\x62\x82\xA3\xC4\x9C\xD7\xC9\xE3\xCC\xD4\x9C\x78\xC7\xE3\xBA\xD5\x8B\x64\xA0\xA5\x81", "Back To The Future: Episode 3 - Citizen Brown", EXT_TTARCH, -1 },
    { 0, "\x87\xCE\x90\xA8\x93\xDE\x64\x73\xA3\xB4\xDA\xC7\xA6\xD4\xC5\x88\x99\x5B\x75\xDC\xA0\xE9\xA5\xE1\x96\x51\xA0\x9D\x9E\xCF\xD5\xA3\xD1\x76\x62\x80\xA9\xC8\x9C\xE0\xE4\xE6\xA9\x9C\x68\x7A\xB7\xD2\xB9\xD2\x98\x64\xA0\xA3\x87", "Hector: Episode 1 - We Negotiate with Terrorists", EXT_TTARCH, -1 },
    { 0, "\x81\xCA\x90\x9F\x78\xDB\x87\xAB\xD7\xB2\xEA\xD8\xA7\xD7\xB8\x88\x99\x5E\x6F\xD8\xA0\xE0\x8A\xDE\xB9\x89\xD4\x9B\xAE\xE0\xD6\xA6\xC4\x76\x62\x83\xA3\xC4\x9C\xD7\xC9\xE3\xCC\xD4\x9C\x78\xC7\xE3\xBA\xD5\x8B\x64\xA0\xA6\x81", "Back To The Future: Episode 4 - Double Visions", EXT_TTARCH, -1 },
    { 0, "\x81\xCA\x90\x9F\x78\xDB\x87\xAB\xD7\xB2\xEA\xD8\xA7\xD7\xB8\x88\x99\x5F\x6F\xD8\xA0\xE0\x8A\xDE\xB9\x89\xD4\x9B\xAE\xE0\xD6\xA6\xC4\x76\x62\x84\xA3\xC4\x9C\xD7\xC9\xE3\xCC\xD4\x9C\x78\xC7\xE3\xBA\xD5\x8B\x64\xA0\xA7\x81", "Back To The Future: Episode 5 - OUTATIME", EXT_TTARCH, -1 },
    { 0, "\x86\xDB\x96\x97\x8F\xD8\x98\x74\xA2\x9E\xBC\xD6\x9B\xC8\xBE\xC3\xCE\x5B\x5D\xA9\x84\xE7\x9F\xD2\xD0\x8D\xD4\x86\x69\x9E\xA8\xA6\xC8\xA8\x9D\xBB\xC6\x94\x69\x9E\xBC\xE6\xE1\xCF\xA2\x9E\xB7\xA0\x75\x95\x6D\xA5\xD9\xD5\xAA", "Puzzle Agent 2", EXT_TTARCH, -1 },
    { 0, "\x89\xde\x9f\x95\x97\xdf\x9c\xa6\xc2\xcd\xe7\xcf\x63\x95\x83\xa1\xde\x9c\x8e\xea\xb0\xde\x99\xbf\xc6\x93\xda\x86\x69\x9c\xab\xa9\xd1\xa6\xa5\xc2\xca\xc6\x89\xcd\xe7\xdf\xa9\x9c\x67\x7c\xc7\xe1\xa6\xd6\x99\x9c\xd3\xc2\xa0", "Jurassik Park: The Game", EXT_TTARCH, -1 },
    { 0, "\x87\xCE\x90\xA8\x93\xDE\x64\x73\xA4\xB4\xDA\xC7\xA6\xD4\xC5\x88\x99\x01\x75\xDC\xA0\xE9\xA5\xE1\x96\x51\xA1\x9D\x9E\xCF\xD5\xA3\xD1\x76\x62\x81\xA9\xC8\x9C\xE0\xE4\xE6\xA9\x9C\x69\x7A\xB7\xD2\xB9\xD2\x98\x64\xA0\xA4\x87", "Hector: Episode 2 - Senseless Act of Justice", EXT_TTARCH, -1 },
    { 0, "\x87\xCE\x90\xA8\x93\xDE\x64\x73\xA5\xB4\xDA\xC7\xA6\xD4\xC5\x88\x99\x5D\x75\xDC\xA0\xE9\xA5\xE1\x96\x51\xA2\x9D\x9E\xCF\xD5\xA3\xD1\x76\x62\x82\xA9\xC8\x9C\xE0\xE4\xE6\xA9\x9C\x6A\x7A\xB7\xD2\xB9\xD2\x98\x64\xA0\xA5\x87", "Hector: Episode 3 - Beyond Reasonable Doom", EXT_TTARCH, -1 },
    { 0, "\x8B\xCA\xA4\x75\x92\xD0\x82\xB5\xD6\xD1\xE7\x95\x62\x95\x9F\xB8\xE0\x6B\x9B\xDB\x8C\xE7\x9A\xD4\xD7\x52\x9F\x85\x85\xCD\xD8\x75\xCD\xA9\x81\xC1\xC5\xC8\xAB\x9D\xA5\xA4\xC4\xCD\xAE\x73\xC0\xD3\x94\xD5\x8A\x98\xE2\xA3\x6F", "Law and Order: Legacies", EXT_TTARCH, -1 },
    { 0, "\x96\xca\x99\x9f\x8d\xda\x9a\x87\xd7\xcd\xd9\x95\x62\x95\xaa\xb8\xd5\x95\x96\xe5\xa4\xb9\x9b\xd0\xc9\x52\x9f\x85\x90\xcd\xcd\x9f\xc8\xb3\x99\x93\xc6\xc4\x9d\x9d\xa5\xa4\xcf\xcd\xa3\x9d\xbb\xdd\xac\xa7\x8b\x94\xd4\xa3\x6f", "Walking Dead: A New Day", EXT_TTARCH, -1 },
    { 0, "\x82\xCE\x99\x99\x86\xDE\x9C\xB7\xEB\xBC\xE4\xCF\x97\xD7\x85\x9A\xCE\x96\x92\xD9\xAF\xDE\xAA\xE8\xB5\x90\xDA\xBA\xAB\x9E\xA4\x99\xCB\xAA\x94\xC1\xCA\xD7\xB2\xBC\xE4\xDF\xDD\xDE\x69\x75\xB7\xDB\xAA\xC5\x98\x9C\xE4\xEB\x8F", "Poker Night 2", EXT_TTARCH, -1 },
    { 0, "\x85\xca\x8f\xa0\x89\xdf\x64\x73\xa2\xb2\xd6\xc6\x9e\xca\xc6\x88\x99\x5a\x73\xd8\x9f\xe1\x9b\xe2\x96\x51\x9f\x9b\x9a\xce\xcd\x99\xd2\x76\x62\x7f\xa7\xc4\x9b\xd8\xda\xe7\xa9\x9c\x67\x78\xb3\xd1\xb1\xc8\x99\x64\xa0\xa2\x85", "The Wolf Among Us", EXT_TTARCH2, -1 },
    { 0, "\x96\xCA\x99\x9F\x8D\xDA\x9A\x87\xD7\xCD\xD9\x96\x62\x95\xAA\xB8\xD5\x95\x96\xE5\xA4\xB9\x9B\xD0\xC9\x53\x9F\x85\x90\xCD\xCD\x9F\xC8\xB3\x99\x93\xC6\xC4\x9D\x9E\xA5\xA4\xCF\xCD\xA3\x9D\xBB\xDD\xAC\xA7\x8B\x94\xD4\xA4\x6F", "The Walking Dead: Season 2", EXT_TTARCH2, -1 },
    { 0, "\x81\xD8\x9F\x98\x89\xDE\x9F\xA4\xE0\xD0\xE8\x95\x62\x95\x95\xC6\xDB\x8E\x92\xE9\xA9\xD6\xA4\xD3\xD8\x52\x9F\x85\x7B\xDB\xD3\x98\xC4\xB7\x9E\xB0\xCF\xC7\xAC\x9D\xA5\xA4\xBA\xDB\xA9\x96\xB7\xE1\xB1\xC4\x94\x97\xE3\xA3\x6F", "Tales from the Borderlands (all episodes)", EXT_TTARCH2, -1 },
    { 0, "\x86\xCA\x9A\x99\x73\xD2\x87\xAB\xE4\xDB\xE3\xC9\xA5\x96\x83\x87\xB0\x8B\x9A\xDC\x8C\xDB\x8A\xD7\xD7\x90\xDD\xBA\xAC\x9D\x91\x64\xA6\xA6\x9F\xB4\xB0\xC9\x8D\xD4\xE7\xE3\xE6\xD1\xAA\x63\x82\x9F\x8C\xC4\x93\x98\xBF\xD8\x93", "Game of Thrones (all episodes)", EXT_TTARCH2, -1 },
    { 0, "\x8c\xd2\x9b\x99\x87\xde\x94\xa9\xe6\x9d\xa5\x94\x7f\xce\xc1\xbc\xcc\x9c\x8e\xdd\xb1\xa6\x66\x9f\xb2\x8a\xdd\xba\x9c\xde\xc2\x9a\xd3\x76\x62\x7f\xae\xcc\xa7\xd1\xd8\xe6\xd9\xd2\xab\x63\x82\x9f\x92\xcc\x94\x98\xd3\xe4\xa0", "Minecraft: Story Mode", EXT_TTARCH2, -1 },
    { 0, "\x96\xca\x99\x9f\x8d\xda\x9a\x87\xd7\xcd\xd9\xb1\x63\x95\x83\xae\xca\x96\x98\xe0\xab\xdc\x7a\xd4\xc6\x85\xbc\x86\x69\x9c\xb8\x95\xcb\xb0\x9b\xbd\xc8\xa7\x9e\xcd\xd9\xc1\xa9\x9c\x67\x89\xb3\xdb\xb0\xcc\x94\x9a\xb4\xd7\xa0", "The Walking Dead: Michonne", EXT_TTARCH2, -1 },
    { 0, "\x81\xca\xa1\xa1\x85\xda\x64\x73\xa2\xae\xd6\xd8\x9f\xc6\xc1\x88\x99\x5a\x6f\xd8\xb1\xe2\x97\xdd\x96\x51\x9f\x97\x9a\xe0\xce\x95\xcd\x76\x62\x7f\xa3\xc4\xad\xd9\xd6\xe2\xa9\x9c\x67\x74\xb3\xe3\xb2\xc4\x94\x64\xa0\xa2\x81", "Batman: The Telltale Series", EXT_TTARCH2, -1 },
    { 0, "\x96\xca\x99\x9f\x8d\xda\x9a\x87\xd7\xcd\xd9\x97\x62\x95\xaa\xb8\xd5\x95\x96\xe5\xa4\xb9\x9b\xd0\xc9\x54\x9f\x85\x90\xcd\xcd\x9f\xc8\xb3\x99\x93\xc6\xc4\x9d\x9f\xa5\xa4\xcf\xcd\xa3\x9d\xbb\xdd\xac\xa7\x8b\x94\xd4\xa5\x6f", "The Walking Dead: A New Frontier", EXT_TTARCH2, -1 },
    { 0, "\x86\xDE\x8E\xA6\x88\xD5\x94\xB1\xE5\x9D\xA5\x94\x79\xDA\xB4\xC9\xCD\x93\x8E\xE5\xB0\xA6\x66\x9F\xAC\x96\xD0\xC7\x9D\xD5\xC2\xA2\xD2\x76\x62\x7F\xA8\xD8\x9A\xDE\xD9\xDD\xD9\xDA\xAA\x63\x82\x9F\x8C\xD8\x87\xA5\xD4\xDB\xA0", "Marvel's Guardians of the Galaxy", EXT_TTARCH2, -1 },
    { 0, "\x8c\xd2\x9b\x99\x87\xde\x94\xa9\xe6\x9e\xa5\x94\x7f\xce\xc1\xbc\xcc\x9c\x8e\xdd\xb1\xa7\x66\x9f\xb2\x8a\xdd\xba\x9c\xde\xc2\x9a\xd3\x77\x62\x7f\xae\xcc\xa7\xd1\xd8\xe6\xd9\xd2\xab\x64\x82\x9f\x92\xcc\x94\x98\xd3\xe4\xa0", "Minecraft: Story Mode - Season Two", EXT_TTARCH2, -1 },
    { 0, "\x81\xCA\xA1\xA1\x85\xDA\x65\x73\xA2\xAE\xD6\xD8\x9F\xC6\xC1\x89\x99\x5A\x6F\xD8\xB1\xE2\x97\xDD\x97\x51\x9F\x97\x9A\xE0\xCE\x95\xCD\x77\x62\x7F\xA3\xC4\xAD\xD9\xD6\xE2\xAA\x9C\x67\x74\xB3\xE3\xB2\xC4\x94\x65\xA0\xA2\x81", "Batman: The Enemy Within", EXT_TTARCH2, -1 },
    { 2, "\x82\xa3\x89\x88\x89\xd8\x9f\xb7\xd3\xd8\xda\xc0\x82\xd7\xc2\xc1\xce\x8d\xa1\xea\x99\xb7\xa5\xdd\xca\x52\xe5\x87\x69\xc8\xa4\x6e\xbb\x99\x97\xbb\xcd\xd7\x9a\xd8\xda\xd0\xc8\xde\xa6\x9c\xb7\xd2\xb9\xd6\x82\x75\xdf\xe0\xa4", "Bone: Out From Boneville 2.0", EXT_TTARCH, -1 },
    { 2, "\x82\xa3\x89\x88\x89\xd8\x9f\xb7\xd3\xd8\xda\xc0\x82\xd7\xc2\xc1\xce\x8d\xa1\xea\x99\xb7\xa5\xdd\xca\x53\xe5\x87\x69\xc8\xa4\x6e\xbb\x99\x97\xbb\xcd\xd7\x9a\xd8\xda\xd0\xc8\xde\xa6\x9c\xb7\xd2\xb9\xd6\x82\x75\xdf\xe0\xa4", "Bone: The Great Cow Race 2.0", EXT_TTARCH, -1 },
    { 0, "\x96\xCA\x99\x9F\x8D\xDA\x9A\x87\xD7\xCD\xD9\xBB\x93\xD1\xBE\xC0\xD7\x91\x71\xDC\x9E\xD9\x8D\xD0\xD1\x8C\xD8\xC3\xA0\xB0\xC6\x95\xC3\x9C\x93\xBB\xCC\xCC\xA7\xD3\xB9\xD9\xD9\xD0\x8E\x93\xBE\xDA\xAE\xD1\x8D\x77\xD5\xD3\xA3", "The Walking Dead: The Telltale Definitive Series", EXT_TTARCH2, -1 },
    { 0, NULL, NULL, -1, -1 }
};



u64 ttarch2_hash(u64 crc, u8 *str);
u64 ttarch_import(FILE *fdo, u8 *fname);
u64 pad_it(u64 num, u64 pad);
u32 rebuild_it(u8 *output_name, FILE *fdo);
files_t *add_files(u8 *fname, u64 fsize, int *ret_files);
int recursive_dir(u8 *filedir);
u64 crypt_it(FILE *fd, u8 *fname, u64 offset, int wanted_size, int encrypt);
u8 *string2key(u8 *data);
int ttarch_extract(FILE *fd, u8 *input_fname);
int ttarch_meta_crypt(u8 *data, u64 datalen, int encrypt);
u64 ttarch_ftell(FILE *stream);
int ttarch_fseek(FILE *stream, u64 offset, int origin);
u64 ttarch_fgetxx(int bytes, FILE *stream);
u64 ttarch_fread(void *ptr, u64 size, FILE *stream);
void xor(u8 *data, u64 datalen, int xornum);
void blowfish(u8 *data, u64 datalen, int encrypt);
int mymemmove(u8 *dst, u8 *src, int size);
int ttarch_dumpa(u8 *fname, u8 *data, u64 size, int already_decrypted);
i64 _oodle_decompress(u8 *in, i64 insz, u8 *out, i64 outsz);
i64 _z_decompress(u8 *in, i64 insz, u8 *out, i64 outsz);
i64 (*do_decompress)(u8 *in, i64 insz, u8 *out, i64 outsz) = _z_decompress;
int check_wildcard(u8 *fname, u8 *wildcard);
u8 *create_dir(u8 *name);
int check_overwrite(u8 *fname);
void myalloc(u8 **data, u64 wantsize, u64 *currsize);
u64 getxx(u8 *tmp, int bytes);
u64 fgetxx(FILE *fd, int bytes);
u64 myfr(FILE *fd, u8 *data, u64 size);
int putxx(u8 *data, u64 num, int bytes);
int fputxx(FILE *fd, u64 num, int bytes);
void dumpa(u8 *fname, u8 *data, u64 size, u8 *more, int more_size);
u64 myfw(FILE *fd, u8 *data, u64 size);
u64 get_num(u8 *str);
void std_err(void);



int main(int argc, char *argv[]) {
    FILE    *fd;
    int     i,
            rebuild         = 0,
            decrypt_only    = -1,
            decrypt_onlysz  = -1,
            encrypt_only    = -1,
            encrypt_onlysz  = -1,
            force_old_mode  = 0;
    u8      *fname,
            *fdir,
            *p;

    //setbuf(stdout, NULL); // commented for going faster

    fputs("\n"
        "Telltale TTARCH files extractor/rebuilder " VER "\n"
        "by Luigi Auriemma\n"
        "e-mail: aluigi@autistici.org\n"
        "web:    aluigi.org\n"
        "\n", stdout);

    if(argc < 4) {
        printf("\n"
            "Usage: %s [options] <gamenum> <file.TTARCH> <output_folder>\n"
            "\n"
            "Options:\n"
            "-l      list the files without extracting them, you can use . as output folder\n"
            "-f W    filter the files to extract using the W wildcard, example -f \"*.mp3\"\n"
            "-o      if the output files already exist this option will overwrite them\n"
            "        automatically without asking the user's confirmation\n"
            "-m      automatically extract the data inside the meta files, for example the\n"
            "        DDS from FONT and D3DTX files and OGG from AUD and so on... USEFUL!\n"
            "-b      rebuild option, instead of extracting it performs the building of the\n"
            "        ttarch archive, example: ttarchext -b 24 output.ttarch input_folder\n"
            "        do NOT use -m in the extraction if you want to rebuild the archive!\n"
            "-k K    specify a custom key in hexadecimal (like 0011223344), use gamenum 0\n"
            "-d OFF  perform only the blowfish decryption of the input file from offset OFF\n"
            "-D O S  as above but the decryption is performed only on the part of file\n"
            "        from offset O for a total of S bytes, the rest will remain as is\n"
            "-e OFF  perform the blowfish encryption of the input file (debug)\n"
            "-E O S  as above but the encryption is performed only on the part of file\n"
            "        from offset O for a total of S bytes, the rest will remain as is\n"
            "-V VER  force a specific version number, needed ONLY with -d and -e when\n"
            "        handling archives of version 7 (like Tales of Monkey Island)\n"
            "-O      force the old mode format, sometimes needed with old archives\n"
            "-x      for versions >= 7 only in rebuild mode, if the game crashes when uses\n"
            "        the rebuilt archive try to rebuild it adding this -x option\n"
            "-T F    dump the decrypted name table in the file F (debug)\n"
            "-v      experimental verbose information\n"
            "\n", argv[0]);

        printf("Games (gamenum):\n");
        for(i = 0; gamekeys[i].name; i++) {
            printf(" %-3d %s\n", i, gamekeys[i].name);
        }
        printf("\n"
            "Examples:\n"
            "  ttarchext.exe -m 24 c:\\4_MonkeyIsland101_pc_tx.ttarch \"c:\\new folder\"\n"
            "  ttarchext.exe -l 24 c:\\4_MonkeyIsland101_pc_tx.ttarch \"c:\\new folder\"\n"
            "  ttarchext.exe -b -V 7 24 c:\\1_output.ttarch c:\\1_input_folder\n"
            "\n"
            "The tool can work also on single encrypted files like the prop files usually\n"
            "located in the main folder or the various aud files, examples:\n"
            "  ttarchext.exe -V 7 24 c:\\ttg_splash_a_telltale.d3dtx \"c:\\new folder\"\n"
            "  ttarchext.exe -V 7 -e 0 24 \"c:\\new folder\\ttg_splash_a_telltale.d3dtx\" c:\\\n"
            "\n"
            "Notes: from version 0.2 the tool performs the automatic decryption of lenc\n"
            "       files to Lua and vice versa during the rebuilding\n"
            "\n"
            "Additional notes copy&pasted from the website of the tool:\n"
            "\n"
            "remember to use the -m option to dump the FONT and D3DTX files as DDS and the AUD as OGG but do NOT use this option if you plan to rebuild the ttarch archive!.\n"
            "the tool has also various options for listing the files without extracting them, overwriting the existent files, wildcards and other options (mainly debug stuff for myself).\n"
            "examples for \"Tales of Monkey Island: Launch of the Screaming Narwhal\":\n"
            "\n"
            "  extraction: ttarchext.exe 24 \"C:\\...pc_launcheronly.ttarch\" c:\\output_folder\n"
            "  rebuilding: ttarchext.exe -b -V 7 24 \"C:\\...\\0.ttarch\" c:\\input_folder\n"
            "  decrypt lenc: ttarchext 55 c:\\input_file.lenc c:\\output_folder\n"
            "  encrypt lua: ttarchext -V 7 -e 0 55 c:\\input_file.lua c:\\output_folder\n"
            "\n"
            "remember that if you have modified only a couple of files (for example english.langdb and one or images) you don't need to rebuild the whole archive but it's enough to build a new one called 0.ttarch containing ONLY the files you modifed, it will be read by the game like a patch and will occupy only a minimal amount of space.\n"
            "note that the old versions of the TellTale games (so not those currently available on that website) are not supported because use different encryptions and sometimes format, and being old versions are NOT supported by me in any case.\n"
            "if the game uses version 7 or 8 and crashes when uses the rebuilt package try to rebuild the archive specifying the -x option.\n"
            "\n"
            "Usually you don't need to create 0.ttarch if you modify only the landb file, you can leave that file in the pack folder.\n"
            "\n");
        exit(1);
    }

    argc -= 3;
    for(i = 1; i < argc; i++) {
        if(((argv[i][0] != '-') && (argv[i][0] != '/')) || (strlen(argv[i]) != 2)) {
            printf("\nError: wrong argument (%s)\n", argv[i]);
            exit(1);
        }
        switch(argv[i][1]) {
            case 'l': list_only         = 1;                    break;
            case 'f': filter_files      = argv[++i];            break;
            case 'o': force_overwrite   = 1;                    break;
            case 'm': meta_extract      = 1;                    break;
            case 'b': rebuild           = 1;                    break;
            case 'k': mykey             = argv[++i];            break;
            case 'd': decrypt_only      = get_num(argv[++i]);   break;
            case 'D': {
                decrypt_only            = get_num(argv[++i]);
                decrypt_onlysz          = get_num(argv[++i]);
                break;
            }
            case 'e': encrypt_only      = get_num(argv[++i]);   break;
            case 'E': {
                encrypt_only            = get_num(argv[++i]);
                encrypt_onlysz          = get_num(argv[++i]);
                break;
            }
            case 'V': version           = get_num(argv[++i]);   break;
            case 'O': force_old_mode++;                         break;  // -O is old_mode 1, -O -O is old_mode 2
            case 'x': xmode             = 0;                    break;
            case 'T': dump_table        = argv[++i];            break;
            case 'v': verbose           = 1;                    break;
            default: {
                printf("\nError: wrong argument (%s)\n", argv[i]);
                exit(1);
            }
        }
    }
    for(i = 0; i < 3; i++) {    // check, it's easy to forget some parameters
        if((argv[argc + i][0] == '-') && (strlen(argv[argc + i]) == 2)) {
            fprintf(stderr, "\n"
                "Error: seems that you have missed one of the needed parameters.\n"
                "       launch this tool without arguments for a quick help\n");
            exit(1);
        }
    }

    gamenum = atoi(argv[argc]);
    fname   = argv[argc + 1];
    fdir    = argv[argc + 2];

    if(rebuild) {
        printf("- start building of %s\n", fname);
        if(check_overwrite(fname) < 0) exit(1);
        fd = fopen(fname, "wb");
    } else {
        printf("- open file %s\n", fname);
        fd = fopen(fname, "rb");
    }
    if(!fd) std_err();

    if(!list_only) {
        printf("- set output folder %s\n", fdir);
        if(chdir(fdir) < 0) std_err();
    }

    if(mykey) {
        printf("- set custom blowfish key\n");
        mykey = string2key(mykey);
    } else {
        for(i = 0; i < gamenum; i++) {
            if(!gamekeys[i].name) break;
        }
        if(!gamekeys[i].name) {
            printf("\nError: the number of key you choosed is too big\n");
            exit(1);
        }
        printf("- set the blowfish key of \"%s\"\n", gamekeys[i].name);
        mykey    = gamekeys[i].key;
        old_mode = gamekeys[i].old_mode;
    }
    if(force_old_mode) old_mode = force_old_mode;
    //blowfish(NULL, 0, mykey, strlen(mykey), 0);   // no longer used

    p = strrchr(fname, '\\');
    if(!p) p = strrchr(fname, '/');
    if(p) fname = p + 1;

    if(version == DEFAULT_VERSION) {
        if(gamekeys[gamenum].extension == EXT_TTARCH2) version = 7;
    }

    if(rebuild) {
        extracted_files = rebuild_it(fname, fd);
        printf("\n- %d files found\n", extracted_files);
    } else {
        if(decrypt_only >= 0) {
            i = crypt_it(fd, fname, decrypt_only, decrypt_onlysz, 0);
            printf("\n- decrypted %d bytes from offset 0x%08x\n", i, decrypt_only);
        } else if(encrypt_only >= 0) {
            i = crypt_it(fd, fname, encrypt_only, encrypt_onlysz, 1);
            printf("\n- encrypted %d bytes from offset 0x%08x\n", i, encrypt_only);
        } else {
            ttarch_extract(fd, fname);
            printf("\n- %d files found\n", extracted_files);
        }
    }

    fclose(fd);
    printf("- done\n");
    return(0);
}



u64 get_file_size(FILE *fd) {
    u64     curr,
            size;

    curr = ftell(fd);
    fseek(fd, 0, SEEK_END);
    size = ftell(fd);
    fseek(fd, curr, SEEK_SET);
    return size;
}



// hash tables used for searching names in a faster way
u64 ttarch2_hash(u64 crc, u8 *str) {
    static const u64    ttarch2_hash_crctable[256] = {
        0x0000000000000000, 0x42f0e1eba9ea3693, 0x85e1c3d753d46d26, 0xc711223cfa3e5bb5, 
        0x493366450e42ecdf, 0x0bc387aea7a8da4c, 0xccd2a5925d9681f9, 0x8e224479f47cb76a, 
        0x9266cc8a1c85d9be, 0xd0962d61b56fef2d, 0x17870f5d4f51b498, 0x5577eeb6e6bb820b, 
        0xdb55aacf12c73561, 0x99a54b24bb2d03f2, 0x5eb4691841135847, 0x1c4488f3e8f96ed4, 
        0x663d78ff90e185ef, 0x24cd9914390bb37c, 0xe3dcbb28c335e8c9, 0xa12c5ac36adfde5a, 
        0x2f0e1eba9ea36930, 0x6dfeff5137495fa3, 0xaaefdd6dcd770416, 0xe81f3c86649d3285, 
        0xf45bb4758c645c51, 0xb6ab559e258e6ac2, 0x71ba77a2dfb03177, 0x334a9649765a07e4, 
        0xbd68d2308226b08e, 0xff9833db2bcc861d, 0x388911e7d1f2dda8, 0x7a79f00c7818eb3b, 
        0xcc7af1ff21c30bde, 0x8e8a101488293d4d, 0x499b3228721766f8, 0x0b6bd3c3dbfd506b, 
        0x854997ba2f81e701, 0xc7b97651866bd192, 0x00a8546d7c558a27, 0x4258b586d5bfbcb4, 
        0x5e1c3d753d46d260, 0x1cecdc9e94ace4f3, 0xdbfdfea26e92bf46, 0x990d1f49c77889d5, 
        0x172f5b3033043ebf, 0x55dfbadb9aee082c, 0x92ce98e760d05399, 0xd03e790cc93a650a, 
        0xaa478900b1228e31, 0xe8b768eb18c8b8a2, 0x2fa64ad7e2f6e317, 0x6d56ab3c4b1cd584, 
        0xe374ef45bf6062ee, 0xa1840eae168a547d, 0x66952c92ecb40fc8, 0x2465cd79455e395b, 
        0x3821458aada7578f, 0x7ad1a461044d611c, 0xbdc0865dfe733aa9, 0xff3067b657990c3a, 
        0x711223cfa3e5bb50, 0x33e2c2240a0f8dc3, 0xf4f3e018f031d676, 0xb60301f359dbe0e5, 
        0xda050215ea6c212f, 0x98f5e3fe438617bc, 0x5fe4c1c2b9b84c09, 0x1d14202910527a9a, 
        0x93366450e42ecdf0, 0xd1c685bb4dc4fb63, 0x16d7a787b7faa0d6, 0x5427466c1e109645, 
        0x4863ce9ff6e9f891, 0x0a932f745f03ce02, 0xcd820d48a53d95b7, 0x8f72eca30cd7a324, 
        0x0150a8daf8ab144e, 0x43a04931514122dd, 0x84b16b0dab7f7968, 0xc6418ae602954ffb, 
        0xbc387aea7a8da4c0, 0xfec89b01d3679253, 0x39d9b93d2959c9e6, 0x7b2958d680b3ff75, 
        0xf50b1caf74cf481f, 0xb7fbfd44dd257e8c, 0x70eadf78271b2539, 0x321a3e938ef113aa, 
        0x2e5eb66066087d7e, 0x6cae578bcfe24bed, 0xabbf75b735dc1058, 0xe94f945c9c3626cb, 
        0x676dd025684a91a1, 0x259d31cec1a0a732, 0xe28c13f23b9efc87, 0xa07cf2199274ca14, 
        0x167ff3eacbaf2af1, 0x548f120162451c62, 0x939e303d987b47d7, 0xd16ed1d631917144, 
        0x5f4c95afc5edc62e, 0x1dbc74446c07f0bd, 0xdaad56789639ab08, 0x985db7933fd39d9b, 
        0x84193f60d72af34f, 0xc6e9de8b7ec0c5dc, 0x01f8fcb784fe9e69, 0x43081d5c2d14a8fa, 
        0xcd2a5925d9681f90, 0x8fdab8ce70822903, 0x48cb9af28abc72b6, 0x0a3b7b1923564425, 
        0x70428b155b4eaf1e, 0x32b26afef2a4998d, 0xf5a348c2089ac238, 0xb753a929a170f4ab, 
        0x3971ed50550c43c1, 0x7b810cbbfce67552, 0xbc902e8706d82ee7, 0xfe60cf6caf321874, 
        0xe224479f47cb76a0, 0xa0d4a674ee214033, 0x67c58448141f1b86, 0x253565a3bdf52d15, 
        0xab1721da49899a7f, 0xe9e7c031e063acec, 0x2ef6e20d1a5df759, 0x6c0603e6b3b7c1ca, 
        0xf6fae5c07d3274cd, 0xb40a042bd4d8425e, 0x731b26172ee619eb, 0x31ebc7fc870c2f78, 
        0xbfc9838573709812, 0xfd39626eda9aae81, 0x3a28405220a4f534, 0x78d8a1b9894ec3a7, 
        0x649c294a61b7ad73, 0x266cc8a1c85d9be0, 0xe17dea9d3263c055, 0xa38d0b769b89f6c6, 
        0x2daf4f0f6ff541ac, 0x6f5faee4c61f773f, 0xa84e8cd83c212c8a, 0xeabe6d3395cb1a19, 
        0x90c79d3fedd3f122, 0xd2377cd44439c7b1, 0x15265ee8be079c04, 0x57d6bf0317edaa97, 
        0xd9f4fb7ae3911dfd, 0x9b041a914a7b2b6e, 0x5c1538adb04570db, 0x1ee5d94619af4648, 
        0x02a151b5f156289c, 0x4051b05e58bc1e0f, 0x87409262a28245ba, 0xc5b073890b687329, 
        0x4b9237f0ff14c443, 0x0962d61b56fef2d0, 0xce73f427acc0a965, 0x8c8315cc052a9ff6, 
        0x3a80143f5cf17f13, 0x7870f5d4f51b4980, 0xbf61d7e80f251235, 0xfd913603a6cf24a6, 
        0x73b3727a52b393cc, 0x31439391fb59a55f, 0xf652b1ad0167feea, 0xb4a25046a88dc879, 
        0xa8e6d8b54074a6ad, 0xea16395ee99e903e, 0x2d071b6213a0cb8b, 0x6ff7fa89ba4afd18, 
        0xe1d5bef04e364a72, 0xa3255f1be7dc7ce1, 0x64347d271de22754, 0x26c49cccb40811c7, 
        0x5cbd6cc0cc10fafc, 0x1e4d8d2b65facc6f, 0xd95caf179fc497da, 0x9bac4efc362ea149, 
        0x158e0a85c2521623, 0x577eeb6e6bb820b0, 0x906fc95291867b05, 0xd29f28b9386c4d96, 
        0xcedba04ad0952342, 0x8c2b41a1797f15d1, 0x4b3a639d83414e64, 0x09ca82762aab78f7, 
        0x87e8c60fded7cf9d, 0xc51827e4773df90e, 0x020905d88d03a2bb, 0x40f9e43324e99428, 
        0x2cffe7d5975e55e2, 0x6e0f063e3eb46371, 0xa91e2402c48a38c4, 0xebeec5e96d600e57, 
        0x65cc8190991cb93d, 0x273c607b30f68fae, 0xe02d4247cac8d41b, 0xa2dda3ac6322e288, 
        0xbe992b5f8bdb8c5c, 0xfc69cab42231bacf, 0x3b78e888d80fe17a, 0x7988096371e5d7e9, 
        0xf7aa4d1a85996083, 0xb55aacf12c735610, 0x724b8ecdd64d0da5, 0x30bb6f267fa73b36, 
        0x4ac29f2a07bfd00d, 0x08327ec1ae55e69e, 0xcf235cfd546bbd2b, 0x8dd3bd16fd818bb8, 
        0x03f1f96f09fd3cd2, 0x41011884a0170a41, 0x86103ab85a2951f4, 0xc4e0db53f3c36767, 
        0xd8a453a01b3a09b3, 0x9a54b24bb2d03f20, 0x5d45907748ee6495, 0x1fb5719ce1045206, 
        0x919735e51578e56c, 0xd367d40ebc92d3ff, 0x1476f63246ac884a, 0x568617d9ef46bed9, 
        0xe085162ab69d5e3c, 0xa275f7c11f7768af, 0x6564d5fde549331a, 0x279434164ca30589, 
        0xa9b6706fb8dfb2e3, 0xeb46918411358470, 0x2c57b3b8eb0bdfc5, 0x6ea7525342e1e956, 
        0x72e3daa0aa188782, 0x30133b4b03f2b111, 0xf7021977f9cceaa4, 0xb5f2f89c5026dc37, 
        0x3bd0bce5a45a6b5d, 0x79205d0e0db05dce, 0xbe317f32f78e067b, 0xfcc19ed95e6430e8, 
        0x86b86ed5267cdbd3, 0xc4488f3e8f96ed40, 0x0359ad0275a8b6f5, 0x41a94ce9dc428066, 
        0xcf8b0890283e370c, 0x8d7be97b81d4019f, 0x4a6acb477bea5a2a, 0x089a2aacd2006cb9, 
        0x14dea25f3af9026d, 0x562e43b4931334fe, 0x913f6188692d6f4b, 0xd3cf8063c0c759d8, 
        0x5dedc41a34bbeeb2, 0x1f1d25f19d51d821, 0xd80c07cd676f8394, 0x9afce626ce85b507
    };
    u8      *p;

    if(!str) {
        printf("\nError: ttarch2_hash NULL pointer\n");
        exit(1);
    }

    for(p = str; *p; p++) {
        crc = ttarch2_hash_crctable[(tolower(*p) ^ (crc >> (u64)(64 - 8))) & 0xff] ^ (crc << (u64)8);
    }
    return crc;
}



int ttarch_import_lua(u8 *ext, u8 *buff, u64 *size, int encrypt) {
    if(ext && (!stricmp(ext, ".lua") || !stricmp(ext, ".lenc"))) {
        if(IS_LUA(buff) || (encrypt && (gamenum >= 58))) {
            if(gamenum >= 58) {
                mymemmove(buff + 4, buff, *size);
                *size += 4;
                memcpy(buff, "\x1bLEo", 4);
                blowfish(buff + 4, *size - 4, encrypt);
            } else if(gamenum >= 56) {
                if(*size < 4) return -1;
                memcpy(buff, "\x1bLEn", 4);
                blowfish(buff + 4, *size - 4, encrypt);
            } else {
                blowfish(buff, *size, encrypt);
            }
            return 0;
        }
    }
    return -1;
}



u64 ttarch_import(FILE *fdo, u8 *fname) {
    static u64  buffsz  = 0;
    static u8   *buff   = NULL;
    struct stat xstat;
    FILE    *fd;
    u64     size;
    u8      *ext;

    ext = strrchr(fname, '.');

    if(fdo) printf("- import %s\n", fname);
    fd = fopen(fname, "rb");
    if(!fd) std_err();
    fstat(fileno(fd), &xstat);
    size = xstat.st_size;
    myalloc(&buff, size, &buffsz);
    myfr(fd, buff, size);
    fclose(fd);

    ttarch_import_lua(ext, buff, &size, 1);

    ttarch_meta_crypt(buff, size, 1);

    if(fdo) myfw(fdo, buff, size);

    return size;
}



u64 pad_it(u64 num, u64 pad) {
    u64     t;

    t = num % pad;
    if(t) num += pad - t;
    return(num);
}



u8 *import_filename(u8 *fname) {
    static u64  buffsz  = 0;
    static u8   *buff   = NULL;

    u8      *ext;

    ext = strrchr(fname, '.');
    if(ext && !stricmp(ext, ".lua")) {
        if(gamenum < 56) {  // the games before Tales from the Borderlands need the lenc extension
            myalloc(&buff, (ext - fname) + 16, &buffsz);
            sprintf(buff, "%.*s.lenc", (int)(ext - fname), fname);
            return buff;
        }
    }
    return fname;
}



void build_sort_ttarch2_crc_names(files_t *files, u32 tot) {
    files_t tmp;
    u32     i,
            j;

    for(i = 0; i < tot; i++) {
        files[i].name_crc = ttarch2_hash(0, import_filename(files[i].name));
    }

    for(i = 0; i < (tot - 1); i++) {
        for(j = i + 1; j < tot; j++) {
            if(files[j].name_crc < files[i].name_crc) {
                memcpy(&tmp,      &files[i], sizeof(files_t));
                memcpy(&files[i], &files[j], sizeof(files_t));
                memcpy(&files[j], &tmp,      sizeof(files_t));
            }
        }
    }
}



u32 rebuild_it(u8 *output_name, FILE *fdo) {
    u64     off         = 0,
            data_size   = 0,
            size_check;
    u32     i,
            tmp,
            tot         = 0,
            totdirs     = 0,
            info_size   = 0,
            names_size  = 0;
    files_t *files      = NULL;
    u8      filedir[4096],
            *folders[]  = { NULL },
            *info_table = NULL,
            *names_table = NULL,
            *p,
            *it,
            *nt,
            *ext;

    ext = strrchr(output_name, '.');
    if(ext && !stricmp(ext, ".ttarch2")) version = 7;

    if(version == DEFAULT_VERSION) {
        printf("\n"
            "Error: you must use the -V option with the rebuild one\n"
            "       for example -V 7 for Monkey Island, you can see the version of the\n"
            "       original archive when you extract or list it\n");
        exit(1);
    }

    printf("- start recursive scanning\n");
    strcpy(filedir, ".");
    recursive_dir(filedir);
    files = add_files(NULL, 0, &tot);
    printf("- collected %d files\n", tot);

    // fix the size, for example the Lua files add the magic at the beginning (size + 4).
    // yeah, it takes some resources but makes the code a lot simpler and easy to update in future.
    printf("- collecting the new files sizes\n");
    for(i = 0; i < tot; i++) {
        files[i].size = ttarch_import(NULL, files[i].name);
    }
    printf("- files sizes fixed\n");

    if(ext && !stricmp(ext, ".ttarch2")) {

        build_sort_ttarch2_crc_names(files, tot);

        info_size = tot * (8 + 8 + 4 + 4 + 2 + 2);
        for(i = 0; i < tot; i++) {
            names_size += strlen(import_filename(files[i].name)) + 1;
            data_size += files[i].size;
        }
        names_size = pad_it(names_size, 0x10000);

        info_table = calloc(info_size, 1);
        if(!info_table) std_err();
        names_table = calloc(names_size, 1);
        if(!names_table) std_err();

        fputxx(fdo, 0x5454434e, 4);  // NCTT
        fputxx(fdo, 4 + 4 + 4 + 4 + info_size + names_size + data_size, 8);

        if(gamenum >= 58) {
            fputxx(fdo, 0x54544134, 4);  // 4ATT
        } else {
            fputxx(fdo, 0x54544133, 4);  // 3ATT
            fputxx(fdo, 2,          4);
        }
        fputxx(fdo, names_size, 4);
        fputxx(fdo, tot,        4);

        it = info_table;
        nt = names_table;
        for(i = 0; i < tot; i++) {
            it += putxx(it, files[i].name_crc, 8);
            it += putxx(it, off,           8);
            it += putxx(it, files[i].size, 4);
            it += putxx(it, 0,             4);  // <= files[i].size
            tmp = nt - names_table;
            it += putxx(it, tmp / 0x10000, 2);
            it += putxx(it, tmp % 0x10000, 2);

            nt += sprintf(nt, "%s", import_filename(files[i].name)) + 1;
            off += files[i].size;
        }

        myfw(fdo, info_table, info_size);
        myfw(fdo, names_table, names_size);

    } else {

        info_size += 4;     // folders
        for(i = 0; folders[i]; i++) {
            info_size += 4 + strlen(folders[i]);
            totdirs++;
        }
        info_size += 4;     // files
        for(i = 0; i < tot; i++) {
            info_size += 4 + strlen(import_filename(files[i].name)) + 4 + 4 + 4;
            data_size += files[i].size;
        }
        if(version <= 2) {
            info_size += 4 + 4 + 4 + 4;
        }
        info_size = pad_it(info_size, 8);   // 8 for blow_fish
        info_table = calloc(info_size, 1);
        if(!info_table) std_err();

        printf("- start building of the file\n");
        fputxx(fdo, version,    4); // version
        fputxx(fdo, 1,          4); // info_mode
        fputxx(fdo, 2,          4); // type3
        if(version >= 3) {
            fputxx(fdo, 1,      4); // files_mode
        }
        if(version >= 3) {
            fputxx(fdo, 0,      4); // ttarch_tot_idx
            fputxx(fdo, data_size, 4);
            if(version >= 4) {
                fputxx(fdo, 0,  4);
                fputxx(fdo, 0,  4);
                if(version >= 7) {
                    fputxx(fdo, xmode,  4);
                    fputxx(fdo, xmode,  4);
                    fputxx(fdo, 0x40,   4);
                    if(version >= 8) {
                        fputxx(fdo, 0,  1);
                    }
                }
            }
        }
        fputxx(fdo, info_size,  4);

        p = info_table;
        p += putxx(p, totdirs,  4); // folders
        for(i = 0; i < totdirs; i++) {
            p += putxx(p, strlen(folders[i]), 4);
            p += sprintf(p, "%s", folders[i]);
        }
        p += putxx(p, tot,      4); // files
        for(i = 0; i < tot; i++) {
            p += putxx(p, strlen(import_filename(files[i].name)), 4);
            p += sprintf(p, "%s", import_filename(files[i].name));
            p += putxx(p, 0,    4);
            p += putxx(p, off,  4);
            p += putxx(p, files[i].size, 4);
            off += files[i].size;
        }
        if(version <= 2) {
            p += putxx(p, info_size + 4, 4);
            p += putxx(p, data_size, 4);
            p += putxx(p, 0xfeedface, 4);
            p += putxx(p, 0xfeedface, 4);
        }
        if(pad_it(p - info_table, 8) != info_size) {
            printf("\nError: problem in the info_size calculated by ttarchext (%d, %d)\n", (int)(p - info_table), (int)info_size);
            exit(1);
        }

        blowfish(info_table, info_size, 1);
        myfw(fdo, info_table, info_size);
    }

    size_check = 0;
    for(i = 0; i < tot; i++) {
        size_check += ttarch_import(fdo, files[i].name);
    }
    if(size_check != data_size) {
        printf("\nError: problem in the data_size calculated by ttarchext (%d, %d)\n", (u32)size_check, (u32)data_size);
        exit(1);
    }

    return(tot);
}



files_t *add_files(u8 *fname, u64 fsize, int *ret_files) {
    static int      filesi  = 0,
                    filesn  = 0;
    static files_t  *files  = NULL;

    if(ret_files) {
        *ret_files = filesi;
        return(files);
    }

    if(filesi >= filesn) {
        filesn += 1024;
        files = realloc(files, sizeof(files_t) * filesn);
        if(!files) std_err();
    }
    files[filesi].name   = strdup(fname);
    files[filesi].offset = 0;
    files[filesi].size   = fsize;
    filesi++;
    return(NULL);
}



int recursive_dir(u8 *filedir) {
    int     plen,
            ret     = -1;

#ifdef WIN32
    static int      winnt = -1;
    OSVERSIONINFO   osver;
    WIN32_FIND_DATA wfd;
    HANDLE  hFind;

    if(winnt < 0) {
        osver.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
        GetVersionEx(&osver);
        if(osver.dwPlatformId >= VER_PLATFORM_WIN32_NT) {
            winnt = 1;
        } else {
            winnt = 0;
        }
    }

    plen = strlen(filedir);
    strcpy(filedir + plen, "\\*.*");
    plen++;

    if(winnt) { // required to avoid problems with Vista and Windows7!
        hFind = FindFirstFileEx(filedir, FindExInfoStandard, &wfd, FindExSearchNameMatch, NULL, 0);
    } else {
        hFind = FindFirstFile(filedir, &wfd);
    }
    if(hFind == INVALID_HANDLE_VALUE) return(0);
    do {
        if(!strcmp(wfd.cFileName, ".") || !strcmp(wfd.cFileName, "..")) continue;

        strcpy(filedir + plen, wfd.cFileName);

        if(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if(recursive_dir(filedir) < 0) goto quit;
        } else {
            add_files(filedir + 2, wfd.nFileSizeLow, NULL);
        }
    } while(FindNextFile(hFind, &wfd));
    ret = 0;

quit:
    FindClose(hFind);
#else
    struct  stat    xstat;
    struct  dirent  **namelist;
    int     n,
            i;

    n = scandir(filedir, &namelist, NULL, NULL);
    if(n < 0) {
        if(stat(filedir, &xstat) < 0) {
            printf("**** %s", filedir);
            std_err();
        }
        add_files(filedir + 2, xstat.st_size, NULL);
        return(0);
    }

    plen = strlen(filedir);
    strcpy(filedir + plen, "/");
    plen++;

    for(i = 0; i < n; i++) {
        if(!strcmp(namelist[i]->d_name, ".") || !strcmp(namelist[i]->d_name, "..")) continue;
        strcpy(filedir + plen, namelist[i]->d_name);

        if(stat(filedir, &xstat) < 0) {
            printf("**** %s", filedir);
            std_err();
        }
        if(S_ISDIR(xstat.st_mode)) {
            if(recursive_dir(filedir) < 0) goto quit;
        } else {
            add_files(filedir + 2, xstat.st_size, NULL);
        }
        FREE(namelist[i])
    }
    ret = 0;

quit:
    for(; i < n; i++) FREE(namelist[i])
    FREE(namelist)
#endif
    filedir[plen - 1] = 0;
    return(ret);
}



u64 crypt_it(FILE *fd, u8 *fname, u64 offset, int wanted_size /*signed!*/, int encrypt) {
    static u64  buffsz  = 0;
    static u8   *buff   = NULL;

    u64     size;
    u8      *ext;

    fname = create_dir(fname);
    if(check_overwrite(fname) < 0) exit(1);
    ext = strrchr(fname, '.');

    size = get_file_size(fd);
    if(offset > size) exit(1);

    if(wanted_size < 0) {
        if(fseek(fd, offset, SEEK_SET)) std_err();
        size -= offset;
    } else {
        fseek(fd, 0, SEEK_SET);
    }

    myalloc(&buff, size, &buffsz);
    myfr(fd, buff, size);

    if(wanted_size < 0) {
        if(ttarch_import_lua(ext, buff, &size, encrypt) < 0) {
            blowfish(buff, size, encrypt);
        }
    } else {
        u64     t64 = wanted_size;
        if(ttarch_import_lua(ext, buff + offset, &t64, encrypt) < 0) {
            blowfish(buff + offset, t64, encrypt);
        }
    }

    dumpa(fname, buff, size, NULL, 0);
    return(size);
}



u8 *string2key(u8 *data) {
    int     i,
            n;
    u8      *ret;

    ret = strdup(data);
    for(i = 0; *data; i++) {
        while(*data && ((*data <= ' ') || (*data == '\\') || (*data == 'x'))) data++;
        if(sscanf(data, "%02x", &n) != 1) break;
        ret[i] = n;
        data += 2;
    }
    ret[i] = 0; // key must be NULL delimited
    return(ret);
}



u64 ttarch_getxx(FILE *fd, u8 **data, int bytes) {
    u64     ret;

    if(fd) {
        ret = fgetxx(fd, bytes);
    } else {
        ret = getxx(*data, bytes);
        *data += bytes;
    }
    return(ret);
}



u8 *ttarch_fgetss(FILE *fd) {
    static u64  buffsz  = 0;
    static u8   *buff   = NULL;
    int     namesz;
    u8      c;

    for(namesz = 0;; namesz++) {
        c = ttarch_fgetxx(1, fd);
        myalloc(&buff, namesz + 1, &buffsz);
        buff[namesz] = c;
        if(!c) break;
    }
    return buff;
}



u8 *ttarch_getname(FILE *fd, u8 **data) {
    static u64  buffsz  = 0;
    static u8   *buff   = NULL;
    int     namesz;

    namesz = ttarch_getxx(fd, data, 4);
    myalloc(&buff, namesz + 1, &buffsz);
    if(fd) {
        myfr(fd, buff, namesz);
    } else {
        memcpy(buff, *data, namesz);
        *data += namesz;
    }
    buff[namesz] = 0;
    return(buff);
}



int ttarch_extract(FILE *fd, u8 *input_fname) {

    #define DUMPA \
        if(verbose) { \
            printf("  offset        %08x\n", (int)offset); \
            printf("  size          %08x\n", (int)size); \
            printf("  name          %s\n", name); \
        } \
        if(filter_files && (check_wildcard(name, filter_files) < 0)) continue; \
        printf("  %08x %-10u %s\n", (u32)(ttarch_tot_idx ? offset : (ttarch_baseoff + offset)), (u32)size, name); \
        if(list_only) { \
            extracted_files++; \
            continue; \
        } \
        \
        myalloc(&buff, size, &buffsz); \
        \
        ttarch_fseek(fd, offset, SEEK_SET); \
        ttarch_fread(buff, size, fd); \
        \
        ttarch_dumpa(name, buff, size, 0);

    static u64  buffsz  = 0;
    static u8   *buff   = NULL;

    u64     offset,
            names_offset    = 0,
            base_offset     = 0,
            size;
    u32     info_mode       = 0,
            files_mode      = 0,
            type3           = 0,
            info_zsize      = 0,
            info_size       = 0,
            names_size      = 0,
            dummy,
            dummy2,
            folders,
            files,
            zero,
            name_blk,
            name_off,
            i,
            ver;
    u8      *info_table     = NULL,
            *names_table    = NULL,
            *t              = NULL,
            *name,
            *ext;

    ext = strrchr(input_fname, '.');
    if(ext && strnicmp(ext, ".ttarch", 7)) {    // simple decryption of a single file... just for fun
        size = get_file_size(fd);
        fseek(fd, 0, SEEK_SET);

        myalloc(&buff, size, &buffsz);
        myfr(fd, buff, size);

        name = strdup(input_fname);
        printf("- decrypt the single input file as %s\n", name);
        if(list_only) {
            // do nothing, it's only a listing
        } else {
            ttarch_dumpa(name, buff, size, 0);
        }
        goto quit;
    }

    if(old_mode) {  // boring old mode
        if(old_mode == 2) {
            info_mode = 1;
            info_size = fgetxx(fd, 4);
            goto goto_extract;
        }
        folders = ttarch_getxx(fd, NULL, 4);
        for(i = 0; i < folders; i++) {
            name = ttarch_getname(fd, NULL);
        }
        files = ttarch_getxx(fd, NULL, 4);
        for(i = 0; i < files; i++) {
            name   = ttarch_getname(fd, NULL);
            zero   = ttarch_getxx(fd, NULL, 4);
            offset = ttarch_getxx(fd, NULL, 4);
            size   = ttarch_getxx(fd, NULL, 4);
        }
        info_size = fgetxx(fd, 4);
        fseek(fd, 0, SEEK_SET);
        goto goto_extract;
    }

    version = fgetxx(fd, 4);

    if(islower(version & 0xff)) {   // eCTT and zCTT
        switch(fgetxx(fd, 4)) {      // 1
            case 0: /* zlib? */                         break;
            case 1: do_decompress = _oodle_decompress;  break;
            default: /* ??? */                          break;
        }
        version = (version & 0xffffff00) | toupper((version & 0xff));
    }

    switch(version) {
        case 0x54544345:        // ECTT
            ttarch_chunks_b = 1;
            // NO BREAK! it must continue as ZCTT

        case 0x5454435a:        // ZCTT
            ttarch_chunksz = fgetxx(fd, 4);
            ttarch_tot_idx = fgetxx(fd, 4);
            printf("- found %d compressed chunks\n", ttarch_tot_idx);
            ttarch_chunks = calloc(ttarch_tot_idx, sizeof(*ttarch_chunks));
            if(!ttarch_chunks) std_err();

                dummy = fgetxx(fd, 8);
            for(i = 0; i < ttarch_tot_idx; i++) {
                dummy2 = fgetxx(fd, 8);
                ttarch_chunks[i] = dummy2 - dummy;
                dummy = dummy2;
            }

            //data_size = ttarch_chunksz * ttarch_tot_idx;
            ttarch_baseoff = ftell(fd);
            break;

        case 0x5454434e:        // NCTT
            fgetxx(fd, 8); // data_size
            ttarch_baseoff = ftell(fd);
            break;

        case 0x54544133:        // 3ATT
        case 0x54544134:        // 4ATT
            //data_size = get_file_size(fd);
            ttarch_baseoff = ftell(fd) - 4;
            break;

        default:
            goto goto_classic_mode;
            break;
    }


//goto_tt_mode:   // The Wolf Among Us: NCTT / ZCTT / 3ATT

    ttarch_fseek(fd, 0, SEEK_SET);
    ver = ttarch_fgetxx(4, fd);
    if((ver & 0xffffff00) != 0x54544100) { // 3ATT
        // exists a 2ATT but I don't know it
        printf("\nError: version %08x is not supported yet\n", ver);
        exit(1);
    }

    if(ver == 0x54544133) { // 3ATT
                   ttarch_fgetxx(4, fd);    // attention in future in case of encryption
    }
    names_size   = ttarch_fgetxx(4, fd);
    files        = ttarch_fgetxx(4, fd);

    info_size    = files * (8 + 8 + 4 + 4 + 2 + 2);
    names_offset = ttarch_ftell(fd) + info_size;
    base_offset  = names_offset + names_size;
    if(verbose) {
        printf("  info_size     %08x\n", info_size);
        printf("  names_offset  %08x\n", (int)names_offset);
        printf("  base_offset   %08x\n", (int)base_offset);
    }

    info_table = calloc(info_size, 1);
    if(!info_table) std_err();
    names_table = calloc(names_size, 1);
    if(!names_table) std_err();

    ttarch_fread(info_table,  info_size,  fd);
    ttarch_fread(names_table, names_size, fd);
    t = info_table;
    for(i = 0; i < files; i++) {
                   ttarch_getxx(NULL, &t, 8);   // hash table
        offset   = ttarch_getxx(NULL, &t, 8);
        size     = ttarch_getxx(NULL, &t, 4);
        dummy    = ttarch_getxx(NULL, &t, 4);   // ???
        name_blk = ttarch_getxx(NULL, &t, 2);
        name_off = ttarch_getxx(NULL, &t, 2);

        name = names_table + name_off + (name_blk * 0x10000);
        offset += base_offset;

        DUMPA
    }

    goto quit;


goto_classic_mode:
    if((version < 1) || (version > 9)) {
        printf("\nError: version %d is not supported yet\n", version);
        exit(1);
    }
    printf("- version    %d\n", version);

    info_mode = fgetxx(fd, 4);
    if(/*(info_mode < 0) ||*/ (info_mode > 1)) {
        printf("\nError: info_mode %d is not supported yet\n", info_mode);
        exit(1);
    }
    printf("- info_mode  %d\n", info_mode);

    type3     = fgetxx(fd, 4);
    printf("- type3      %d\n", type3);

    if(version >= 3) {
        files_mode = fgetxx(fd, 4);
    }
    if(/*(files_mode < 0)  || */ (files_mode > 2)) {
        printf("\nError: files_mode %d is not supported yet\n", files_mode);
        exit(1);
    }
    printf("- files_mode %d\n", files_mode);

    if(version >= 3) {
        ttarch_tot_idx = fgetxx(fd, 4);
        if(ttarch_tot_idx) {
            printf("- found %d compressed chunks\n", ttarch_tot_idx);
            ttarch_chunks = calloc(ttarch_tot_idx, sizeof(*ttarch_chunks));
            if(!ttarch_chunks) std_err();
            for(i = 0; i < ttarch_tot_idx; i++) {
                ttarch_chunks[i] = fgetxx(fd, 4);
            }
        }

                    fgetxx(fd, 4);  // the size of the field where are stored all the files contents
        if(version >= 4) {
            dummy = fgetxx(fd, 4);
            dummy = fgetxx(fd, 4);
            if(version >= 7) {
                dummy = fgetxx(fd, 4);
                dummy = fgetxx(fd, 4);
                ttarch_chunksz = fgetxx(fd, 4);
                ttarch_chunksz *= 1024;
                printf("- set chunk size to 0x%x bytes\n", ttarch_chunksz);
                if(version >= 8) {
                    fgetxx(fd, 1);  // 0 or 1
                    if(version >= 9) {  // sort of CRC, apparently for version 9 only
                        if(files_mode >= 2) fgetxx(fd, 4);
                    }
                }
            }
        }
    }

    info_size  = fgetxx(fd, 4);
    if(!info_size) info_size  = fgetxx(fd, 4);  // this is a lame work-around for some old version 8 archives that can be ignored and removed
    if((version >= 7) && files_mode >= 2) {
        info_zsize = fgetxx(fd, 4);
    }
goto_extract:
    printf("- info_table has a size of %d bytes\n", info_size);
    info_table = calloc(info_size, 1);
    if(!info_table) std_err();

    if((version >= 7) && (files_mode >= 2)) {
        t = calloc(info_zsize, 1);
        myfr(fd, t, info_zsize);
        do_decompress(t, info_zsize, info_table, info_size);
        FREE(t)
    } else {
        myfr(fd, info_table, info_size);
    }

    if(info_mode >= 1) {
        printf("- decrypt info_table\n");
        blowfish(info_table, info_size, 0);
    }

    ttarch_baseoff = ftell(fd);
    printf("- set files base offset 0x%08x\n", (u32)ttarch_baseoff);

    if(files_mode >= 2) {   // not verified
        if(info_mode > 0) ttarch_chunks_b = 1;
    }
    printf("- filesystem compression: %s\n", ttarch_tot_idx  ? "on" : "off");
    printf("- filesystem encryption:  %s\n", ttarch_chunks_b ? "on" : "off");

    t = info_table;
    if(dump_table) {
        dumpa(dump_table, info_table, info_size, NULL, 0);
    }

    printf("- retrieve folders:\n");
    folders = ttarch_getxx(NULL, &t, 4);
    for(i = 0; i < folders; i++) {
        name = ttarch_getname(NULL, &t);
        printf("  %s\n", name);
    }

    printf("\n"
        "  offset   filesize   filename\n"
        "------------------------------\n");

    files = ttarch_getxx(NULL, &t, 4);
    for(i = 0; i < files; i++) {
        name   = ttarch_getname(NULL, &t);
        zero   = ttarch_getxx(NULL, &t, 4);
        offset = ttarch_getxx(NULL, &t, 4);
        size   = ttarch_getxx(NULL, &t, 4);

        if(zero) {  // this value is just ignored in the current versions of ttarch, maybe it's for a future usage?
            printf("\nError: this file has an unknown ZERO value, contact me\n");
            exit(1);
        }

        DUMPA
    }

quit:
    FREE(info_table)
    FREE(names_table)
    return 0;
}



int ttarch_meta_crypt(u8 *data, u64 datalen, int encrypt) {
#define SET_BLOCKS(X,Y,Z) { \
            blocks_size     = X; \
            blocks_crypt    = Y; \
            blocks_clean    = Z; \
        }

    u32     file_type       = 0;
    u64     i,
            blocks_size     = 0,
            blocks_crypt    = 0,
            blocks_clean    = 0,
            // rem_blocks
            blocks;
    int     meta            = 1;
    u8      *p,
            *l;

    if(datalen < 4) return(file_type);

    p = data;
    l = data + datalen;
    file_type = ttarch_getxx(NULL, &p, 4);

    switch(file_type) {
        case 0x4D424553: SET_BLOCKS(0x40,  0x40, 0x64)  break;  // SEBM
        case 0x4D42494E:                                break;  // NIBM
        case 0xFB4A1764: SET_BLOCKS(0x80,  0x20, 0x50)  break;
        case 0xEB794091: SET_BLOCKS(0x80,  0x20, 0x50)  break;
        case 0x64AFDEFB: SET_BLOCKS(0x80,  0x20, 0x50)  break;
        case 0x64AFDEAA: SET_BLOCKS(0x100, 0x8,  0x18)  break;
        case 0x4D545245:                                break;  // ERTM
        default:         meta = 0;                      break;  // is not a meta stream file
    }

    if(blocks_size) {   // meta, just the same result
        blocks     = (datalen - 4) / blocks_size;
        //rem_blocks = (datalen - 4) % blocks_size;

        for(i = 0; i < blocks; i++) {
            if(p >= l) break;
            if(!(i % blocks_crypt)) {
                blowfish(p, blocks_size, encrypt);
            } else if(!(i % blocks_clean) && (i > 0)) {
                // skip this block
            } else {
                xor(p, blocks_size, 0xff);
            }
            p += blocks_size;
        }
    }

    return(meta);
}



u64 ttarch_ftell(FILE *stream) {
    return ttarch_offset;
}



int ttarch_fseek(FILE *stream, u64 offset, int origin) {
    u64     off = 0;
    u32     i,
            idx;

    if(!ttarch_tot_idx) {
        off = offset;
    } else {
        idx = offset / ttarch_chunksz;
        if(idx > ttarch_tot_idx) return(-1);
        for(i = 0; i < idx; i++) {
            off += ttarch_chunks[i];
        }
        ttarch_rem = offset % ttarch_chunksz;
    }
    ttarch_offset = offset;
    return(fseek(stream, ttarch_baseoff + off, origin));
}



u64 ttarch_fgetxx(int bytes, FILE *stream) {
    u8      tmp[bytes];

    ttarch_fread(tmp, bytes, stream);
    return getxx(tmp, bytes);
}



u64 ttarch_fread(void *ptr, u64 size, FILE *stream) {
    static  u8  *in     = NULL,
                *out    = NULL;
    u64     idx,
            len,
            currsz;

    if(!ttarch_tot_idx) {
        myfr(stream, ptr, size);
        if(ttarch_chunks_b) blowfish(ptr, size, 0);
        ttarch_offset += size;
        return(size);
    }

    if(!in || !out) {
        in  = calloc(ttarch_chunksz, 1);
        out = calloc(ttarch_chunksz, 1);
        if(!in || !out) std_err();
    }

    ttarch_fseek(stream, ttarch_offset, SEEK_SET);

    currsz = 0;
    for(idx = ttarch_offset / ttarch_chunksz; idx < ttarch_tot_idx; idx++) {
        if(currsz >= size) break;
        myfr(stream, in, ttarch_chunks[idx]);
        if(ttarch_chunks_b) blowfish(in, ttarch_chunks[idx], 0);
        if(ttarch_chunks[idx] == ttarch_chunksz) {
            len = ttarch_chunksz;
            memcpy(out, in, len);
        } else {
            len = do_decompress(in, ttarch_chunks[idx], out, ttarch_chunksz);
        }
        if(ttarch_rem) {
            if(ttarch_rem > len) {
                ttarch_rem -= len;
                continue;
            }
            len -= ttarch_rem;
            mymemmove(out, out + ttarch_rem, len);
            ttarch_rem = 0;
        }
        currsz += len;
        if(currsz > size) {
            len -= (currsz - size);
            currsz = size;
        }
        if(ptr) {
            memcpy(ptr, out, len);
            ptr += len;
        }
        if(currsz >= size) break;
    }
    ttarch_offset += currsz;
    ttarch_rem    = ttarch_offset % ttarch_chunksz;
    return(currsz);
}



void xor(u8 *data, u64 datalen, int xornum) {
    u64     i;

    for(i = 0; i < datalen; i++) {
        data[i] ^= xornum;
    }
}



void blowfish(u8 *data, u64 datalen, int encrypt) {
    static  blf_ctx *blowfish_ctx = NULL;
    static  int old_version = -1;

    if(!blowfish_ctx || (version != old_version)) { // init
        if(!blowfish_ctx) {
            blowfish_ctx = calloc(1, sizeof(blf_ctx));
            if(!blowfish_ctx) std_err();
        }
        if(version >= 7) {
            blf_key7(blowfish_ctx, mykey, strlen(mykey));
        } else {
            blf_key(blowfish_ctx, mykey, strlen(mykey));
        }
    }

    if(!data) return;

    // with ttarch2 version is ever a big number

    if(encrypt) {
        if(version >= 7) {
            blf_enc7(blowfish_ctx, (void *)data, datalen / 8);
        } else {
            blf_enc (blowfish_ctx, (void *)data, datalen / 8);
        }
    } else {
        if(version >= 7) {
            blf_dec7(blowfish_ctx, (void *)data, datalen / 8);
        } else {
            blf_dec (blowfish_ctx, (void *)data, datalen / 8);
        }
    }
}



u8 *scan_search(u8 *buff, u64 *buffsz, u8 *find, int findsz) {
#define MAX_SCAN_SIZE   4096
#define MAX_SCAN_CRYPT  8   // blowfish size
    u64     i,
            tmpsz;
    u8      tmp[MAX_SCAN_CRYPT];

    if(findsz > *buffsz) return(NULL);
    if(findsz > MAX_SCAN_CRYPT) {
        printf("\nError: you need to modify MAX_SCAN_CRYPT in scan_search\n");
        exit(1);
    }

    tmpsz = MAX_SCAN_SIZE;   // it's not needed to scan the whole file
    if(*buffsz < tmpsz) tmpsz = *buffsz;
    tmpsz -= findsz;

    for(i = 0; i <= tmpsz; i++) {
        if(!memcmp(buff + i, find, findsz)) {
            buff    += i;
            *buffsz -= i;
            return(buff);
        }
    }

    for(i = 0; i <= tmpsz; i++) {
        memcpy(tmp, buff + i, MAX_SCAN_CRYPT);
        blowfish(tmp, MAX_SCAN_CRYPT, 0);
        if(!memcmp(tmp, find, findsz)) {
            buff    += i;
            *buffsz -= i;
            blowfish(buff, 0x800, 0);
            return(buff);
        }
    }
    return(NULL);
}



u8 *ttarch_meta_dump(u8 *ext, u8 *data, u64 *datalen) { // completely experimental and horrible, ignores the classes
    u32     file_type;
    u64     size;
    u32     t,
            t2;
    u8      *p,
            *ret,
            tmp[16];

    p    = data;
    size = *datalen;
    if(!meta_extract) return(data);
    if(size < 4) return(data);

    file_type = ttarch_getxx(NULL, &p, 4);

    ret = NULL;

    if((file_type & 0xffffff00) == 0x4d535600) {   // 5VSM, 6VSM, *VSM
        t = ttarch_getxx(NULL, &p, 4) + ttarch_getxx(NULL, &p, 4);
        t2 = ttarch_getxx(NULL, &p, 4); // I'm in doubt about this value
        if((t + t2) <= size) t += t2;
        if(t > size) t = size;
        p = data + size - t;
        size = t;
        ret = p;
    }

    if(!stricmp(ext, ".font") || !stricmp(ext, ".d3dtx")) {
        size -= 4;
        ret = scan_search(p, &size, "DDS ", 4);
        if(ret) strcpy(ext, ".dds");
        else    { ret = p; size += 4; } // restore

    } else if(!stricmp(ext, ".aud")) {
        size -= 4;
        ret = scan_search(p, &size, "OggS", 4);
        if(ret) strcpy(ext, ".ogg");
        else    { ret = p; size += 4; } // restore

    } else if(!stricmp(ext, ".lenc") || !stricmp(ext, ".lua")) {

        if(IS_LUA2(data)) {
            if((version >= 0) && (version < 7)) version = 7; // do NOT enable this: else version = 1;
            blowfish(data + 4, size - 4, 0);
            memcpy(data, "\x1bLua", 4);

        } else if(IS_LUA3(data)) {
            if((version >= 0) && (version < 7)) version = 7; // do NOT enable this: else version = 1;
            blowfish(data + 4, size - 4, 0);
            size -= 4;  // no header
            ret = data + 4;

        } else {
            memcpy(tmp, data, 8);
            blowfish(tmp, 8, 0);
            if(!IS_LUA(tmp)) {
                if((version >= 0) && (version < 7)) version = 7; else version = 1;
                memcpy(tmp, data, 8);
                blowfish(tmp, 8, 0);
                if(!IS_LUA(tmp)) {
                    return data;
                    //printf("\nError: the input lenc/lua file can't be decrypted (the header doesn't match)\n");
                    //exit(1);
                }
            }
            blowfish(data, size, 0);
        }
        strcpy(ext, ".lua");
    }
    if(ret && (size >= 0)) {
        *datalen = size;
    } else {
        ret = data;
    }
    return(ret);
}



int mymemmove(u8 *dst, u8 *src, int size) {
    int     i;

    if(!dst || !src) return(0);
    if(size < 0) size = strlen(src) + 1;
    if(dst < src) {
        for(i = 0; i < size; i++) {
            dst[i] = src[i];
        }
    } else {
        for(i = size - 1; i >= 0; i--) {
            dst[i] = src[i];
        }
    }
    return(i);
}



int ttarch_dumpa(u8 *fname, u8 *data, u64 size, int already_decrypted) {
    int     more_size   = 0;
    u8      *more       = NULL,
            *ext,
            *p;

    if(!already_decrypted) {
        ttarch_meta_crypt(data, size, 0);
    }
    ext = strrchr(fname, '.');
    if(ext) {
        // force the decryption of lenc files, it seems a good idea!
        int old_meta_extract = meta_extract;
        if(!stricmp(ext, ".lenc") || !stricmp(ext, ".lua")) meta_extract = 1;
        if(meta_extract) {
            data = ttarch_meta_dump(ext, data, &size);
            if(!stricmp(ext, ".landb") && ttgtools_fix && memcmp(data, "ERTM", 4)) {
                more_size = 4 + 4 + 0x60;
                more = calloc(more_size, 1);
                if(!more) std_err();
                memcpy(more, "ERTM", 4);
                putxx(more + 4, 8, 4);
                memset(more + 8, 0xff, 0x60);
            }
        }
        meta_extract = old_meta_extract;
    }

    // the following is a set of filename cleaning instructions to avoid that files or data with special names are not saved
    if(fname) {
        if(fname[1] == ':') fname += 2;
        for(p = fname; *p && (*p != '\n') && (*p != '\r'); p++) {
            if(strchr("?%*:|\"<>", *p)) {    // invalid filename chars not supported by the most used file systems
                *p = '_';
            }
        }
        *p = 0;
        for(p--; (p >= fname) && ((*p == ' ') || (*p == '.')); p--) *p = 0;   // remove final spaces and dots
    }

    //if(filter_files && (check_wildcard(fname, filter_files) < 0)) return(0);
    //printf("  %08x %-10u %s\n", offset, size, fname);

    fname = create_dir(fname);
    if(check_overwrite(fname) < 0) return(0);

    dumpa(fname, data, size, more, more_size);

    extracted_files++;
    FREE(more)
    return(0);
}



i64 _z_decompress(u8 *in, i64 insz, u8 *out, i64 outsz) {
    static z_stream *z_zlib     = NULL;
    static z_stream *z_deflate  = NULL;
    z_stream *z;

#define UNZIP_INIT(X,Y) \
    if(!z_##X) { \
        z_##X = calloc(1, sizeof(z_stream)); \
        if(!z_##X) std_err(); \
        z_##X->zalloc = (alloc_func)0; \
        z_##X->zfree  = (free_func)0; \
        z_##X->opaque = (voidpf)0; \
        if(inflateInit2(z_##X, Y)) { \
            printf("\nError: "#X" initialization error\n"); \
            exit(1); \
        } \
    }
#define UNZIP_END(X) \
        if(z_##X) { \
            inflateEnd(z_##X); \
            FREE(z_##X) \
        }

    if(!insz || !outsz) return(0);
    if(!in && !out) {
        UNZIP_END(zlib)
        UNZIP_END(deflate)
        return(-1);
    }

    UNZIP_INIT(zlib,    15)
    UNZIP_INIT(deflate, -15)
    z = z_zlib;
redo:
    inflateReset(z);

    z->next_in   = in;
    z->avail_in  = insz;
    z->next_out  = out;
    z->avail_out = outsz;
    if(inflate(z, Z_FINISH) != Z_STREAM_END) {
        if(z == z_zlib) {
            z = z_deflate;
            goto redo;
        }
        printf("\nError: the compressed zlib/deflate input at offset 0x%08x (%d -> %d) is wrong or incomplete\n", (int)g_dbg_offset, (int)insz, (int)outsz);
        exit(1);
    }
    return(z->total_out);
}



// imported from QuickBMS
#ifdef WIN32
#include "MemoryModule.c"
#define QUICKBMS_ERROR_DLL  1
#define myexit  exit
#include "oodle.c"
#endif



i64 _oodle_decompress(u8 *in, i64 insz, u8 *out, i64 outsz) {
    i64 ret = 0;
#ifdef WIN32
    oodle_init();
    ret = OodleLZ_Decompress(in, insz, out, outsz, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, 3);
#endif
    if(!ret) {
        printf("\nError: the compressed oodle input at offset 0x%08x (%d -> %d) is wrong or incomplete\n", (int)g_dbg_offset, (int)insz, (int)outsz);
        exit(1);
    }
    return ret;
}



int check_wildcard(u8 *fname, u8 *wildcard) {
    u8      *f      = fname,
            *w      = wildcard,
            *last_w = NULL,
            *last_f = NULL;

    if(!fname) return -1;
    if(!wildcard) return -1;
    while(*f || *w) {
        if(!*w && !last_w) return -1;
        if(*w == '?') {
            if(!*f) break;
            w++;
            f++;
        } else if(*w == '*') {
            w++;
            last_w = w;
            last_f = f;
        } else {
            if(!*f) break;
            if(((*f == '\\') || (*f == '/')) && ((*w == '\\') || (*w == '/'))) {
                f++;
                w++;
            } else if(tolower(*f) != tolower(*w)) {
                if(!last_w) return -1;
                w = last_w;
                if(last_f) f = last_f;
                f++;
                if(last_f) last_f = f;
            } else {
                f++;
                w++;
            }
        }
    }
    if(*f || *w) return -1;
    return 0;
}



u8 *create_dir(u8 *fname) {
    u8      *p,
            *l;

    p = strchr(fname, ':'); // unused
    if(p) {
        *p = '_';
        fname = p + 1;
    }
    for(p = fname; *p && strchr("\\/. \t:", *p); p++) *p = '_';
    fname = p;

    for(p = fname; ; p = l + 1) {
        for(l = p; *l && (*l != '\\') && (*l != '/'); l++);
        if(!*l) break;
        *l = 0;

        if(!strcmp(p, "..")) {
            p[0] = '_';
            p[1] = '_';
        }

        make_dir(fname);
        *l = PATHSLASH;
    }
    return(fname);
}



int check_overwrite(u8 *fname) {
    FILE    *fd;
    u8      ans[16];

    if(force_overwrite) return(0);
    if(!fname) return(0);
    fd = fopen(fname, "rb");
    if(!fd) return(0);
    fclose(fd);
    printf("- the file \"%s\" already exists\n  do you want to overwrite it (y/N/all)?\n", fname);
    fgets(ans, sizeof(ans), stdin);
    if(tolower(ans[0]) == 'y') return(0);
    if(tolower(ans[0]) == 'a') {
        force_overwrite = 1;
        return(0);
    }
    return(-1);
}



void myalloc(u8 **data, u64 wantsize, u64 *currsize) {
    u64     tmp = 0;

    if(!currsize) currsize = &tmp;
    if(!wantsize) return;
    if(wantsize <= *currsize) {
        if(*currsize > 0) return;
    }
    *data = realloc(*data, wantsize + MEMMOVE_SIZE);
    if(!*data) std_err();
    memset(*data + *currsize, 0, wantsize - *currsize);
    *currsize = wantsize;
}



u64 getxx(u8 *tmp, int bytes) {
    u64     num;
    int     i;

    num = 0;
    for(i = 0; i < bytes; i++) {
        num |= ((u64)tmp[i] << ((u64)i << (u64)3));
    }
    if(verbose) {
        switch(bytes) {
            case 1:  printf("            %02x\n", (u32)num);    break;
            case 2:  printf("            %04x\n", (u32)num);    break;
            case 8:  printf("            %"PRIx"\n",   num);    break;
            default: printf("            %08x\n", (u32)num);    break;
        }
    }
    return(num);
}



u64 fgetxx(FILE *fd, int bytes) {
    u64     ret;
    u8      tmp[bytes];

    myfr(fd, tmp, bytes);
    ret = getxx(tmp, bytes);
    //if(verbose) printf("  %08x: %08x\n", (u32)ftell(fd) - bytes, (u32)ret);
    return(ret);
}



u64 myfr(FILE *fd, u8 *data, u64 size) {
    u64     len;

    g_dbg_offset = ftell(fd);
    if(!data) {
        for(len = 0; len < size; len++) {
            if(fgetc(fd) < 0) break;
        }
    } else {
        len = fread(data, 1, size, fd);
    }
    if(len != size) {
        printf("\nError: incomplete input file, can't read %u bytes\n", (u32)(size - len));
        exit(1);
    }
    return(len);
}



int putxx(u8 *data, u64 num, int bytes) {
    int     i;

    for(i = 0; i < bytes; i++) {
        data[i] = (u64)num >> ((u64)i << (u64)3);
    }
    return(bytes);
}



int fputxx(FILE *fd, u64 num, int bytes) {
    u8      tmp[bytes];

    putxx(tmp, num, bytes);
    myfw(fd, tmp, bytes);
    return(bytes);
}



void dumpa(u8 *fname, u8 *data, u64 size, u8 *more, int more_size) {
    FILE    *fdo;

    fdo = fopen(fname, "wb");
    if(!fdo) std_err();
    if(more_size > 0) myfw(fdo, more, more_size);
    myfw(fdo, data, size);
    fclose(fdo);
}



u64 myfw(FILE *fd, u8 *data, u64 size) {
    u64     len;

    len = fwrite(data, 1, size, fd);
    if(len != size) {
        printf("\nError: impossible to write %u bytes\n", (u32)(size - len));
        exit(1);
    }
    return(len);
}



u64 get_num(u8 *str) {
    //u64     offset;
    int     off32;  // currently this is not important

    if(!strncmp(str, "0x", 2) || !strncmp(str, "0X", 2)) {
        sscanf(str + 2, "%x", &off32);
    } else {
        sscanf(str, "%u", &off32);
    }
    return(off32);
}



void std_err(void) {
    perror("Error");
    exit(1);
}


