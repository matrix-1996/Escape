/**
 * $Id$
 * Copyright (C) 2008 - 2009 Nils Asmussen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <esc/common.h>
#include <esc/date.h>
#include <time.h>

#define MAX_DATE_LEN	25

/**
 * Converts the given time-struct into the given date-struct
 *
 * @param timeptr the time-struct
 * @param date the date-struct to fill
 */
static void tmToDate(const struct tm *timeptr,sDate *date);
/**
 * Converts the given date-struct into the given time-struct
 *
 * @param timeptr the time-struct to fill
 * @param date the date-struct
 */
static void dateToTm(struct tm *timeptr,const sDate *date);

double difftime(time_t time2,time_t time1) {
	return (double)(time2 - time1);
}

time_t mktime(struct tm *timeptr) {
	sDate date;
	tmToDate(timeptr,&date);
	return getTimeOf(&date);
}

time_t time(time_t *timer) {
	time_t res = (time_t)getTime();
	if(timer)
		*timer = res;
	return res;
}

char *asctime(const struct tm *timeptr) {
	static char dateStr[MAX_DATE_LEN];
	strftime(dateStr,MAX_DATE_LEN,"%c",timeptr);
	return dateStr;
}

char *ctime(const time_t *timer) {
	return asctime(localtime(timer));
}

struct tm *gmtime(const time_t *timer) {
	static struct tm timeptr;
	sDate date;
	getDateOf(&date,*(u32*)timer);
	dateToTm(&timeptr,&date);
	return &timeptr;
}

struct tm *localtime(const time_t *timer) {
	/* TODO locale... */
	return gmtime(timer);
}

size_t strftime(char *ptr,size_t maxsize,const char *format,const struct tm *timeptr) {
	sDate date;
	tmToDate(timeptr,&date);
	return dateToString(ptr,maxsize,format,&date);
}

static void tmToDate(const struct tm *timeptr,sDate *date) {
	date->hour = timeptr->tm_hour;
	date->min = timeptr->tm_min;
	date->sec = timeptr->tm_sec;
	date->month = timeptr->tm_mon + 1;
	date->monthDay = timeptr->tm_mday;
	date->weekDay = timeptr->tm_wday + 1;
	date->yearDay = timeptr->tm_yday;
	date->year = timeptr->tm_year + 1900;
	date->isDst = timeptr->tm_isdst;
}

static void dateToTm(struct tm *timeptr,const sDate *date) {
	timeptr->tm_hour = date->hour;
	timeptr->tm_min = date->min;
	timeptr->tm_sec = date->sec;
	timeptr->tm_mon = date->month - 1;
	timeptr->tm_mday = date->monthDay;
	timeptr->tm_wday = date->weekDay - 1;
	timeptr->tm_yday = date->yearDay;
	timeptr->tm_year = date->year - 1900;
	timeptr->tm_isdst = date->isDst;
}
