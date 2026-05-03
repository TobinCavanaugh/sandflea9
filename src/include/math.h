#ifndef SANDFLEA_MATH_H
#define SANDFLEA_MATH_H

#include "dialect.h"

#define NAN  f32_NaN
#define NANF f32_NaN

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define signbit(x) __builtin_signbit(x)

double sqrt(double x);
float sqrtf(float x);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double fabs(double x);
float fabsf(float x);
double trunc(double x);
float truncf(float x);
double rint(double x);
float rintf(float x);
double copysign(double x, double y);
float copysignf(float x, float y);

#endif
