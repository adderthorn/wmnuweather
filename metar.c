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
#include <unistd.h>
#include <time.h>
#if HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "wmweather+.h"
#include "metar.h"
#include "warnings.h"
#include "download.h"
#include "convert.h"
#include "die.h"
#include "sunzenith.h"
#include "moon.h"
#include "subst.h"

/* Important variables */
static time_t metar_time=0;

static char *metar_newfile=NULL;
static char *metar_file=NULL;
static char *metar_req[2]={ NULL, NULL };

struct current_weather current;

/* Regular Expressions */
static pcre2_code *station_time;
static pcre2_code *wind;
static pcre2_code *weather;
static pcre2_code *vis[4];
static pcre2_code *temp;
static pcre2_code *pressure;

/* prototypes */
static int parse_metar(char *file);

/* functions */

static void reset_current(struct current_weather *c){
    c->last_update=time(NULL);
    c->month=0;
    c->date=-1;
    c->time=-1;
    c->temp=999;
    c->rh=-1;
    c->winddir=-1;
    c->windspeed=-1;
    c->pressure=-1;
    c->heatindex=999;
    c->windchill=999;
    c->sky=-1;
    c->vis=7;
    c->obs=0;
    c->frz=0;
    c->snow=0;
    c->rain=0;
    c->tstorm=0;
    c->moon=NAN;
}

#define compile(var, re) \
    var=pcre2_compile((PCRE2_UCHAR*)re, PCRE2_ZERO_TERMINATED, 0, &e, &o, NULL); \
    if(var==NULL){\
        die("init_metar PCRE2 compile error: %s at %ld", pcre2_alloc_get_error(e), o); \
    }

void init_metar(void){
    int e;
    PCRE2_SIZE o;
    struct subst_val subs[]={
        { 's', STRING, &metar_station },
        { 0, 0, 0 }
    };
    
    snprintf(bigbuf, BIGBUF_LEN, "%s.metar.txt", metar_station);
    metar_file=get_pid_filename(bigbuf);
    snprintf(bigbuf, BIGBUF_LEN, "%s.new-metar.txt", metar_station);
    metar_newfile=get_pid_filename(bigbuf);

    if((metar_req[0]=subst(metar_uri, subs))==NULL) die("init_metar");
    if(metar_post!=NULL && (metar_req[1]=subst(metar_post, subs))==NULL) die("init_metar");

    metar_time=0;

    strncpy(bigbuf, metar_station, BIGBUF_LEN-25);
    bigbuf[BIGBUF_LEN-25]='\0';
    strcat(bigbuf, " ((?:\\d\\d)?)(\\d\\d\\d\\d)Z( .* )");
    compile(station_time, bigbuf);
    compile(wind, " (VRB|\\d\\d\\d)(\\d\\d\\d?)(?:G\\d\\d\\d?)?(KT|MPS|KMH)((?: \\d\\d\\dV\\d\\d\\d)?) ");
    compile(weather, " ((?:-|\\+|VC)?)((?:MI|PR|BC|DR|BL|SH|TS|FZ)?)((?:DZ|RA|SN|SG|IC|PE|PL|GR|GS|UP){0,3})((?:BR|FG|FU|VA|DU|SA|HZ|PY)?)((?:PO|SQ|FC|SS|DS)?)\\b");
    compile(vis[0], " (\\d+)SM ");
    compile(vis[1], " (\\d+)/(\\d+)SM ");
    compile(vis[2], " (\\d+) (\\d+)/(\\d+)SM ");
    compile(vis[3], " (\\d{4})[NS]?[EW]? ");
    compile(temp, " (M?\\d\\d\\d?)/((?:M?\\d\\d\\d?)?) ");
    compile(pressure, " ([AQ])(\\d\\d\\d\\d) ");

    /* Remove stale file */
    unlink(metar_file);
    unlink(metar_newfile);
    reset_current(&current);
    current.last_update = 0; // This was not a real "update", just an init
}
#undef compile

static void metar_callback(char *filename, void *v){
    struct stat statbuf;

    if(stat(metar_newfile, &statbuf)>=0){
        if(S_ISREG(statbuf.st_mode) && statbuf.st_size!=0
           && parse_metar(metar_newfile)){
            rename(metar_newfile, metar_file);
        } else {
            unlink(metar_newfile);
            if(!parse_metar(metar_file)) reset_current(&current);
        }
    }

    update_warnings(v!=NULL);
}

void metar_cleanup(void){
    unlink(metar_newfile);
    unlink(metar_file);
}

void update_metar(int force){
    time_t t;
    
    t=time(NULL)/60;
    if(!force && metar_time>t) return;

    metar_time=t+15;
    download_file(metar_newfile, metar_req[0], metar_req[1], force?DOWNLOAD_KILL_OTHER_REQUESTS:0, metar_callback, force?"":NULL);
}


