#include <nuttx/config.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <aw-alsa-lib/pcm.h>
#include <opus.h>
#define SAMPLE_RATE        16000
#define CHANNELS           1
#define FRAME_MS           60
#define FRAME_SAMPLES      (SAMPLE_RATE * FRAME_MS / 1000)
#define OPUS_BITRATE       24000
#define OPUS_MAX_BYTES     4000
#define AUDIO_UP_PORT      5676
#define AUDIO_DOWN_PORT    5677
#define AUDIO_DEVICE       "hw:snddmic"
static int open_capture(snd_pcm_t **pcm)
{
  snd_pcm_hw_params_t *params;
  unsigned int rate = SAMPLE_RATE;
  int ret;
  ret = snd_vela_pcm_open(pcm, AUDIO_DEVICE, SND_VELA_PCM_STREAM_CAPTURE, 0);
  if (ret < 0)
    {
      printf("sound_app: open %s failed: %d\n",
             AUDIO_DEVICE, ret);
      return ret;
    }
  ret = snd_vela_pcm_hw_params_malloc(&params);
  if (ret < 0)
    {
      snd_vela_pcm_close(*pcm);
      *pcm = NULL;
      return ret;
    }
  ret = snd_vela_pcm_hw_params_any(*pcm, params);
  if (ret >= 0)
    {
      ret = snd_vela_pcm_hw_params_set_access(*pcm, params,
                                         SND_PCM_ACCESS_RW_INTERLEAVED);
    }
  if (ret >= 0)
    {
      ret = snd_vela_pcm_hw_params_set_format(*pcm, params,
                                         SND_PCM_FORMAT_S16_LE);
    }
  if (ret >= 0)
    {
      ret = snd_vela_pcm_hw_params_set_channels(*pcm, params, CHANNELS);
    }
  if (ret >= 0)
    {
      ret = snd_vela_pcm_hw_params_set_rate(*pcm, params, rate, 0);
    }
  if (ret >= 0)
    {
      ret = snd_vela_pcm_hw_params(*pcm, params);
    }
  snd_vela_pcm_hw_params_free(params);
  if (ret < 0)
    {
      printf("sound_app: configure capture failed: %d\n",
             ret);
      snd_vela_pcm_close(*pcm);
      *pcm = NULL;
      return ret;
    }
  ret = snd_vela_pcm_prepare(*pcm);
  if (ret < 0)
    {
      printf("sound_app: prepare capture failed: %d\n",
             ret);
      snd_vela_pcm_close(*pcm);
      *pcm = NULL;
      return ret;
    }
  printf("sound_app: microphone ready, %u Hz, %d channel\n",
         rate, CHANNELS);
  return 0;
}
static void close_capture(snd_pcm_t **pcm)
{
  if (*pcm != NULL)
    {
      snd_vela_pcm_drop(*pcm);
      snd_vela_pcm_close(*pcm);
      *pcm = NULL;
    }
}
static bool is_start_command(const char *command)
{
  return strcmp(command, "start") == 0 ||
         strcmp(command, "listening") == 0;
}
static bool is_stop_command(const char *command)
{
  return strcmp(command, "stop") == 0 ||
         strcmp(command, "standby") == 0;
}
int main(int argc, FAR char *argv[])
{
  struct sockaddr_in local_addr;
  struct sockaddr_in server_addr;
  snd_pcm_t *pcm = NULL;
  OpusEncoder *encoder = NULL;
  int control_fd;
  int audio_fd;
  int opus_error;
  int ret;
  bool recording = false;
  short pcm_data[FRAME_SAMPLES * CHANNELS];
  unsigned char opus_data[OPUS_MAX_BYTES];
  char command[64];
  (void)argc;
  (void)argv;
  printf("sound_app: STT audio sender started\n");
  control_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (control_fd < 0)
    {
      printf("sound_app: create control socket failed: %d\n", errno);
      return -errno;
    }
  memset(&local_addr, 0, sizeof(local_addr));
  local_addr.sin_family = AF_INET;
  local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  local_addr.sin_port = htons(AUDIO_DOWN_PORT);
  if (bind(control_fd, (struct sockaddr *)&local_addr,
           sizeof(local_addr)) < 0)
    {
      printf("sound_app: bind UDP %d failed: %d\n",
             AUDIO_DOWN_PORT, errno);
      close(control_fd);
      return -errno;
    }
  ret = fcntl(control_fd, F_GETFL, 0);
  if (ret >= 0)
    {
      fcntl(control_fd, F_SETFL, ret | O_NONBLOCK);
    }
  audio_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (audio_fd < 0)
    {
      printf("sound_app: create audio socket failed: %d\n", errno);
      close(control_fd);
      return -errno;
    }
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  server_addr.sin_port = htons(AUDIO_UP_PORT);
  printf("sound_app: waiting UDP %d command\n", AUDIO_DOWN_PORT);
  while (true)
    {
      ssize_t command_len;
      command_len = recv(control_fd, command, sizeof(command) - 1, 0);
      if (command_len > 0)
        {
          command[command_len] = '\0';
          if (is_start_command(command) && !recording)
            {
              ret = open_capture(&pcm);
              if (ret < 0)
                {
                  continue;
                }
              encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS,
                                            OPUS_APPLICATION_VOIP,
                                            &opus_error);
              if (opus_error != OPUS_OK || encoder == NULL)
                {
                  printf("sound_app: Opus encoder failed: %s\n",
                         opus_strerror(opus_error));
                  close_capture(&pcm);
                  encoder = NULL;
                  continue;
                }
              opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
              opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
              recording = true;
              printf("sound_app: recording started\n");
            }
          else if (is_stop_command(command) && recording)
            {
              recording = false;
              opus_encoder_destroy(encoder);
              encoder = NULL;
              close_capture(&pcm);
              printf("sound_app: recording stopped\n");
            }
        }
      if (!recording)
        {
          usleep(20000);
          continue;
        }
      ret = snd_vela_pcm_readi(pcm, pcm_data, FRAME_SAMPLES);
      if (ret == -EAGAIN)
        {
          usleep(5000);
          continue;
        }
      if (ret < 0)
        {
          printf("sound_app: read failed: %d\n", ret);
          snd_vela_pcm_prepare(pcm);
          continue;
        }
      if (ret != FRAME_SAMPLES)
        {
          continue;
        }
      ret = opus_encode(encoder, pcm_data, FRAME_SAMPLES,
                        opus_data, sizeof(opus_data));
      if (ret < 0)
        {
          printf("sound_app: Opus encode failed: %s\n",
                 opus_strerror(ret));
          continue;
        }
      if (sendto(audio_fd, opus_data, ret, 0,
                 (struct sockaddr *)&server_addr,
                 sizeof(server_addr)) < 0)
        {
          printf("sound_app: send audio failed: %d\n", errno);
        }
    }
  return 0;
}

