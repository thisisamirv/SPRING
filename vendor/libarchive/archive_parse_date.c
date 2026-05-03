/*
 * This code is in the public domain and has no copyright.
 *
 * This is a plain C recursive-descent translation of an old
 * public-domain YACC grammar that has been used for parsing dates in
 * very many open-source projects.
 *
 * Since the original authors were generous enough to donate their
 * work to the public domain, I feel compelled to match their
 * generosity.
 *
 * Tim Kientzle, February 2009.
 */

/*
**  Originally written by Steven M. Bellovin <smb@research.att.com> while
**  at the University of North Carolina at Chapel Hill.  Later tweaked by
**  a couple of people on Usenet.  Completely overhauled by Rich $alz
**  <rsalz@bbn.com> and Jim Berets <jberets@bbn.com> in August, 1990;
**
**  This grammar has 10 shift/reduce conflicts.
**
**  This code is in the public domain and has no copyright.
*/

#include "archive_platform.h"

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#error "archive_platform.h must be included first"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "archive.h"

#ifndef ARCHIVE_H_INCLUDED
#error "archive.h must be included"
#endif

#define EPOCH 1970
#define MINUTE (60L)
#define HOUR (60L * MINUTE)
#define DAY (24L * HOUR)

enum DSTMODE { DSTon, DSToff, DSTmaybe };

enum { tAM, tPM };

enum {
  tAGO = 260,
  tDAY,
  tDAYZONE,
  tAMPM,
  tMONTH,
  tMONTH_UNIT,
  tSEC_UNIT,
  tUNUMBER,
  tZONE,
  tDST
};
struct token {
  int token;
  time_t value;
};

struct gdstate {
  struct token *tokenp;

  int HaveYear;
  int HaveMonth;
  int HaveDay;
  int HaveWeekDay;
  int HaveTime;
  int HaveZone;
  int HaveRel;

  time_t Timezone;
  time_t Day;
  time_t Hour;
  time_t Minutes;
  time_t Month;
  time_t Seconds;
  time_t Year;

  enum DSTMODE DSTmode;

  time_t DayOrdinal;
  time_t DayNumber;

  time_t RelMonth;
  time_t RelSeconds;
};

static int timephrase(struct gdstate *gds) {
  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == ':' &&
      gds->tokenp[2].token == tUNUMBER && gds->tokenp[3].token == ':' &&
      gds->tokenp[4].token == tUNUMBER) {

    ++gds->HaveTime;
    gds->Hour = gds->tokenp[0].value;
    gds->Minutes = gds->tokenp[2].value;
    gds->Seconds = gds->tokenp[4].value;
    gds->tokenp += 5;
  } else if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == ':' &&
             gds->tokenp[2].token == tUNUMBER) {

    ++gds->HaveTime;
    gds->Hour = gds->tokenp[0].value;
    gds->Minutes = gds->tokenp[2].value;
    gds->Seconds = 0;
    gds->tokenp += 3;
  } else if (gds->tokenp[0].token == tUNUMBER &&
             gds->tokenp[1].token == tAMPM) {

    ++gds->HaveTime;
    gds->Hour = gds->tokenp[0].value;
    gds->Minutes = gds->Seconds = 0;

    gds->tokenp += 1;
  } else {

    return 0;
  }

  if (gds->tokenp[0].token == tAMPM) {

    if (gds->Hour == 12)
      gds->Hour = 0;
    if (gds->tokenp[0].value == tPM)
      gds->Hour += 12;
    gds->tokenp += 1;
  }
  if (gds->tokenp[0].token == '+' && gds->tokenp[1].token == tUNUMBER) {

    gds->HaveZone++;
    gds->DSTmode = DSToff;
    gds->Timezone = -((gds->tokenp[1].value / 100) * HOUR +
                      (gds->tokenp[1].value % 100) * MINUTE);
    gds->tokenp += 2;
  }
  if (gds->tokenp[0].token == '-' && gds->tokenp[1].token == tUNUMBER) {

    gds->HaveZone++;
    gds->DSTmode = DSToff;
    gds->Timezone = +((gds->tokenp[1].value / 100) * HOUR +
                      (gds->tokenp[1].value % 100) * MINUTE);
    gds->tokenp += 2;
  }
  return 1;
}

