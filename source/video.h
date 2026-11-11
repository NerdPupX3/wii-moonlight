#ifndef VIDEO_H
#define VIDEO_H

#include <gccore.h>
#include "Limelight.h"

/* Fill cb with the Wii's video decoder renderer callbacks. */
void video_init_callbacks(PDECODER_RENDERER_CALLBACKS cb);

/* UI thread: pull the next decoded RGB565 frame. Returns 1 and fills the out
 * pointers if a frame is ready, else 0. w/h are the visible video size;
 * tex_w/tex_h are the padded power-of-two texture dimensions. The buffer is
 * owned by the decoder and valid until the next video_frame_get() call. */
void video_note_display(u64 ticks);
int video_frame_get(u16 **rgb_out, int *w_out, int *h_out, int *tex_w_out, int *tex_h_out);

#endif /* VIDEO_H */
