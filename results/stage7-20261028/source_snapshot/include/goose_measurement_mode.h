#ifndef FEEDGUARD_GOOSE_MEASUREMENT_MODE_H
#define FEEDGUARD_GOOSE_MEASUREMENT_MODE_H
#include <stdint.h>
/* Included in generated Goose C, not the separately cached native helper. */
static inline int32_t fg_goose_measurement_release(void) {
#if defined(__OPTIMIZE__) && __OPTIMIZE__ > 0 && (!defined(GS_DEBUG) || !GS_DEBUG)
    return 1;
#else
    return 0;
#endif
}
#endif