static int zonephrase(struct gdstate *gds) {
  if (gds->tokenp[0].token == tZONE && gds->tokenp[1].token == tDST) {
    gds->HaveZone++;
    gds->Timezone = gds->tokenp[0].value;
    gds->DSTmode = DSTon;
    gds->tokenp += 1;
    return 1;
  }

  if (gds->tokenp[0].token == tZONE) {
    gds->HaveZone++;
    gds->Timezone = gds->tokenp[0].value;
    gds->DSTmode = DSToff;
    gds->tokenp += 1;
    return 1;
  }

  if (gds->tokenp[0].token == tDAYZONE) {
    gds->HaveZone++;
    gds->Timezone = gds->tokenp[0].value;
    gds->DSTmode = DSTon;
    gds->tokenp += 1;
    return 1;
  }
  return 0;
}

static int datephrase(struct gdstate *gds) {
  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == '/' &&
      gds->tokenp[2].token == tUNUMBER && gds->tokenp[3].token == '/' &&
      gds->tokenp[4].token == tUNUMBER) {
    gds->HaveYear++;
    gds->HaveMonth++;
    gds->HaveDay++;
    if (gds->tokenp[0].value >= 13) {

      gds->Year = gds->tokenp[0].value;
      gds->Month = gds->tokenp[2].value;
      gds->Day = gds->tokenp[4].value;
    } else if ((gds->tokenp[4].value >= 13) || (gds->tokenp[2].value >= 13)) {

      gds->Month = gds->tokenp[0].value;
      gds->Day = gds->tokenp[2].value;
      gds->Year = gds->tokenp[4].value;
    } else {

      gds->Month = gds->tokenp[0].value;
      gds->Day = gds->tokenp[2].value;
      gds->Year = gds->tokenp[4].value;
    }
    gds->tokenp += 5;
    return 1;
  }

  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == '/' &&
      gds->tokenp[2].token == tUNUMBER) {

    gds->HaveMonth++;
    gds->HaveDay++;
    gds->Month = gds->tokenp[0].value;
    gds->Day = gds->tokenp[2].value;
    gds->tokenp += 3;
    return 1;
  }

  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == '-' &&
      gds->tokenp[2].token == tUNUMBER && gds->tokenp[3].token == '-' &&
      gds->tokenp[4].token == tUNUMBER) {

    gds->HaveYear++;
    gds->HaveMonth++;
    gds->HaveDay++;
    gds->Year = gds->tokenp[0].value;
    gds->Month = gds->tokenp[2].value;
    gds->Day = gds->tokenp[4].value;
    gds->tokenp += 5;
    return 1;
  }

  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == '-' &&
      gds->tokenp[2].token == tMONTH && gds->tokenp[3].token == '-' &&
      gds->tokenp[4].token == tUNUMBER) {
    gds->HaveYear++;
    gds->HaveMonth++;
    gds->HaveDay++;
    if (gds->tokenp[0].value > 31) {

      gds->Year = gds->tokenp[0].value;
      gds->Month = gds->tokenp[2].value;
      gds->Day = gds->tokenp[4].value;
    } else {

      gds->Day = gds->tokenp[0].value;
      gds->Month = gds->tokenp[2].value;
      gds->Year = gds->tokenp[4].value;
    }
    gds->tokenp += 5;
    return 1;
  }

  if (gds->tokenp[0].token == tMONTH && gds->tokenp[1].token == tUNUMBER &&
      gds->tokenp[2].token == ',' && gds->tokenp[3].token == tUNUMBER) {

    gds->HaveYear++;
    gds->HaveMonth++;
    gds->HaveDay++;
    gds->Month = gds->tokenp[0].value;
    gds->Day = gds->tokenp[1].value;
    gds->Year = gds->tokenp[3].value;
    gds->tokenp += 4;
    return 1;
  }

  if (gds->tokenp[0].token == tMONTH && gds->tokenp[1].token == tUNUMBER) {

    gds->HaveMonth++;
    gds->HaveDay++;
    gds->Month = gds->tokenp[0].value;
    gds->Day = gds->tokenp[1].value;
    gds->tokenp += 2;
    return 1;
  }

  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == tMONTH &&
      gds->tokenp[2].token == tUNUMBER) {

    gds->HaveYear++;
    gds->HaveMonth++;
    gds->HaveDay++;
    gds->Day = gds->tokenp[0].value;
    gds->Month = gds->tokenp[1].value;
    gds->Year = gds->tokenp[2].value;
    gds->tokenp += 3;
    return 1;
  }

  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == tMONTH) {

    gds->HaveMonth++;
    gds->HaveDay++;
    gds->Day = gds->tokenp[0].value;
    gds->Month = gds->tokenp[1].value;
    gds->tokenp += 2;
    return 1;
  }

  return 0;
}

