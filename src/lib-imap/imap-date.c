/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "gmtoff.h"

#include <ctype.h>

static const char *month_names[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static int parse_timezone(const char *str)
{
	int offset;

	/* +|-zone */
	if ((*str != '+' && *str != '-') ||
	    !i_isdigit(str[1]) || !i_isdigit(str[2]) ||
	    !i_isdigit(str[3]) || !i_isdigit(str[4]))
		return 0;

	offset = (str[1]-'0') * 1000 + (str[2]-'0') * 100 +
		(str[3]-'0') * 10 + (str[4]-'0');
	return *str == '+' ? offset : -offset;
}

static const char *imap_parse_date_internal(const char *str, struct tm *tm)
{
	int i;

	if (str == NULL || *str == '\0')
		return NULL;

	memset(tm, 0, sizeof(struct tm));

	/* "dd-mon-yyyy [hh:mi:ss +|-zone]"
	   dd is 1-2 digits and may be prefixed with space or zero. */

	if (str[0] == ' ') {
		/* " d-..." */
		str++;
	}

	if (!(i_isdigit(str[0]) && (str[1] == '-' ||
				    (i_isdigit(str[1]) && str[2] == '-'))))
	      return NULL;

	tm->tm_mday = (str[0]-'0');
	if (str[1] == '-')
		str += 2;
	else {
		tm->tm_mday = (tm->tm_mday * 10) + (str[1]-'0');
		str += 3;
	}

	/* month name */
	for (i = 0; i < 12; i++) {
		if (strncasecmp(month_names[i], str, 3) == 0) {
			tm->tm_mon = i;
			break;
		}
	}
	if (i == 12 || str[3] != '-')
		return NULL;
	str += 4;

	/* yyyy */
	if (!i_isdigit(str[0]) || !i_isdigit(str[1]) ||
	    !i_isdigit(str[2]) || !i_isdigit(str[3]))
		return NULL;

	tm->tm_year = (str[0]-'0') * 1000 + (str[1]-'0') * 100 +
		(str[2]-'0') * 10 + (str[3]-'0') - 1900;

	str += 4;
	return str;
}

int imap_parse_date(const char *str, time_t *time)
{
	struct tm tm;

	str = imap_parse_date_internal(str, &tm);
	if (str == NULL)
		return FALSE;

	tm.tm_isdst = -1;
	*time = mktime(&tm);
	return *time >= 0;
}

int imap_parse_datetime(const char *str, time_t *time)
{
	struct tm tm;
	int zone_offset;

	str = imap_parse_date_internal(str, &tm);
	if (str == NULL)
		return FALSE;

	if (str[0] != ' ')
		return FALSE;
	str++;

	/* hh: */
	if (!i_isdigit(str[0]) || !i_isdigit(str[1]) || str[2] != ':')
		return FALSE;
	tm.tm_hour = (str[0]-'0') * 10 + (str[1]-'0');
	str += 3;

	/* mm: */
	if (!i_isdigit(str[0]) || !i_isdigit(str[1]) || str[2] != ':')
		return FALSE;
	tm.tm_min = (str[0]-'0') * 10 + (str[1]-'0');
	str += 3;

	/* ss */
	if (!i_isdigit(str[0]) || !i_isdigit(str[1]) || str[2] != ' ')
		return FALSE;
	tm.tm_sec = (str[0]-'0') * 10 + (str[1]-'0');
	str += 3;

	/* timezone */
	zone_offset = parse_timezone(str);

	tm.tm_isdst = -1;
	*time = mktime(&tm);
	if (*time < 0)
		return FALSE;

	*time -= zone_offset * 60;
	return TRUE;
}

const char *imap_to_datetime(time_t time)
{
	struct tm *tm;
	int offset, negative;

	tm = localtime(&time);
	offset = gmtoff(tm, time);
	if (offset >= 0)
		negative = 0;
	else {
		negative = 1;
		offset = -offset;
	}
	offset /= 60;

	return t_strdup_printf("%02d-%s-%04d %02d:%02d:%02d %c%02d%02d",
			       tm->tm_mday, month_names[tm->tm_mon],
			       tm->tm_year+1900,
			       tm->tm_hour, tm->tm_min, tm->tm_sec,
			       negative ? '-' : '+', offset / 60, offset % 60);
}

const char *imap_to_date(time_t time)
{
	struct tm *tm;

	tm = localtime(&time);
	return t_strdup_printf("%d-%s-%04d", tm->tm_mday,
			       month_names[tm->tm_mon], tm->tm_year+1900);
}
