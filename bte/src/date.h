#pragma once
#include <utility>
#include <time.h>
#include <stdint.h>

// returns next correct date (properly accounting for leap years)
int nextDate(int date); // date is an int in form YYYYMMDD

// returns previous correct date (properly accounting for leap years)
int previousDate(int date); // date is an int in form YYYYMMDD

// Similar to nextDate, except it skips Saturdays and Sundays. Note: it doesn't check for holidays when markets are closed
int nextTradableDate(int date); // date is an int in form YYYYMMDD

// reverse of nextTradableDate. Note: it doesn't check for holidays when markets are closed
int previousTradableDate(int date); // date is an int in form YYYYMMDD

// return YYYYMM01 for any YYYYMMDD date
inline int monthDate(int date)
{
    return date / 100 * 100 + 1;
}

inline int nextMonthDate(int date)
{
    int month = date % 10000 / 100, year = date / 10000;
    return month < 12 ? (year * 10000 + (month + 1) * 100 + 1) : ((year + 1) * 10000 + 101);
}

inline int previousMonthDate(int date)
{
    int month = date % 10000 / 100, year = date / 10000;
    return month > 1 ? (year * 10000 + (month - 1) * 100 + 1) : ((year - 1) * 10000 + 1201);
}

// return [0..6] for [Sunday-Saturday]
int dayOfWeek(int year, int month, int day); // 1 <= month <= 12
int dayOfWeek(int date);

// check if date was a holiday, e.g. market was closed. https://www.nyse.com/markets/hours-calendars (date must be from 2004)
bool isDayOff(int date);

// Check if a date is tradable, e.g. was a regular week day. It might still be a holiday, e.g. isDayOff(date) might be true
inline bool isTradableDate(int date)
{
    int dow = dayOfWeek(date);
    return dow > 0 && dow < 6;
}

// get YYYYMMDD date of last tradable day for the requested time.
// If completedDay is true, then the time must be 9pm or later for the day to be completed, otherwise time must be 4am+
int lastTradableDayFromTime(time_t t, bool completedDay);

// Get last finished trading date, assuming market data isn't complete before 9pm NY time (after market closes 8pm). Note: it doesn't check for holidays when markets are closed
int getLastTradableDateCompleted();

// Similar to getLastTradableDateCompleted, but will also include last unfinished day (e.g. today's date if it's tradable).
int getLastTradableDate();

// returns pair for date/time (NY timezone), where date is an int in form YYYYMMDD, time is ms from day start (NY timezone)
std::pair<int, int> getDateTimeFromUtcMs(int64_t millisecondsTime);

// similar to localtime_r, except it converts from UTC to NY timezone properly accounting for different rules that were used in the past for DST shifts
tm* nytime_r(const time_t* t, tm* tm);
tm* nytime2_r(const time_t* t, tm* tm); // Works on dates from 1918-03-31 (first time DST was used) to 2037-11-01. Slower than nytime_r by 35%

// similar to mktime, except it assumes that nyt is in NY timezone
time_t mktimeNy(struct tm* nyt);

// similar to mktime, but assumes UTC instead of local timezone. mkgmtime is inverse of gmtime
time_t mkgmtime(struct tm* tm);
time_t timeutc(const struct tm* utc);
time_t timegm_chrono(struct tm* tm); // timegm implemented using std::chrono (tm gets normalized)

// check fields of struct tm and recalculate where necessary to ensure these fields are in their expected ranges.
// tm->tm_wday and tm->tm_yday are recalculated using information available in other field
void normalize_tm(struct tm* tm);

// number of days from unix epoch
int daysFromEpoch(int y, int m, int d); // 1 <= m <= 12
int daysFromEpoch(int date);

time_t getEodTimeForDate(int date); // return 23:59:00 timestamp for the requested date

struct tm* gmtime_x(const time_t* t, struct tm* tp); // equivalent to gmtime_r, but works on any timestamp (also faster by 50%)
struct tm* gmtime_r_chrono(const time_t* t, struct tm* tp); // equivalent to gmtime_r, implemented using std::chrono

#ifdef _WIN32
static inline tm* localtime_r(const time_t* t, tm* tmp)
{
    localtime_s(tmp, t);
    return tmp;
}

static inline tm* gmtime_r(const time_t* t, tm* tmp)
{
    gmtime_s(tmp, t);
    return tmp;
}
#endif