static int relunitphrase(struct gdstate *gds) {
  if (gds->tokenp[0].token == '-' && gds->tokenp[1].token == tUNUMBER &&
      gds->tokenp[2].token == tSEC_UNIT) {

    gds->HaveRel++;
    gds->RelSeconds -= gds->tokenp[1].value * gds->tokenp[2].value;
    gds->tokenp += 3;
    return 1;
  }
  if (gds->tokenp[0].token == '+' && gds->tokenp[1].token == tUNUMBER &&
      gds->tokenp[2].token == tSEC_UNIT) {

    gds->HaveRel++;
    gds->RelSeconds += gds->tokenp[1].value * gds->tokenp[2].value;
    gds->tokenp += 3;
    return 1;
  }
  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == tSEC_UNIT) {

    gds->HaveRel++;
    gds->RelSeconds += gds->tokenp[0].value * gds->tokenp[1].value;
    gds->tokenp += 2;
    return 1;
  }
  if (gds->tokenp[0].token == '-' && gds->tokenp[1].token == tUNUMBER &&
      gds->tokenp[2].token == tMONTH_UNIT) {

    gds->HaveRel++;
    gds->RelMonth -= gds->tokenp[1].value * gds->tokenp[2].value;
    gds->tokenp += 3;
    return 1;
  }
  if (gds->tokenp[0].token == '+' && gds->tokenp[1].token == tUNUMBER &&
      gds->tokenp[2].token == tMONTH_UNIT) {

    gds->HaveRel++;
    gds->RelMonth += gds->tokenp[1].value * gds->tokenp[2].value;
    gds->tokenp += 3;
    return 1;
  }
  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == tMONTH_UNIT) {

    gds->HaveRel++;
    gds->RelMonth += gds->tokenp[0].value * gds->tokenp[1].value;
    gds->tokenp += 2;
    return 1;
  }
  if (gds->tokenp[0].token == tSEC_UNIT) {

    gds->HaveRel++;
    gds->RelSeconds += gds->tokenp[0].value;
    gds->tokenp += 1;
    return 1;
  }
  if (gds->tokenp[0].token == tMONTH_UNIT) {

    gds->HaveRel++;
    gds->RelMonth += gds->tokenp[0].value;
    gds->tokenp += 1;
    return 1;
  }
  return 0;
}

static int dayphrase(struct gdstate *gds) {
  if (gds->tokenp[0].token == tDAY) {

    gds->HaveWeekDay++;
    gds->DayOrdinal = 1;
    gds->DayNumber = gds->tokenp[0].value;
    gds->tokenp += 1;
    if (gds->tokenp[0].token == ',')
      gds->tokenp += 1;
    return 1;
  }
  if (gds->tokenp[0].token == tUNUMBER && gds->tokenp[1].token == tDAY) {

    gds->HaveWeekDay++;
    gds->DayOrdinal = gds->tokenp[0].value;
    gds->DayNumber = gds->tokenp[1].value;
    gds->tokenp += 2;
    return 1;
  }
  return 0;
}

