/*
 * time.c:  time/date utilities
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include <string.h>
#include <apr_pools.h>
#include <apr_time.h>
#include <apr_strings.h>
#include "svn_time.h"



/*** Code. ***/

/* Our timestamp strings look like this:
 *
 *    "2002-05-07Thh:mm:ss.uuuuuuZ"
 *
 * The format is conformant with ISO-8601 and the date format required
 * by RFC2518 for creationdate. It is a direct converision between
 * apr_time_t and a string, so converting to string and back retains
 * the exact value.
 */
static const char * const timestamp_format =
"%04d-%02d-%02dT%02d:%02d:%02d.%06dZ";

/* Our old timestamp strings looked like this:
 * 
 *    "Tue 3 Oct 2000 HH:MM:SS.UUU (day 277, dst 1, gmt_off -18000)"
 *
 * The idea is that they are conventionally human-readable for the
 * first part, and then in parentheses comes everything else required
 * to completely fill in an apr_time_exp_t: tm_yday, tm_isdst,
 * and tm_gmtoff.
 *
 * This format is still recognized on input, for backward
 * compatibility, but no longer generated.
 */
static const char * const old_timestamp_format =
"%s %d %s %d %02d:%02d:%02d.%06d (day %03d, dst %d, gmt_off %06d)";

/* Our human representation of dates look like this:
 *
 *    "Sun, 23 Jun 2002 11:13:02 +0300"
 *
 * This format is used whenever time is shown to the user
 * directly.
 */
static const char * const human_timestamp_format =
"%3.3s, %.2d %3.3s %.2d %.2d:%.2d:%.2d %+.2d%.2d";


const char *
svn_time_to_nts (apr_time_t t, apr_pool_t *pool)
{
  const char *t_cstr;
  apr_time_exp_t exploded_time;

  /* We toss apr_status_t return value here -- for one thing, caller
     should pass in good information.  But also, where APR's own code
     calls these functions it tosses the return values, and
     furthermore their current implementations can only return success
     anyway. */

  /* We get the date in GMT now -- and expect the tm_gmtoff and
     tm_isdst to be not set. We also ignore the weekday and yearday,
     since those are not needed. */

  apr_time_exp_gmt (&exploded_time, t);

  /* It would be nice to use apr_strftime(), but APR doesn't give a
     way to convert back, so we wouldn't be able to share the format
     string between the writer and reader. */
  t_cstr = apr_psprintf (pool,
                         timestamp_format,
                         exploded_time.tm_year + 1900,
                         exploded_time.tm_mon + 1,
                         exploded_time.tm_mday,
                         exploded_time.tm_hour,
                         exploded_time.tm_min,
                         exploded_time.tm_sec,
                         exploded_time.tm_usec);

  /* ### Remove this when the old style timestamp parsing is taken
     out. 
  t_cstr = apr_psprintf (pool,
                         old_timestamp_format,
                         apr_day_snames[exploded_time.tm_wday],
                         exploded_time.tm_mday,
                         apr_month_snames[exploded_time.tm_mon],
                         exploded_time.tm_year + 1900,
                         exploded_time.tm_hour,
                         exploded_time.tm_min,
                         exploded_time.tm_sec,
                         exploded_time.tm_usec,
                         exploded_time.tm_yday + 1,
                         exploded_time.tm_isdst,
                         exploded_time.tm_gmtoff);
  */

  return t_cstr;
}


static int
find_matching_string (char *str, apr_size_t size, const char strings[][4])
{
  apr_size_t i;

  for (i = 0; i < size; i++)
    if (strings[i] && (strcmp (str, strings[i]) == 0))
      return i;

  return -1;
}


svn_error_t *
svn_time_from_nts(apr_time_t *when, const char *data, apr_pool_t *pool)
{
  apr_time_exp_t exploded_time;
  apr_status_t apr_err;
  char wday[4], month[4];

  /* First try the new timestamp format. */
  if (sscanf (data,
              timestamp_format,
              &exploded_time.tm_year,
              &exploded_time.tm_mon,
              &exploded_time.tm_mday,
              &exploded_time.tm_hour,
              &exploded_time.tm_min,
              &exploded_time.tm_sec,
              &exploded_time.tm_usec) == 7)
    {
      exploded_time.tm_year -= 1900;
      exploded_time.tm_mon -= 1;
      exploded_time.tm_wday = 0;
      exploded_time.tm_yday = 0;
      exploded_time.tm_isdst = 0;
      exploded_time.tm_gmtoff = 0;
      
      apr_err = apr_implode_gmt (when, &exploded_time);
      if(apr_err != APR_SUCCESS)
        {
          return svn_error_createf (SVN_ERR_BAD_DATE, apr_err, NULL, pool,
                                    "Date conversion failed.");
        }
      
      return SVN_NO_ERROR;
    }
  /* Then try the compatibility option. */
  else if (sscanf (data,
                   old_timestamp_format,
                   wday,
                   &exploded_time.tm_mday,
                   month,
                   &exploded_time.tm_year,
                   &exploded_time.tm_hour,
                   &exploded_time.tm_min,
                   &exploded_time.tm_sec,
                   &exploded_time.tm_usec,
                   &exploded_time.tm_yday,
                   &exploded_time.tm_isdst,
                   &exploded_time.tm_gmtoff) == 11)
    {
      exploded_time.tm_year -= 1900;
      exploded_time.tm_yday -= 1;
      /* Using hard coded limits for the arrays - they are going away
         soon in any case. */
      exploded_time.tm_wday = find_matching_string (wday, 7, apr_day_snames);
      exploded_time.tm_mon = find_matching_string (month, 12, apr_month_snames);

      apr_err = apr_implode_gmt (when, &exploded_time);
      if(apr_err != APR_SUCCESS)
        {
          return svn_error_createf (SVN_ERR_BAD_DATE, apr_err, NULL, pool,
                                    "Date conversion failed.");
        }

      return SVN_NO_ERROR;
    }
  /* Timestamp is something we do not recognize. */
  else
    {
      return svn_error_createf(SVN_ERR_BAD_DATE, 0, NULL, pool,
                               "Date parsing failed.");
    }
}


const char *
svn_time_to_human_nts (apr_time_t t, apr_pool_t *pool)
{
  apr_time_exp_t exploded_time;

  apr_time_exp_lt (&exploded_time, t);

  return apr_psprintf (pool,
                       human_timestamp_format,
                       apr_day_snames[exploded_time.tm_wday],
                       exploded_time.tm_mday,
                       apr_month_snames[exploded_time.tm_mon],
                       exploded_time.tm_year + 1900,
                       exploded_time.tm_hour,
                       exploded_time.tm_min,
                       exploded_time.tm_sec,
                       exploded_time.tm_gmtoff / (60 * 60),
                       (exploded_time.tm_gmtoff / 60) % 60);
}



/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end: */
