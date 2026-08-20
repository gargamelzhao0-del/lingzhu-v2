/*
 * Board profile identity + startup banner (P1-1).
 * Prints the active board profile and key GPIO assignments at boot so bring-up
 * logs unambiguously record which hardware map the firmware was built for.
 */
#ifndef NUWAX_BOARD_H
#define NUWAX_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable name of the board profile selected via Kconfig. */
const char *nuwax_board_name(void);

/* Logs the board profile and the key shared GPIO assignments (I2C, display,
 * touch, audio-final, power). Call once early in app_main. */
void nuwax_board_log_banner(void);

#ifdef __cplusplus
}
#endif

#endif /* NUWAX_BOARD_H */
