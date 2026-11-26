#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Hold IR TX low (if enabled) and start the onboard button poll task. */
void board_hw_init(void);

#ifdef __cplusplus
}
#endif
