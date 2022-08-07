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

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <string.h>
#include <limits.h>
#include <math.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "forecast.h"
#include "convert.h"
#include "getLine.h"
#include "die.h"

/* Important variables */
static struct forecast **forecasts=NULL;
static int num_forecasts=0;
static pcre2_code *date=NULL;
static int changed=0;

/* functions */

time_t find_next_time(char *file, char *pat, int minutes){
    FILE *fp;
    char *s;
    time_t t, mintime;

    mintime=time(NULL)/60+15;
    if((fp=fopen(file, "r"))==NULL) return mintime;

    s=NULL;
    while(!feof(fp)){
        getLine(&s, fp);
        if(strstr(s, pat)!=NULL) break;
        free(s);
        s=NULL;
    }
    fclose(fp);
    if(s==NULL) return mintime;
    t=parse_time_string(s)/60+minutes;
    free(s);
    return (t>mintime)?t:mintime;
}

time_t parse_time_string(char *s){
    struct tm tm;
    pcre2_match_data *match_data;
    char *v;
    int e;
    PCRE2_SIZE o;
    int ret;

    if(date==NULL){
        date=pcre2_compile((PCRE2_UCHAR*)"\\b(\\d+)/(\\d+)/(\\d+)\\s+(\\d\\d)(\\d\\d)\\s*UTC\\b", PCRE2_ZERO_TERMINATED, 0, &e, &o, NULL);
        if(date==NULL){
            v=pcre2_alloc_get_error(e);
            warn("find_next PCRE2 compile error: %s at %ld", v, o);
            free(v);
            return -1;
        }
        return parse_time_string(s);
    }

    match_data=pcre2_match_data_create_from_pattern(date, NULL);
    if(!match_data) die("Memory allocation failed");
    ret=0;
    do {
        e=pcre2_match(date, (PCRE2_UCHAR*)s, strlen(s), 0, 0, match_data, NULL);
        if(e==PCRE2_ERROR_NOMATCH) break;
        if(e<0){
            ret=-1;
            v=pcre2_alloc_get_error(e);
            warn("find_next PCRE2 match error: %s", v);
            free(v);
            break;
        }

        if(pcre2_substring_get_bynumber(match_data, 1, (PCRE2_UCHAR**)&v, &o)<0) break;
        tm.tm_mon=atoi(v)-1;
        pcre2_substring_free((PCRE2_UCHAR*)v);
        if(pcre2_substring_get_bynumber(match_data, 2, (PCRE2_UCHAR**)&v, &o)<0) break;
        tm.tm_mday=atoi(v);
        pcre2_substring_free((PCRE2_UCHAR*)v);
        if(pcre2_substring_get_bynumber(match_data, 3, (PCRE2_UCHAR**)&v, &o)<0) break;
        tm.tm_year=atoi(v)-1900;
        pcre2_substring_free((PCRE2_UCHAR*)v);
        if(pcre2_substring_get_bynumber(match_data, 4, (PCRE2_UCHAR**)&v, &o)<0) break;
        tm.tm_hour=atoi(v);
        pcre2_substring_free((PCRE2_UCHAR*)v);
        if(pcre2_substring_get_bynumber(match_data, 5, (PCRE2_UCHAR**)&v, &o)<0) break;
        tm.tm_min=atoi(v);
        pcre2_substring_free((PCRE2_UCHAR*)v);
        tm.tm_sec=0;

        ret=mkgmtime(&tm);
    } while(0);

    pcre2_match_data_free(match_data);
    return ret;
}

void add_forecast(struct forecast *f, char *ID, char *station){
    if((forecasts=realloc(forecasts, ++num_forecasts*sizeof(*forecasts)))==NULL)
        die("realloc in add_forecast");

    if(ID==NULL){
        memset(f->ID, '\0', 4);
    } else {
        strncpy(f->ID, ID, 3);
        f->ID[3]='\0';
    }
    f->station=station;
    forecasts[num_forecasts-1]=f;
    changed=1;
}

void reset_forecast(struct forecast *f){
    f->last_update=time(NULL);
    f->month=0;
    f->day=-1;
    f->year=SHRT_MIN;
    f->wday=-1;
    f->hour=-1;
    f->low=999;
    f->high=999;
    f->temp=999;
    f->dewpt=999;
    f->rh=-1;
    f->winddir=-1;
    f->windspeed=-1;
    f->heatindex=999;
    f->windchill=999;
    f->precipamt=-1;
    f->snowamt=-1;
    f->sky=-1;
    f->vis=7;
    f->obs=0;
    f->pcp_total=0;
    f->frz=0;
    f->snow=0;
    f->rain=0;
    f->tstorm=0;
    f->svtstorm=0;
    f->moon=NAN;
    f->time=-1;
    changed=1;
}

