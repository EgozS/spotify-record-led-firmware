#include "counter.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

int counter_days(const char *since, int tz_min) {
    time_t now = time(nullptr);
    if (now < 1704067200) return -1;  // before 2024-01-01: clock not synced yet

    if (strlen(since) != 10 || since[4] != '-' || since[7] != '-') return -1;
    int y = atoi(since), m = atoi(since + 5), d = atoi(since + 8);
    if (y < 1900 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) return -1;

    long since_days = days_from_civil(y, (unsigned)m, (unsigned)d);
    long local_secs = (long)now + (long)tz_min * 60;
    long today = local_secs >= 0 ? local_secs / 86400 : -((-local_secs + 86399) / 86400);
    long diff = today - since_days;
    return diff < 0 ? 0 : (int)diff;
}
