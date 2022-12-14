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

#if defined(HAVE_WORKING_SNPRINTF)

/* snprintf works, nothing to do */

#else

/* snprintf is b0rken, use vsnprintf */

#include <stdio.h>
#include <stdarg.h>

int rpl_snprintf(char *str, size_t size, const char *format, ...){
    va_list ap;
    int r;

    fprintf(stderr, "Using snprintf replacement\n");

    va_start(ap, format);
    r=vsnprintf(str, size, format, ap);
    va_end(ap);
    return r;
}

#endif
