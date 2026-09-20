#ifndef __LEDS_H__
#define __LEDS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

int leds_init(void);
int leds_ctl(int which, bool on);

#ifdef __cplusplus
}
#endif

#endif /* __LEDS_H__ */