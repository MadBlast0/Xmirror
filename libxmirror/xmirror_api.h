// xmirror_api.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int start_xmirror(int argc, char *argv[]);
void stop_xmirror();

/* Reports the mirrored picture's size. Called on the engine's mirroring thread
 * whenever the client announces its video format: at the start of a session and
 * again whenever it changes -- most commonly when the device rotates between
 * portrait and landscape. width/height are the encoded picture; source_width and
 * source_height are the client's own screen. Pass NULL to stop reporting. */
typedef void (*xmirror_video_size_callback)(int width, int height,
                                            int source_width, int source_height);
void xmirror_set_video_size_callback(xmirror_video_size_callback callback);

/* Why the last start_xmirror() call returned non-zero, in plain language, or an
 * empty string. Valid until the next start_xmirror() call on the same thread. */
const char *xmirror_last_error(void);

/* Something the user should know that did not stop the engine -- for example
 * that hardware video decoding failed and software decoding took over. Called
 * on the engine's thread. Pass NULL to stop reporting. */
typedef void (*xmirror_notice_callback)(const char *message);
void xmirror_set_notice_callback(xmirror_notice_callback callback);

#ifdef __cplusplus
}
#endif
