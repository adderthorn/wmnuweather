#include "config.h"

/*  Copyright (C) 2002  Brad Jorsch <anomie@users.sourceforge.net>

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
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "wmweather+.h"
#include "die.h"

void vwarn(char *fmt, va_list ap){
    fprintf(stderr, "%s: ", ProgName);
    vfprintf(stderr, fmt, ap);
    if (errno) fprintf(stderr, ": %s", strerror(errno));
    fprintf(stderr, "\n");
}

void warn(char *fmt, ...){
    va_list argv;

    va_start(argv, fmt);
    vwarn(fmt, argv);
    va_end(argv);
}


void vdie(char *fmt, va_list ap){
    vwarn(fmt, ap);
    exit(1);
}

void die(char *fmt, ...){
    va_list argv;

    va_start(argv, fmt);
    vwarn(fmt, argv);
    va_end(argv);
    exit(1);
}

size_t len = 128;
char *pcre2_alloc_get_error(int e) {
    char *b = malloc(len);
    int e2;

    if(!b){
        die("Memory allocation failed");
    }
    while(1){
        e2 = pcre2_get_error_message(e, (PCRE2_UCHAR*)b, len);
        if(e2>=0){
            return b;
        } else if(e2==PCRE2_ERROR_BADDATA){
            snprintf(b, len, "Unknown error code %d", e);
            return b;
        } else if(e2==PCRE2_ERROR_NOMEMORY){
            len *= 2;
            b = realloc(b, len);
            if(!b){
                die("Memory allocation failed");
            }
        } else {
            die("Unknown error code %d from pcre2_get_error_message", e2);
        }
    }
}
