#include <features.h>

#if defined(__linux__) && defined(__GLIBC__) && defined(__x86_64__)

extern double isac_sinh_glibc_compat(double);
extern double isac_cosh_glibc_compat(double);

__asm__(".symver isac_sinh_glibc_compat,sinh@GLIBC_2.2.5");

__asm__(".symver isac_cosh_glibc_compat,cosh@GLIBC_2.2.5");

double __wrap_sinh(double x) {
  return isac_sinh_glibc_compat(x);
}

double __wrap_cosh(double x) {
  return isac_cosh_glibc_compat(x);
}

#else

#error "glibc_compat.c requires Linux x86_64 with glibc"

#endif