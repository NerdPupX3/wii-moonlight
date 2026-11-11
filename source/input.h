#ifndef INPUT_H
#define INPUT_H

#include <gccore.h>

#define INPUT_NONE 0
#define INPUT_QUIT 1    /* + and - held together: leave the stream */

// Call once after LiStartConnection() succeeds. screenW/screenH is the size of the
// Wii's display, used to map the IR pointer to the streamed desktop.
void input_start(int screenW, int screenH);

// Call every frame from the stream loop (after WPAD_ScanPads()). Sends the Wiimote
// and Nunchuk state to the host. Returns INPUT_QUIT when + and - are pressed together.
int input_poll(int streaming);

#endif
