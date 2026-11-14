#ifndef _SYS_VENUS_LMD_RADIO_H_
#define _SYS_VENUS_LMD_RADIO_H_
#include <stdbool.h>
/* RPMh regulator vote for the venus_lmd Wi-Fi bring-up; votes persist until reboot. */
bool venus_lmd_radio_supply_on(const char *name, unsigned millivolts);
#endif