static int phrase(struct gdstate *gds) {
  if (timephrase(gds))
    return 1;
  if (zonephrase(gds))
    return 1;
  if (datephrase(gds))
    return 1;
  if (dayphrase(gds))
    return 1;
  if (relunitphrase(gds)) {
    if (gds->tokenp[0].token == tAGO) {
      gds->RelSeconds = -gds->RelSeconds;
      gds->RelMonth = -gds->RelMonth;
      gds->tokenp += 1;
    }
    return 1;
  }

  if (gds->tokenp[0].token == tUNUMBER) {
    if (gds->HaveTime && !gds->HaveYear && !gds->HaveRel) {
      gds->HaveYear++;
      gds->Year = gds->tokenp[0].value;
      gds->tokenp += 1;
      return 1;
    }

    if (gds->tokenp[0].value > 10000) {

      gds->HaveYear++;
      gds->HaveMonth++;
      gds->HaveDay++;
      gds->Day = (gds->tokenp[0].value) % 100;
      gds->Month = (gds->tokenp[0].value / 100) % 100;
      gds->Year = gds->tokenp[0].value / 10000;
      gds->tokenp += 1;
      return 1;
    }

    if (gds->tokenp[0].value < 24) {
      gds->HaveTime++;
      gds->Hour = gds->tokenp[0].value;
      gds->Minutes = 0;
      gds->Seconds = 0;
      gds->tokenp += 1;
      return 1;
    }

    if ((gds->tokenp[0].value / 100 < 24) &&
        (gds->tokenp[0].value % 100 < 60)) {

      gds->Hour = gds->tokenp[0].value / 100;
      gds->Minutes = gds->tokenp[0].value % 100;
      gds->Seconds = 0;
      gds->tokenp += 1;
      return 1;
    }
  }

  return 0;
}

