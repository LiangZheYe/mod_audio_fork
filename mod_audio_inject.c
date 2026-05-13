/*
 *
 * mod_audio_inject.c -- Freeswitch module for injecting audio to remote server over
 * websockets
 *
 */
#include "mod_audio_inject.h"

#include "lws_glue.h"

// static int mod_running = 0;

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_inject_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_audio_inject_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_audio_inject_load);

SWITCH_MODULE_DEFINITION(mod_audio_inject, mod_audio_inject_load,
                         mod_audio_inject_shutdown,
                         NULL /*mod_audio_inject_runtime*/);

static void responseHandler(switch_core_session_t *session,
                            const char *eventName, char *json) {
  switch_event_t *event;

  switch_channel_t *channel = switch_core_session_get_channel(session);
  if (json)
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                      "responseHandler: sending event payload: %s.\n", json);
  switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
  switch_channel_event_set_data(channel, event);
  if (json) switch_event_add_body(event, "%s", json);
  switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data,
                                      switch_abc_type_t type) {
  (void)user_data;
  switch_bool_t ret = SWITCH_TRUE;
  switch_core_session_t *session = switch_core_media_bug_get_session(bug);
  private_t *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
  switch (type) {
    case SWITCH_ABC_TYPE_INIT:
      break;

    case SWITCH_ABC_TYPE_CLOSE: {
      /* BUG-22 fix: check tech_pvt before dereferencing */
      if (!tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Got SWITCH_ABC_TYPE_CLOSE but tech_pvt is NULL\n");
        break;
      }
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                        "Got SWITCH_ABC_TYPE_CLOSE for bug %s\n",
                        tech_pvt->bugname);
      inject_session_cleanup(session, tech_pvt->bugname, 1);
    } break;

    case SWITCH_ABC_TYPE_READ:
      ret = inject_frame(session, bug);
      break;

    case SWITCH_ABC_TYPE_WRITE_REPLACE:
      ret = dub_speech_frame(bug, tech_pvt);
      break;

    case SWITCH_ABC_TYPE_WRITE:
    default:
      break;
  }

  return ret;
}

static switch_status_t start_capture(
    switch_core_session_t *session, switch_media_bug_flag_t flags, char *host,
    unsigned int port, char *path, int sampling, int sslFlags,
    int bidirectional_audio_enable, int bidirectional_audio_stream,
    int bidirectional_audio_sample_rate, char *bugname) {
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug;
  switch_status_t status;
  switch_codec_t *read_codec;

  void *pUserData = NULL;
  int channels = (flags & SMBF_STEREO) ? 2 : 1;

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                    "mod_audio_inject (%s): streaming %d sampling to %s path %s "
                    "port %d tls: %s bidirectional_audio_sample_rate: %d.\n",
                    bugname, sampling, host, path, port,
                    sslFlags ? "yes" : "no", bidirectional_audio_sample_rate);

  if (switch_channel_get_private(channel, bugname)) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                      "mod_audio_inject: bug %s already attached!\n", bugname);
    return SWITCH_STATUS_FALSE;
  }

  read_codec = switch_core_session_get_read_codec(session);

  if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                      "mod_audio_inject: channel must have reached pre-answer "
                      "status before calling start!\n");
    return SWITCH_STATUS_FALSE;
  }

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                    "calling inject_session_init.\n");
  if (SWITCH_STATUS_FALSE ==
      inject_session_init(session, responseHandler,
                        read_codec->implementation->actual_samples_per_second,
                        host, port, path, sampling, sslFlags, channels, bugname,
                        bidirectional_audio_enable, bidirectional_audio_stream,
                        bidirectional_audio_sample_rate, &pUserData)) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                      "Error initializing mod_audio_inject session.\n");
    return SWITCH_STATUS_FALSE;
  }

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                    "adding bug %s.\n", bugname);
  if ((status = switch_core_media_bug_add(session, bugname, NULL,
                                          capture_callback, pUserData, 0, flags,
                                          &bug)) != SWITCH_STATUS_SUCCESS) {
    return status;
  }
  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                    "setting bug private data %s.\n", bugname);
  switch_channel_set_private(channel, bugname, bug);

  if (inject_session_connect(&pUserData) != SWITCH_STATUS_SUCCESS) {
    /* BUG-08 fix: clean up bug and resources when connect fails */
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                      "Error mod_audio_inject session cannot connect.\n");
    switch_channel_set_private(channel, bugname, NULL);
    switch_core_media_bug_remove(session, &bug);
    return SWITCH_STATUS_FALSE;
  }

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                    "exiting start_capture.\n");
  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char *bugname) {
  switch_status_t status = SWITCH_STATUS_SUCCESS;

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                    "mod_audio_inject (%s): stop\n", bugname);
  status = inject_session_cleanup(session, bugname, 0);

  return status;
}

