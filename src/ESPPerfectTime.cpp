#include <Arduino.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <lwip/apps/sntp.h>
#ifdef ESP8266
#include <sntp-lwip2.h>
#endif // ESP8266
#ifdef ESP32
#include <esp32-hal.h>
#endif // ESP32
#include "sntp.h"
#include "ESPPerfectTime.h"

#define EPOCH_YEAR                     1970
#define TIME_2020_01_01_00_00_00_UTC   1577836800
#define SECS_PER_MIN                   60
#define SECS_PER_HOUR                  3600
#define SECS_PER_DAY                   86400

#define IS_LEAP_YEAR(y) ((((y) % 4) == 0 && ((y) % 100) != 0) || ((y) % 400) == 0)
#define TM_YEAR(m)      ((m) - 1900)
#define TM_MONTH(m)     ((m) - 1)

static const int _ydays[2][13] = {
  {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
  {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}
};

static uint8_t _leap_indicator = LI_NO_WARNING;
// The second of the end of the month -- 23:59:59 in UTC
static time_t  _leap_time      = 0;
static struct tm _tm_result;

static void adjustLeapSec(time_t *t) {
  if (_leap_indicator == LI_LAST_MINUTE_61_SEC && *t > _leap_time)
    (*t)--;
  else if (_leap_indicator == LI_LAST_MINUTE_59_SEC && *t >= _leap_time)
    (*t)++;
}

static time_t mkgmtime(tm *tm) {
  size_t isLeapYear = IS_LEAP_YEAR(tm->tm_year) ? 1 : 0;
  tm->tm_yday = _ydays[isLeapYear][tm->tm_mon] + tm->tm_mday - 1;

  int epoch_year;
  time_t result;
  if (tm->tm_year < TM_YEAR(2020)) {
    epoch_year = EPOCH_YEAR;
    result = 0;
  } else {
    epoch_year = 2020;
    result = TIME_2020_01_01_00_00_00_UTC;
  }

  int days = 0;
  for (int y = epoch_year; y < 1900 + tm->tm_year; y++) {
    days += IS_LEAP_YEAR(y) ? 366 : 365;
  }
  days += tm->tm_yday;

  result += days       * SECS_PER_DAY
         + tm->tm_hour * SECS_PER_HOUR
         + tm->tm_min  * SECS_PER_MIN
         + tm->tm_sec;

  return result;
}

static time_t calcNextLeap(const time_t current) {
  struct tm now = *::gmtime(&current);
  struct tm leaping = {0};
  if (now.tm_mon == TM_MONTH(12)){
    leaping.tm_year = now.tm_year + 1;
    leaping.tm_mon  = TM_MONTH(1);
  } else {
    leaping.tm_year = now.tm_year;
    leaping.tm_mon  = now.tm_mon + 1;
  }
  leaping.tm_mday = 1;

  return mkgmtime(&leaping) - 1;
}

time_t pftime::time(time_t *timer) {
#ifdef ESP8266
  /* time() implemented in ESP8266 core is incorrect */
  /* so we use gettimeofday() instead of time() */
  /* see: https://github.com/esp8266/Arduino/issues/4637 */
  struct timeval tv;
  pftime::gettimeofday(&tv, nullptr);
  return tv.tv_sec;
#else
  /* ESP32's time() is correct */
  time_t t = ::time(nullptr);
  adjustLeapSec(&t);
  return t;
#endif
}

#define DEF_FOOTIME(name)                                                          \
  struct tm *pftime::name(const time_t *timer, suseconds_t *res_usec) {            \
    if (timer)                                                                     \
      return ::name(timer);                                                        \
                                                                                   \
    struct timeval tv;                                                             \
    ::gettimeofday(&tv, nullptr);                                                  \
                                                                                   \
    if (res_usec)                                                                  \
      *res_usec = tv.tv_usec;                                                      \
                                                                                   \
    if (_leap_indicator == LI_LAST_MINUTE_61_SEC && tv.tv_sec == _leap_time + 1) { \
      _tm_result = *::name(&_leap_time);                                           \
      _tm_result.tm_sec = 60;                                                      \
      return &_tm_result;                                                          \
    }                                                                              \
                                                                                   \
    adjustLeapSec(&tv.tv_sec);                                                     \
    return ::name(&tv.tv_sec);                                                     \
  }

DEF_FOOTIME(gmtime);
DEF_FOOTIME(localtime);

int pftime::gettimeofday(struct timeval *tv, struct timezone *tz) {
  if (tv) {
    ::gettimeofday(tv, nullptr);
    adjustLeapSec(&tv->tv_sec);
  }
  return 0;
}

int pftime::settimeofday(const struct timeval *tv, const struct timezone *tz, uint8_t li) {
  if (tv) {
    int result = ::settimeofday(tv, tz);
    _leap_indicator = li;
    if (li != LI_NO_WARNING) {
      _leap_time = calcNextLeap(tv->tv_sec);
      //Serial.printf("Leap second will insert/delete after %d\n", _leap_time);
    }
    return result;
  }
}

// from esp32-hal-time.c
static void setTimeZone(long offset, int daylight)
{
    char cst[17] = {0};
    char cdt[17] = "DST";
    char tz[33] = {0};

    if(offset % 3600){
        sprintf(cst, "UTC%ld:%02u:%02u", offset / 3600, abs((offset % 3600) / 60), abs(offset % 60));
    } else {
        sprintf(cst, "UTC%ld", offset / 3600);
    }
    if(daylight != 3600){
        long tz_dst = offset - daylight;
        if(tz_dst % 3600){
            sprintf(cdt, "DST%ld:%02u:%02u", tz_dst / 3600, abs((tz_dst % 3600) / 60), abs(tz_dst % 60));
        } else {
            sprintf(cdt, "DST%ld", tz_dst / 3600);
        }
    }
    sprintf(tz, "%s%s", cst, cdt);
    setenv("TZ", tz, 1);
#ifdef ESP8266
    sntp_set_timezone_in_seconds(0);
#endif
    tzset();
}

/*
 * configTime
 * Source: https://github.com/esp8266/Arduino/blob/master/cores/esp8266/time.c
 * */
void pftime::configTime(long gmtOffset_sec, int daylightOffset_sec, const char* server1, const char* server2, const char* server3)
{
  if(sntp_enabled()) {
    sntp_stop();
  }
  pftime_sntp::stop();

  //pftime_sntp::setoperatingmode(SNTP_OPMODE_POLL);
  pftime_sntp::setservername(0, (char*)server1);
  pftime_sntp::setservername(1, (char*)server2);
  pftime_sntp::setservername(2, (char*)server3);

  setTimeZone(-gmtOffset_sec, daylightOffset_sec);

  pftime_sntp::init();
}


#ifdef ESP8266
void pftime::configTime(const char *tz,
                        const char *server1, const char *server2, const char *server3) {
  sntp_stop();
  pftime_sntp::stop();

  pftime_sntp::setservername(0, (char*)server1);
  pftime_sntp::setservername(1, (char*)server2);
  pftime_sntp::setservername(2, (char*)server3);

  char tzram[strlen_P(tz) + 1];
  memcpy_P(tzram, tz, sizeof(tzram));
  setenv("TZ", tzram, 1/*overwrite*/);
  sntp_set_timezone_in_seconds(0);
  tzset();

  pftime_sntp::init();
}
#endif

#ifdef ESP32
void pftime::configTzTime(const char* tz, const char* server1, const char* server2, const char* server3)
{
  if(sntp_enabled()) {
      sntp_stop();
  }
  pftime_sntp::stop();

  //sntp_setoperatingmode(SNTP_OPMODE_POLL);
  pftime_sntp::setservername(0, (char*)server1);
  pftime_sntp::setservername(1, (char*)server2);
  pftime_sntp::setservername(2, (char*)server3);
  pftime_sntp::init();
  setenv("TZ", tz, 1);
  tzset();
}
#endif