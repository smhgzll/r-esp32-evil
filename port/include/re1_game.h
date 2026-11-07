// re1_game.h - entry points of the shared game loop (port/common/GameMain.cpp).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Boot the game (config, sound, scheduler, renderer). 1 on success.
int  re1_game_init(void);
// One loop iteration; 0 = the game asked to exit. *ranTick = 1 when a 33 ms
// game tick actually ran (the frame limiter skips early calls).
int  re1_game_step(int* ranTick);
void re1_game_shutdown(void);
// init + step until exit + shutdown.
void re1_game_run(void);

#ifdef __cplusplus
}
#endif
