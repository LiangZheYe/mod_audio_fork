#ifndef __LWS_GLUE_H__
#define __LWS_GLUE_H__

#include "mod_audio_inject.h"

int parse_ws_uri(switch_channel_t *channel, const char *szServerUri, char *host,
                 char *path, unsigned int *pPort, int *pSslFlags);

switch_status_t inject_init();
switch_status_t inject_cleanup();
switch_status_t inject_session_init(
    switch_core_session_t *session, responseHandler_t responseHandler,
    uint32_t samples_per_second, char *host, unsigned int port, char *path,
    int sampling, int sslFlags, int channels, char *bugname,
    int bidirectional_audio_enable, int bidirectional_audio_stream,
    int bidirectional_audio_sample_rate, void **ppUserData);
switch_status_t inject_session_cleanup(switch_core_session_t *session,
                                     char *bugname, int channelIsClosing);
switch_status_t inject_session_stop_play(switch_core_session_t *session,
                                       char *bugname);
switch_status_t inject_session_pauseresume(switch_core_session_t *session,
                                         char *bugname, int pause);
switch_status_t inject_session_graceful_shutdown(switch_core_session_t *session,
                                               char *bugname);
switch_bool_t inject_frame(switch_core_session_t *session,
                         switch_media_bug_t *bug);
switch_bool_t dub_speech_frame(switch_media_bug_t *bug, private_t *tech_pvt);
switch_status_t inject_service_threads();
switch_status_t inject_session_connect(void **ppUserData);
#endif