static struct LEXICON {
  size_t abbrev;
  const char *name;
  int type;
  time_t value;
} const TimeWords[] = {

    {0, "am", tAMPM, tAM},
    {0, "pm", tAMPM, tPM},

    {3, "january", tMONTH, 1},
    {3, "february", tMONTH, 2},
    {3, "march", tMONTH, 3},
    {3, "april", tMONTH, 4},
    {3, "may", tMONTH, 5},
    {3, "june", tMONTH, 6},
    {3, "july", tMONTH, 7},
    {3, "august", tMONTH, 8},
    {3, "september", tMONTH, 9},
    {3, "october", tMONTH, 10},
    {3, "november", tMONTH, 11},
    {3, "december", tMONTH, 12},

    {2, "sunday", tDAY, 0},
    {3, "monday", tDAY, 1},
    {2, "tuesday", tDAY, 2},
    {3, "wednesday", tDAY, 3},
    {2, "thursday", tDAY, 4},
    {2, "friday", tDAY, 5},
    {2, "saturday", tDAY, 6},

    {0, "gmt", tZONE, 0 * HOUR},
    {0, "ut", tZONE, 0 * HOUR},
    {0, "utc", tZONE, 0 * HOUR},
    {0, "wet", tZONE, 0 * HOUR},
    {0, "bst", tDAYZONE, 0 * HOUR},
    {0, "wat", tZONE, 1 * HOUR},
    {0, "at", tZONE, 2 * HOUR},

    {0, "nft", tZONE, 3 * HOUR + 30 * MINUTE},
    {0, "nst", tZONE, 3 * HOUR + 30 * MINUTE},
    {0, "ndt", tDAYZONE, 3 * HOUR + 30 * MINUTE},
    {0, "ast", tZONE, 4 * HOUR},
    {0, "adt", tDAYZONE, 4 * HOUR},
    {0, "est", tZONE, 5 * HOUR},
    {0, "edt", tDAYZONE, 5 * HOUR},
    {0, "cst", tZONE, 6 * HOUR},
    {0, "cdt", tDAYZONE, 6 * HOUR},
    {0, "mst", tZONE, 7 * HOUR},
    {0, "mdt", tDAYZONE, 7 * HOUR},
    {0, "pst", tZONE, 8 * HOUR},
    {0, "pdt", tDAYZONE, 8 * HOUR},
    {0, "yst", tZONE, 9 * HOUR},
    {0, "ydt", tDAYZONE, 9 * HOUR},
    {0, "hst", tZONE, 10 * HOUR},
    {0, "hdt", tDAYZONE, 10 * HOUR},
    {0, "cat", tZONE, 10 * HOUR},
    {0, "ahst", tZONE, 10 * HOUR},
    {0, "nt", tZONE, 11 * HOUR},
    {0, "idlw", tZONE, 12 * HOUR},
    {0, "cet", tZONE, -1 * HOUR},
    {0, "met", tZONE, -1 * HOUR},
    {0, "mewt", tZONE, -1 * HOUR},
    {0, "mest", tDAYZONE, -1 * HOUR},
    {0, "swt", tZONE, -1 * HOUR},
    {0, "sst", tDAYZONE, -1 * HOUR},
    {0, "fwt", tZONE, -1 * HOUR},
    {0, "fst", tDAYZONE, -1 * HOUR},
    {0, "eet", tZONE, -2 * HOUR},
    {0, "bt", tZONE, -3 * HOUR},
    {0, "it", tZONE, -3 * HOUR - 30 * MINUTE},
    {0, "zp4", tZONE, -4 * HOUR},
    {0, "zp5", tZONE, -5 * HOUR},
    {0, "ist", tZONE, -5 * HOUR - 30 * MINUTE},
    {0, "zp6", tZONE, -6 * HOUR},

    {0, "wast", tZONE, -7 * HOUR},
    {0, "wadt", tDAYZONE, -7 * HOUR},
    {0, "jt", tZONE, -7 * HOUR - 30 * MINUTE},
    {0, "cct", tZONE, -8 * HOUR},
    {0, "jst", tZONE, -9 * HOUR},
    {0, "cast", tZONE, -9 * HOUR - 30 * MINUTE},
    {0, "cadt", tDAYZONE, -9 * HOUR - 30 * MINUTE},
    {0, "east", tZONE, -10 * HOUR},
    {0, "eadt", tDAYZONE, -10 * HOUR},
    {0, "gst", tZONE, -10 * HOUR},
    {0, "nzt", tZONE, -12 * HOUR},
    {0, "nzst", tZONE, -12 * HOUR},
    {0, "nzdt", tDAYZONE, -12 * HOUR},
    {0, "idle", tZONE, -12 * HOUR},

    {0, "dst", tDST, 0},

    {4, "years", tMONTH_UNIT, 12},
    {5, "months", tMONTH_UNIT, 1},
    {9, "fortnights", tSEC_UNIT, 14 * DAY},
    {4, "weeks", tSEC_UNIT, 7 * DAY},
    {3, "days", tSEC_UNIT, DAY},
    {4, "hours", tSEC_UNIT, HOUR},
    {3, "minutes", tSEC_UNIT, MINUTE},
    {3, "seconds", tSEC_UNIT, 1},

    {0, "tomorrow", tSEC_UNIT, DAY},
    {0, "yesterday", tSEC_UNIT, -DAY},
    {0, "today", tSEC_UNIT, 0},
    {0, "now", tSEC_UNIT, 0},
    {0, "last", tUNUMBER, -1},
    {0, "this", tSEC_UNIT, 0},
    {0, "next", tUNUMBER, 2},
    {0, "first", tUNUMBER, 1},
    {0, "1st", tUNUMBER, 1},

    {0, "2nd", tUNUMBER, 2},
    {0, "third", tUNUMBER, 3},
    {0, "3rd", tUNUMBER, 3},
    {0, "fourth", tUNUMBER, 4},
    {0, "4th", tUNUMBER, 4},
    {0, "fifth", tUNUMBER, 5},
    {0, "5th", tUNUMBER, 5},
    {0, "sixth", tUNUMBER, 6},
    {0, "seventh", tUNUMBER, 7},
    {0, "eighth", tUNUMBER, 8},
    {0, "ninth", tUNUMBER, 9},
    {0, "tenth", tUNUMBER, 10},
    {0, "eleventh", tUNUMBER, 11},
    {0, "twelfth", tUNUMBER, 12},
    {0, "ago", tAGO, 1},

    {0, "a", tZONE, 1 * HOUR},
    {0, "b", tZONE, 2 * HOUR},
    {0, "c", tZONE, 3 * HOUR},
    {0, "d", tZONE, 4 * HOUR},
    {0, "e", tZONE, 5 * HOUR},
    {0, "f", tZONE, 6 * HOUR},
    {0, "g", tZONE, 7 * HOUR},
    {0, "h", tZONE, 8 * HOUR},
    {0, "i", tZONE, 9 * HOUR},
    {0, "k", tZONE, 10 * HOUR},
    {0, "l", tZONE, 11 * HOUR},
    {0, "m", tZONE, 12 * HOUR},
    {0, "n", tZONE, -1 * HOUR},
    {0, "o", tZONE, -2 * HOUR},
    {0, "p", tZONE, -3 * HOUR},
    {0, "q", tZONE, -4 * HOUR},
    {0, "r", tZONE, -5 * HOUR},
    {0, "s", tZONE, -6 * HOUR},
    {0, "t", tZONE, -7 * HOUR},
    {0, "u", tZONE, -8 * HOUR},
    {0, "v", tZONE, -9 * HOUR},
    {0, "w", tZONE, -10 * HOUR},
    {0, "x", tZONE, -11 * HOUR},
    {0, "y", tZONE, -12 * HOUR},
    {0, "z", tZONE, 0 * HOUR},

    {0, NULL, 0, 0}};

