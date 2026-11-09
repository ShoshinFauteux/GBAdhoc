/* ------------------------------------------------------------------- main */

/* The PSP's local wall clock as seconds since 1970, with no timezone applied
 * (the core decodes it with gmtime).  Days from civil date: H. Hinnant's
 * algorithm, valid for any proleptic Gregorian date. */
static long long g_clock_fake;      /* harness psp_clock: this console's
                                     * clock reads this at boot (seconds
                                     * since 1970), then runs; 0 = real */
static long long g_clock_fake_tz;   /* harness psp_clock_tz: with psp_clock,
                                     * the seconds that clock is ahead of
                                     * UTC (the console's zone); default 0 */
static time_t psp_datetime_seconds(const ScePspDateTime *dt)
{
   long long y, m, era, yoe, doy, doe, days;
   y   = (long long)dt->year - (dt->month <= 2);
   m   = (long long)dt->month;
   era = (y >= 0 ? y : y - 399) / 400;
   yoe = y - era * 400;
   doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (long long)dt->day - 1;
   doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   days = era * 146097 + doe - 719468;
   return (time_t)(days * 86400 + (long long)dt->hour * 3600 +
                   (long long)dt->minute * 60 + (long long)dt->second);
}

static time_t psp_local_wallclock(void)
{
   ScePspDateTime dt;
   if (g_clock_fake)
      return (time_t)(g_clock_fake +
                      (long long)(sceKernelGetSystemTimeWide() / 1000000ull));
   if (sceRtcGetCurrentClockLocalTime(&dt) < 0)
      return (time_t)0;             /* no reading: the clocks hold */
   return psp_datetime_seconds(&dt);
}

/* The same clock as UTC: what the console's clock reads with its time zone
 * and daylight-saving setting taken off.  The GB/GBC cartridge clock runs
 * on this one, because its save stamps the time in UTC (fe_host.c); it
 * counts elapsed seconds only, so the game shows the same time either way. */
static time_t psp_utc_wallclock(void)
{
   ScePspDateTime dt;
   if (g_clock_fake)
      return (time_t)(g_clock_fake - g_clock_fake_tz +
                      (long long)(sceKernelGetSystemTimeWide() / 1000000ull));
   if (sceRtcGetCurrentClock(&dt, 0) < 0)
      return (time_t)0;
   return psp_datetime_seconds(&dt);
}

