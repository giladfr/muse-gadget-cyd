// Forced in with -include: the dashboard's time() calls read a fixed fake
// clock (Tue 2026-10-06 10:42 CDT, advancing in real time) so previews are
// reproducible.
#pragma once
#include <time.h>
time_t fake_time(time_t *t);
#define time(t) fake_time(t)