static time_t Convert(time_t Month, time_t Day, time_t Year, time_t Hours,
                      time_t Minutes, time_t Seconds, time_t Timezone,
                      enum DSTMODE DSTmode) {
  signed char DaysInMonth[12] = {31, 0, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  time_t Julian;
  int i;
  struct tm *ltime;
#if defined(HAVE_LOCALTIME_R) || defined(HAVE_LOCALTIME_S)
  struct tm tmbuf;
#endif

  if (Year < 69)
    Year += 2000;
  else if (Year < 100)
    Year += 1900;
  DaysInMonth[1] =
      Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0) ? 29 : 28;
  if (Year < EPOCH || (sizeof(time_t) <= 4 && Year >= 2038) || Month < 1 ||
      Month > 12

      || Day < 1 || Day > DaysInMonth[(int)--Month] || Hours < 0 ||
      Hours > 23 || Minutes < 0 || Minutes > 59 || Seconds < 0 || Seconds > 59)
    return -1;

  Julian = Day - 1;
  for (i = 0; i < Month; i++)
    Julian += DaysInMonth[i];
  for (i = EPOCH; i < Year; i++)
    Julian += 365 + (i % 4 == 0);
  Julian *= DAY;
  Julian += Timezone;
  Julian += Hours * HOUR + Minutes * MINUTE + Seconds;
#if defined(HAVE_LOCALTIME_S)
  ltime = localtime_s(&tmbuf, &Julian) ? NULL : &tmbuf;
#elif defined(HAVE_LOCALTIME_R)
  ltime = localtime_r(&Julian, &tmbuf);
#else
  ltime = localtime(&Julian);
#endif
  if (DSTmode == DSTon || (DSTmode == DSTmaybe && ltime->tm_isdst))
    Julian -= HOUR;
  return Julian;
}

static time_t DSTcorrect(time_t Start, time_t Future) {
  time_t StartDay;
  time_t FutureDay;
  struct tm *ltime;
#if defined(HAVE_LOCALTIME_R) || defined(HAVE_LOCALTIME_S)
  struct tm tmbuf;
#endif
#if defined(HAVE_LOCALTIME_S)
  ltime = localtime_s(&tmbuf, &Start) ? NULL : &tmbuf;
#elif defined(HAVE_LOCALTIME_R)
  ltime = localtime_r(&Start, &tmbuf);
#else
  ltime = localtime(&Start);
#endif
  StartDay = (ltime->tm_hour + 1) % 24;
#if defined(HAVE_LOCALTIME_S)
  ltime = localtime_s(&tmbuf, &Future) ? NULL : &tmbuf;
#elif defined(HAVE_LOCALTIME_R)
  ltime = localtime_r(&Future, &tmbuf);
#else
  ltime = localtime(&Future);
#endif
  FutureDay = (ltime->tm_hour + 1) % 24;
  return (Future - Start) + (StartDay - FutureDay) * HOUR;
}

static time_t RelativeDate(time_t Start, time_t zone, int dstmode,
                           time_t DayOrdinal, time_t DayNumber) {
  struct tm *tm;
  time_t t, now;
#if defined(HAVE_GMTIME_R) || defined(HAVE_GMTIME_S)
  struct tm tmbuf;
#endif

  t = Start - zone;
#if defined(HAVE_GMTIME_S)
  tm = gmtime_s(&tmbuf, &t) ? NULL : &tmbuf;
#elif defined(HAVE_GMTIME_R)
  tm = gmtime_r(&t, &tmbuf);
#else
  tm = gmtime(&t);
#endif
  now = Start;
  now += DAY * ((DayNumber - tm->tm_wday + 7) % 7);
  now += 7 * DAY * (DayOrdinal <= 0 ? DayOrdinal : DayOrdinal - 1);
  if (dstmode == DSTmaybe)
    return DSTcorrect(Start, now);
  return now - Start;
}

