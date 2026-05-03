#ifndef SANDFLEA_TIME_H
#define SANDFLEA_TIME_H

#include "dialect.h"

typedef i64 time_t;
typedef i64 clock_t;

#define CLOCKS_PER_SEC 1000

clock_t clock();

#endif
