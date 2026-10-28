#ifndef FEEDGUARD_MEASUREMENT_H
#define FEEDGUARD_MEASUREMENT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t fg_clock_ns(void);
uint64_t fg_clock_frequency(void);
uint64_t fg_peak_working_set_bytes(void);
#ifdef __cplusplus
}
#endif
#endif