static int parse_metar(char *file){
    FILE *fp;
    char *s, *c;
    pcre2_match_data *match_data;
    int e;
    PCRE2_SIZE o, slen;
    int len;
    float f;
    PCRE2_SIZE i, j;

    reset_current(&current);
    if((fp=fopen(file, "r"))==NULL) return 0;
    len=fread(bigbuf, sizeof(char), BIGBUF_LEN-2, fp);
    fclose(fp);
    if(len<1) return 0;
    for(i=0; i<len; i++){
        if(isspace(bigbuf[i])) bigbuf[i]=' ';
    }
    c=strstr(bigbuf, " RMK");
    if(c!=NULL) *(c+1)='\0';
    c=strstr(bigbuf, " TEMPO");
    s=strstr(bigbuf, " BECMG");
    if(c!=NULL) *(c+1)='\0';
    if(s!=NULL) *(s+1)='\0';
    /* XXX: parse trend forecast data? */

    len=strlen(bigbuf);
    if(bigbuf[len-1]!=' '){
        bigbuf[len++]=' ';
        bigbuf[len]='\0';
    }

#define RET0 { pcre2_match_data_free(match_data); return 0; }
    
    /* Look for something like a METAR coded report */
    match_data=pcre2_match_data_create_from_pattern(station_time, NULL);
    if(!match_data) die("Memory allocation failed");
    e=pcre2_match(station_time, (PCRE2_UCHAR*)bigbuf, len, 0, 0, match_data, NULL);
    if(e<=0) RET0;
    if(pcre2_substring_get_bynumber(match_data, 1, (PCRE2_UCHAR**)&c, &o)<0) RET0;
    if(c[0]!='\0') current.date=atoi(c);
    pcre2_substring_free((void*)c);
    if(pcre2_substring_get_bynumber(match_data, 2, (PCRE2_UCHAR**)&c, &o)<0) RET0;
    current.time=atoi(c);
    pcre2_substring_free((void*)c);

    /* Chop off extraneous stuff */
    if(pcre2_substring_get_bynumber(match_data, 3, (PCRE2_UCHAR**)&s, &slen)<0) RET0;

#undef RET0
#define RET0 { pcre2_substring_free((void*)s); pcre2_match_data_free(match_data); return 0; }
#define match(pat) \
    pcre2_match_data_free(match_data); \
    match_data=pcre2_match_data_create_from_pattern(pat, NULL); \
    if(!match_data) die("Memory allocation failed"); \
    e=pcre2_match(pat, (PCRE2_UCHAR*)s, slen, 0, 0, match_data, NULL);
#define get_substr(n, c) \
     if(pcre2_substring_get_bynumber(match_data, n, (PCRE2_UCHAR**)&c, &o)<0) RET0;

    /* windspeed, winddir */
    match(wind);
    if(e>0){
        get_substr(4, c);
        if(c[0]!='\0'){
            current.winddir=0;
        } else {
            pcre2_substring_free((void*)c);
            get_substr(1, c);
            if(c[0]=='V') current.winddir=0;
            else current.winddir=((int)((atoi(c)+11.25)/22.5))%16+1;
        }
        pcre2_substring_free((void*)c);
        get_substr(2, c);
        current.windspeed=atoi(c);
        pcre2_substring_free((void*)c);
        get_substr(3, c);
        if(c[0]=='M'){ /* MPS */
            current.windspeed=mps2knots(current.windspeed);
        } else if(c[0]=='K' && c[1]=='M'){ /* KMH */
            current.windspeed=kph2knots(current.windspeed);
        }
        pcre2_substring_free((void*)c);
    }

    /* vis */
    f=99;
    c=strstr(s, " M1/4SM ");
    if(c!=NULL){
        f=0;
        goto wind_done;
    }
    match(vis[2]);
    if(e>0){
        get_substr(2, c);
        i=atoi(c);
        pcre2_substring_free((void*)c);
        get_substr(3, c);
        j=atoi(c);
        pcre2_substring_free((void*)c);
        get_substr(1, c);
        f=atoi(c)+(float)i/j;
        pcre2_substring_free((void*)c);
        goto wind_done;
    }
    match(vis[1]);
    if(e>0){
        get_substr(2, c);
        i=atoi(c);
        pcre2_substring_free((void*)c);
        get_substr(1, c);
        f=(float)atoi(c)/i;
        pcre2_substring_free((void*)c);
        goto wind_done;
    }
    match(vis[0]);
    if(e>0){
        get_substr(1, c);
        f=atoi(c);
        pcre2_substring_free((void*)c);
        goto wind_done;
    }
    c=strstr(s, " CAVOK ");
    if(c!=NULL){
        f=99;
        current.sky=0;
        goto wind_done;
    }
    match(vis[3]);
    if(e>0){
        get_substr(1, c);
        f=m2mi(atoi(c));
        pcre2_substring_free((void*)c);
        goto wind_done;
    }
wind_done:
    if(f<=6) current.vis=6;
    if(f<=5) current.vis=5;
    if(f<3) current.vis=4;
    if(f<1) current.vis=3;
    if(f<=.5) current.vis=2;
    if(f<=.25) current.vis=1;

    /* temp, rh */
    match(temp);
    if(e>0){
        get_substr(1, c);
        if(c[0]=='M') c[0]='-';
        current.temp=atoi(c);
        pcre2_substring_free((void*)c);
        get_substr(2, c);
        if(c[0]!='\0'){
            if(c[0]=='M') c[0]='-';
            current.rh=rh_C(current.temp, atoi(c));
        }
        pcre2_substring_free((void*)c);
    }

    /* pressure */
    match(pressure);
    if(e>0){
        get_substr(2, c);
        i=atoi(c);
        pcre2_substring_free((void*)c);
        get_substr(1, c);
        if(c[0]=='Q'){
            current.pressure=hPa2inHg(i);
        } else {
            current.pressure=i/100.0;
        }
        pcre2_substring_free((void*)c);
    }

    /* sky */
    if(strstr(s, " SKC")!=NULL || strstr(s, " CLR")!=NULL) current.sky=0;
    if(strstr(s, " FEW")!=NULL) current.sky=1;
    if(strstr(s, " SCT")!=NULL) current.sky=2;
    if(strstr(s, " BKN")!=NULL) current.sky=3;
    if(strstr(s, " OVC")!=NULL || strstr(s, " VV")!=NULL) current.sky=4;

    /* obs, frz, snow, rain, tstorm */
    /* There can be multiple weather chunks, so we while loop */
    pcre2_match_data_free(match_data);
    match_data=pcre2_match_data_create_from_pattern(weather, NULL);
    if(!match_data) die("Memory allocation failed"); \
    j=0;
    while((e=pcre2_match(weather, (PCRE2_UCHAR*)s, slen, j, 0, match_data, NULL))>0){{
        char *in, *de, *pp, *ob, *ot;

        j=pcre2_get_startchar(match_data)+1;
        get_substr(0, c);
        i=(c[1]=='\0');
        pcre2_substring_free((void*)c);
        if(i) continue;
        

        get_substr(1, in);
        get_substr(2, de);
        get_substr(3, pp);
        get_substr(4, ob);
        get_substr(5, ot);

#define IN(haystack, needle) ((needle[0]=='\0')?0:strstr(haystack, needle))
        if(current.obs<1 && strcmp(de, "FZ") && IN("BR|FG", ob))
            current.obs=1;
        if(current.obs<2 && IN("FU|VA|DU|SA|HZ|PY", ob))
            current.obs=2;
        if(current.obs<3 && IN("PO|SS|DS", ot))
            current.obs=3;
        if(current.obs<3 && IN("DR|BL", de)
           && (strstr(pp, "SN") || IN("DU|SA|PY", ob)))
            current.obs=3;
        if(!strcmp(ot, "FC")){
            current.sky=5;
            current.obs=99;
            current.vis=7;
        }
#undef IN

        i=66;
        if(in[0]=='-' || in[0]=='V') i=33;
        if(in[0]=='+') i=99;
        if(!strcmp(de, "SH")) i=33;
        if(current.frz<i
           && ((!strcmp(de, "FZ") && (strstr(pp, "DZ") || strstr(pp, "RA")))
               || strstr(pp, "IC") || strstr(pp, "PE") || strstr(pp, "PL")
               || strstr(pp, "GR") || strstr(pp, "GS")))
                current.frz=i;
        if(current.snow<i && strcmp(de, "BL")
           && (strstr(pp, "SN") || strstr(pp, "SG")))
            current.snow=i;
        if(current.rain<i && (strstr(pp, "UP")
                              || (strcmp(de, "FZ")
                                  && (strstr(pp, "DZ") || strstr(pp, "RA")))))
            current.rain=i;
        if(current.tstorm<i && !strcmp(de, "TS"))
            current.tstorm=i;

        pcre2_substring_free((void*)in);
        pcre2_substring_free((void*)de);
        pcre2_substring_free((void*)pp);
        pcre2_substring_free((void*)ob);
        pcre2_substring_free((void*)ot);
    }}
    if(current.obs==99) current.obs=0;
    
    /* Done parsing! Just a few final calculations... */
#undef RET0
#undef match
#undef get_substr
    pcre2_match_data_free(match_data);
    pcre2_substring_free((void*)s);

    current.heatindex=heatindex_C(current.temp, current.rh);
    current.windchill=windchill_C(current.temp, current.windspeed);

    /* Figure out the proper month... */
    {
        int mon, day, year, time2; /* holds UTC */
        int y; /* with current.*, holds local time */
        time_t t=time(NULL);
        struct tm *tm=gmtime(&t);
        current.month=tm->tm_mon+1;
        if(tm->tm_mday<current.date) current.month--;
        if(current.month<1){ current.month+=12; tm->tm_year--; }
        y=year=tm->tm_year;
        mon=current.month;
        day=current.date;
        time2=current.time;
        current.time=utc2local((int)current.time, &current.month, &current.date, &y, NULL);
    
        if(latitude!=999 && calcSolarZenith(latitude, longitude, year, mon, day, hm2min(time2))>90)
            current.moon=calc_moon(current.month, current.date, y, current.time);
    }
    return 1;
}