static switch_status_t do_pauseresume(switch_core_session_t *session,
                                      char *bugname, int pause) {
  switch_status_t status = SWITCH_STATUS_SUCCESS;

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                    "mod_audio_inject (%s): %s\n", bugname,
                    pause ? "pause" : "resume");
  status = inject_session_pauseresume(session, bugname, pause);

  return status;
}

static switch_status_t stop_play(switch_core_session_t *session,
                                 char *bugname) {
  switch_status_t status = SWITCH_STATUS_SUCCESS;

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                    "mod_audio_inject stop_play\n");
  status = inject_session_stop_play(session, bugname);

  return status;
}

static switch_status_t do_graceful_shutdown(switch_core_session_t *session,
                                            char *bugname) {
  switch_status_t status = SWITCH_STATUS_SUCCESS;

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                    "mod_audio_inject (%s): do_graceful_shutdown \n", bugname);
  status = inject_session_graceful_shutdown(session, bugname);

  return status;
}

#define INJECT_API_SYNTAX                                                       \
  "<uuid> [start | stop | pause | resume | graceful-shutdown | stop_play ] "  \
  "[wss-url | path] [mono | mixed | stereo] [8000 | 16000 | 24000 | 32000 | " \
  "64000] [bugname] [bidirectionalAudio_enabled] "                            \
  "[bidirectionalAudio_stream_enabled] [bidirectionalAudio_stream_samplerate]"