static time_t RelativeMonth(time_t Start, time_t Timezone, time_t RelMonth) {
  struct tm *tm;
  time_t Month;
  time_t Year;
#if defined(HAVE_LOCALTIME_R) || defined(HAVE_LOCALTIME_S)
  struct tm tmbuf;
#endif

  if (RelMonth == 0)
    return 0;
#if defined(HAVE_LOCALTIME_S)
  tm = localtime_s(&tmbuf, &Start) ? NULL : &tmbuf;
#elif defined(HAVE_LOCALTIME_R)
  tm = localtime_r(&Start, &tmbuf);
#else
  tm = localtime(&Start);
#endif
  Month = 12 * (tm->tm_year + 1900) + tm->tm_mon + RelMonth;
  Year = Month / 12;
  Month = Month % 12 + 1;
  return DSTcorrect(Start, Convert(Month, (time_t)tm->tm_mday, Year,
                                   (time_t)tm->tm_hour, (time_t)tm->tm_min,
                                   (time_t)tm->tm_sec, Timezone, DSTmaybe));
}

static char consume_unsigned_number(const char **in, time_t *value) {
  char c;
  if (isdigit((unsigned char)(c = **in))) {
    for (*value = 0; isdigit((unsigned char)(c = *(*in)++));)
      *value = 10 * *value + c - '0';
    (*in)--;
    return 1;
  }
  return 0;
}

static int nexttoken(const char **in, time_t *value) {
  char c;
  char buff[64];

  for (;;) {
    while (isspace((unsigned char)**in))
      ++*in;

    if (**in == '(') {
      int Count = 0;
      do {
        c = *(*in)++;
        if (c == '\0')
          return c;
        if (c == '(')
          Count++;
        else if (c == ')')
          Count--;
      } while (Count > 0);
      continue;
    }

    {
      const char *src = *in;
      const struct LEXICON *tp;
      unsigned i = 0;

      while (*src != '\0' && (isalnum((unsigned char)*src) || *src == '.') &&
             i < sizeof(buff) - 1) {
        if (*src != '.') {
          if (isupper((unsigned char)*src))
            buff[i++] = (char)tolower((unsigned char)*src);
          else
            buff[i++] = *src;
        }
        src++;
      }
      buff[i] = '\0';

      for (tp = TimeWords; tp->name; tp++) {
        size_t abbrev = tp->abbrev;
        if (abbrev == 0)
          abbrev = strlen(tp->name);
        if (strlen(buff) >= abbrev &&
            strncmp(tp->name, buff, strlen(buff)) == 0) {

          *in = src;

          *value = tp->value;
          return tp->type;
        }
      }
    }

    if (consume_unsigned_number(in, value)) {
      return (tUNUMBER);
    }

    return *(*in)++;
  }
}

#define TM_YEAR_ORIGIN 1900

static long difftm(struct tm *a, struct tm *b) {
  int ay = a->tm_year + (TM_YEAR_ORIGIN - 1);
  int by = b->tm_year + (TM_YEAR_ORIGIN - 1);
  long days = (

      a->tm_yday - b->tm_yday

      + ((ay >> 2) - (by >> 2)) - (ay / 100 - by / 100) +
      ((ay / 100 >> 2) - (by / 100 >> 2))

      + (long)(ay - by) * 365);
  return (days * DAY + (a->tm_hour - b->tm_hour) * HOUR +
          (a->tm_min - b->tm_min) * MINUTE + (a->tm_sec - b->tm_sec));
}

static time_t parse_unix_epoch(const char *p) {
  time_t epoch;

  if (*p == '+') {
    p++;
  }

  if (!consume_unsigned_number(&p, &epoch))
    return (time_t)-1;

  if (*p != '\0')
    return (time_t)-1;

  return epoch;
}