static int is_forecast_valid(const struct forecast *a){
    return (a->ID[0]!='\0' &&
            a->month>0 && a->month<=12 &&
            a->day>0 && a->day<=31 &&
            a->year!=SHRT_MIN);
}

static int is_forecast_current(struct forecast *f, time_t now){
    time_t t;

    t=forecast_time(f);
    t+=(f->hour<0)?86399:3599;
    return t>now;
}

static int compar(const void *aa, const void *bb){
    struct forecast *a=*(struct forecast **)aa;
    struct forecast *b=*(struct forecast **)bb;
    int i, j;

    /* First, any undefined forecast is greater than any defined forecast */
    i=is_forecast_valid(a);
    j=is_forecast_valid(b);
    if(!i && !j) return 0; /* all undef forecasts are equal */
    if(!i) return 1;
    if(!j) return -1;

    /* Any whole-day forecast is greater than any partial forecast */
    if(a->hour<0 && b->hour>=0) return 1;
    if(a->hour>=0 && b->hour<0) return -1;

    /* Ok, compare dates now */
    if(a->year>b->year) return 1;
    if(a->year<b->year) return -1;
    if(a->month>b->month) return 1;
    if(a->month<b->month) return -1;
    if(a->day>b->day) return 1;
    if(a->day<b->day) return -1;
    if(a->hour>b->hour) return 1;
    if(a->hour<b->hour) return -1;

    /* Last resort, sort in alphabetical order by ID */
    return strcasecmp(a->ID, b->ID);
}

static void sort_forecasts(void){
    if(forecasts==NULL) return;
    qsort(forecasts, num_forecasts, sizeof(struct forecast *), compar);
    changed=0;
}

time_t forecast_time(struct forecast *f){
    struct tm tm;

    if(f->time!=-1) return f->time;
    tm.tm_year=f->year;
    tm.tm_mon=f->month-1;
    tm.tm_mday=f->day;
    tm.tm_hour=(f->hour<0)?0:f->hour;
    tm.tm_min=tm.tm_sec=0;
    return (f->time=mktime(&tm));
}

static char current_ID[4]={ '\0', '\0', '\0', '\0' };
static int current_index=-1;
static struct forecast *current=NULL;
static time_t current_time=0;
static int current_hour=0;

static void set_current(int i){
    current_index=i;
    if(i<0 || i>num_forecasts){
        current=NULL;
        memset(current_ID, 0, 4);
        current_time=0;
        current_hour=0;
    } else {
        current=forecasts[i];
        memcpy(current_ID, current->ID, 4);
        current_time=forecast_time(current);
        current_hour=current->hour;
    }
}

static void locate_current(void){
    int i;
    time_t now, target;
    int target_hour;
    long curdiff=0;
    long tmpdiff;
    char target_ID[4];

    now=time(NULL);
    if(!changed && current!=NULL && is_forecast_current(current, now)) return;

    sort_forecasts();
    target=current_time;
    target_hour=current_hour;
    memcpy(target_ID, current_ID, 4);
    set_current(-1);

    for(i=0; i<num_forecasts; i++){
        if(!is_forecast_valid(forecasts[i])) continue;
        if(!is_forecast_current(forecasts[i], now)) continue;
        tmpdiff=abs(forecast_time(forecasts[i])-target);
        if((target_hour<0 && forecasts[i]->hour>=0) ||
           (target_hour>=0 && forecasts[i]->hour<0))
            tmpdiff+=31556926;
        if(memcmp(forecasts[i]->ID, target_ID, 4)) tmpdiff++;
        if(current==NULL || tmpdiff<curdiff){
            set_current(i);
            curdiff=tmpdiff;
        }
    }
}

struct forecast *current_forecast_get(void){
    locate_current();
    return current;
}

static inline int mod(int i, int n){
    i=i%n; if(i<0) i+=n;
    return i;
}

void current_forecast_next(int dir){
    int i;
    time_t now;

    if(num_forecasts==0) return;

    locate_current();
    now=time(NULL);

    if(current_index<0 || current_index>num_forecasts) current_index=0;
    for(i=mod(current_index+dir, num_forecasts); ; i=mod(i+dir, num_forecasts)){
        if(is_forecast_valid(forecasts[i]) && is_forecast_current(forecasts[i], now)){
            set_current(i);
            return;
        }
        if(i==current_index){
            set_current(-1);
            return;
        }
    }
}