SWITCH_STANDARD_API(inject_function) {
  char *mycmd = NULL, *argv[10] = {0};
  int argc = 0;
  switch_status_t status = SWITCH_STATUS_FALSE;
  char *bugname = MY_BUG_NAME;

  if (!zstr(cmd) && (mycmd = strdup(cmd))) {
    argc = switch_separate_string(mycmd, ' ', argv,
                                  (sizeof(argv) / sizeof(argv[0])));
  }
  if (!zstr(cmd)) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                      "mod_audio_inject cmd: %s\n", cmd);
  }

  if (zstr(cmd) || argc < 2 || (0 == strcmp(argv[1], "start") && argc < 4)) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                      "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
    stream->write_function(stream, "-USAGE: %s\n", INJECT_API_SYNTAX);
    goto done;
  } else {
    switch_core_session_t *lsession = NULL;

    if ((lsession = switch_core_session_locate(argv[0]))) {
      if (!strcasecmp(argv[1], "stop")) {
        if (argc > 2) bugname = argv[2];
        status = do_stop(lsession, bugname);
      } else if (!strcasecmp(argv[1], "stop_play")) {
        status = stop_play(lsession, bugname);
      } else if (!strcasecmp(argv[1], "pause")) {
        if (argc > 2) bugname = argv[2];
        status = do_pauseresume(lsession, bugname, 1);
      } else if (!strcasecmp(argv[1], "resume")) {
        if (argc > 2) bugname = argv[2];
        status = do_pauseresume(lsession, bugname, 0);
      } else if (!strcasecmp(argv[1], "graceful-shutdown")) {
        if (argc > 2) bugname = argv[2];
        status = do_graceful_shutdown(lsession, bugname);
      } else if (!strcasecmp(argv[1], "start")) {
        switch_channel_t *channel = switch_core_session_get_channel(lsession);
        char host[MAX_WS_URL_LEN], path[MAX_PATH_LEN];
        unsigned int port;
        int sslFlags;

        int sampling = 8000;
        switch_media_bug_flag_t flags = SMBF_READ_STREAM;
        int bidirectional_audio_enable = 1;
        int bidirectional_audio_stream = 0;
        int bidirectional_audio_sample_rate = 0;
        if (argc > 8) {
          if (argv[5][0] != '\0') {
            bugname = argv[5];
          }
          bidirectional_audio_enable = !strcmp(argv[6], "true") ? 1 : 0;
          bidirectional_audio_stream = !strcmp(argv[7], "true") ? 1 : 0;
          bidirectional_audio_sample_rate = atoi(argv[8]);

          if (bidirectional_audio_enable) {
            flags |= SMBF_WRITE_REPLACE;
          }
        } else if (argc > 5) {
          bugname = argv[5];
        }

        if (0 == strcmp(argv[3], "mixed")) {
          flags |= SMBF_WRITE_STREAM;
        } else if (0 == strcmp(argv[3], "stereo")) {
          flags |= SMBF_WRITE_STREAM;
          flags |= SMBF_STEREO;
        } else if (0 != strcmp(argv[3], "mono")) {
          switch_log_printf(
              SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
              "invalid mix type: %s, must be mono, mixed, or stereo\n",
              argv[3]);
          switch_core_session_rwunlock(lsession);
          goto done;
        }
        if (0 == strcmp(argv[4], "16k")) {
          sampling = 16000;
        } else if (0 == strcmp(argv[4], "8k")) {
          sampling = 8000;
        } else {
          sampling = atoi(argv[4]);
        }
        if (!parse_ws_uri(channel, argv[2], &host[0], &path[0], &port,
                          &sslFlags)) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                            SWITCH_LOG_ERROR, "invalid websocket uri: %s\n",
                            argv[2]);
        } else if (sampling % 8000 != 0) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                            SWITCH_LOG_ERROR, "invalid sample rate: %s\n",
                            argv[4]);
        }
        status = start_capture(lsession, flags, host, port, path, sampling,
                               sslFlags, bidirectional_audio_enable,
                               bidirectional_audio_stream,
                               bidirectional_audio_sample_rate, bugname);
      } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "unsupported mod_audio_inject cmd: %s\n", argv[1]);
      }
      switch_core_session_rwunlock(lsession);
    } else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                        "Error locating session %s\n", argv[0]);
    }
  }

  if (status == SWITCH_STATUS_SUCCESS) {
    stream->write_function(stream, "+OK Success\n");
  } else {
    stream->write_function(stream, "-ERR Operation Failed\n");
  }

done:

  switch_safe_free(mycmd);
  return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_inject_load) {
  switch_api_interface_t *api_interface;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                    "mod_audio_inject API loading..\n");

  /* connect my internal structure to the blank pointer passed to me */
  *module_interface =
      switch_loadable_module_create_module_interface(pool, modname);

  /* create/register custom event message types */
  if (switch_event_reserve_subclass(EVENT_ERROR) != SWITCH_STATUS_SUCCESS ||
      switch_event_reserve_subclass(EVENT_DISCONNECT) !=
          SWITCH_STATUS_SUCCESS) {
    switch_log_printf(
        SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "Couldn't register an event subclass for mod_audio_inject API.\n");
    return SWITCH_STATUS_TERM;
  }
  SWITCH_ADD_API(api_interface, "uuid_audio_inject", "audio_inject API",
                 inject_function, INJECT_API_SYNTAX);
  switch_console_set_complete("add uuid_audio_inject start wss-url");
  switch_console_set_complete("add uuid_audio_inject stop");

  inject_init();

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                    "mod_audio_inject API successfully loaded\n");

  /* indicate that the module should continue to be loaded */
  // mod_running = 1;
  return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_audio_inject_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_inject_shutdown) {
  inject_cleanup();
  // mod_running = 0;
  switch_event_free_subclass(EVENT_DISCONNECT);
  switch_event_free_subclass(EVENT_ERROR);

  return SWITCH_STATUS_SUCCESS;
}

/*
  If it exists, this is called in it's own thread when the module-load completes
  If it returns anything but SWITCH_STATUS_TERM it will be called again
  automatically Macro expands to: switch_status_t mod_audio_inject_runtime()
*/
/*
SWITCH_MODULE_RUNTIME_FUNCTION(mod_audio_inject_runtime)
{
  inject_service_threads(&mod_running);
        return SWITCH_STATUS_TERM;
}
*/
