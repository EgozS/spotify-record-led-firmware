#pragma once

// Whole days elapsed since `since` ("YYYY-MM-DD"), using the local date at `tz_min`
// minutes east of UTC. Returns -1 while the clock has not been set (no internet time yet)
// or when the date text is invalid. A future date counts as 0.
int counter_days(const char *since, int tz_min);