time_t archive_parse_date(time_t now, const char *p) {
  struct token tokens[256];
  struct gdstate _gds;
  struct token *lasttoken;
  struct gdstate *gds;
  struct tm local, *tm;
  struct tm gmt, *gmt_ptr;
  time_t Start;
  time_t tod;
  long tzone;

  if (*p == '@')
    return parse_unix_epoch(p + 1);

  memset(tokens, 0, sizeof(tokens));

  memset(&_gds, 0, sizeof(_gds));
  gds = &_gds;

#if defined(HAVE_LOCALTIME_S)
  tm = localtime_s(&local, &now) ? NULL : &local;
#elif defined(HAVE_LOCALTIME_R)
  tm = localtime_r(&now, &local);
#else
  memset(&local, 0, sizeof(local));
  tm = localtime(&now);
#endif
  if (tm == NULL)
    return -1;
#if !defined(HAVE_LOCALTIME_R) && !defined(HAVE_LOCALTIME_S)
  local = *tm;
#endif

#if defined(HAVE_GMTIME_S)
  gmt_ptr = gmtime_s(&gmt, &now) ? NULL : &gmt;
#elif defined(HAVE_GMTIME_R)
  gmt_ptr = gmtime_r(&now, &gmt);
#else
  memset(&gmt, 0, sizeof(gmt));
  gmt_ptr = gmtime(&now);
  if (gmt_ptr != NULL) {

    gmt = *gmt_ptr;
  }
#endif
  if (gmt_ptr != NULL)
    tzone = difftm(&gmt, &local);
  else

    tzone = 0;
  if (local.tm_isdst)
    tzone += HOUR;

  lasttoken = tokens;
  while ((lasttoken->token = nexttoken(&p, &lasttoken->value)) != 0) {
    ++lasttoken;
    if (lasttoken > tokens + 255)
      return -1;
  }
  gds->tokenp = tokens;

  while (gds->tokenp < lasttoken) {
    if (!phrase(gds))
      return -1;
  }

  if (!gds->HaveZone) {
    gds->Timezone = tzone;
    gds->DSTmode = DSTmaybe;
  }

  if (gds->HaveZone && gmt_ptr != NULL) {
    now -= gds->Timezone;
#if defined(HAVE_GMTIME_S)
    gmt_ptr = gmtime_s(&gmt, &now) ? NULL : &gmt;
#elif defined(HAVE_GMTIME_R)
    gmt_ptr = gmtime_r(&now, &gmt);
#else
    gmt_ptr = gmtime(&now);
#endif
    if (gmt_ptr != NULL)
      local = *gmt_ptr;
    now += gds->Timezone;
  }

  if (!gds->HaveYear)
    gds->Year = local.tm_year + 1900;
  if (!gds->HaveMonth)
    gds->Month = local.tm_mon + 1;
  if (!gds->HaveDay)
    gds->Day = local.tm_mday;

  if (gds->HaveTime > 1 || gds->HaveZone > 1 || gds->HaveWeekDay > 1 ||
      gds->HaveYear > 1 || gds->HaveMonth > 1 || gds->HaveDay > 1)
    return -1;

  if (gds->HaveYear || gds->HaveMonth || gds->HaveDay || gds->HaveTime ||
      gds->HaveWeekDay) {
    Start = Convert(gds->Month, gds->Day, gds->Year, gds->Hour, gds->Minutes,
                    gds->Seconds, gds->Timezone, gds->DSTmode);
    if (Start < 0)
      return -1;
  } else {
    Start = now;
    if (!gds->HaveRel)
      Start -= local.tm_hour * HOUR + local.tm_min * MINUTE + local.tm_sec;
  }

  Start += gds->RelSeconds;
  Start += RelativeMonth(Start, gds->Timezone, gds->RelMonth);

  if (gds->HaveWeekDay && !(gds->HaveYear || gds->HaveMonth || gds->HaveDay)) {
    tod = RelativeDate(Start, gds->Timezone, gds->DSTmode, gds->DayOrdinal,
                       gds->DayNumber);
    Start += tod;
  }

  return Start == -1 ? 0 : Start;
}

#if defined(TEST)

int main(int argc, char **argv) {
  time_t d;
  time_t now = time(NULL);

  while (*++argv != NULL) {
    (void)printf("Input: %s\n", *argv);
    d = get_date(now, *argv);
    if (d == -1)
      (void)printf("Bad format - couldn't convert.\n");
    else
      (void)printf("Output: %s\n", ctime(&d));
  }
  exit(0);
}
#endif
