/****************************************************************************
 * apps/demo1/control_center/control_center.c
 *
 * Xiaozhi speech-to-text control center
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <cJSON.h>

#include "cfg.h"
#include "http.h"
#include "ipc_udp.h"
#include "uuid.h"
#include "websocket_client.h"

/****************************************************************************
 * Definitions
 ****************************************************************************/

#define printf(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

#define XIAOZHI_OTA_URL       "https://api.tenclass.net/xiaozhi/ota/"
#define XIAOZHI_WS_HOST       "api.tenclass.net"
#define XIAOZHI_WS_PORT       "443"
#define XIAOZHI_WS_PATH       "/xiaozhi/v1/"

#define UI_COMMAND_MAX_LENGTH 64
#define SOUND_APP_PORT       5677

/****************************************************************************
 * Device state
 ****************************************************************************/

typedef enum
{
  kDeviceStateUnknown = 0,
  kDeviceStateStarting,
  kDeviceStateWifiConfiguring,
  kDeviceStateIdle,
  kDeviceStateConnecting,
  kDeviceStateListening,
  kDeviceStateSpeaking,
  kDeviceStateUpgrading,
  kDeviceStateActivating,
  kDeviceStateFatalError
} DeviceState;

/****************************************************************************
 * Private data
 ****************************************************************************/

static p_ipc_endpoint_t g_ipc_ep_audio;
static p_ipc_endpoint_t g_ipc_ep_ui;

static DeviceState g_device_state = kDeviceStateUnknown;

static bool g_websocket_ready;
static bool g_audio_upload_enable;

static char g_session_id[64];
static int g_listening_field;
static int g_result_field;

/****************************************************************************
 * UI communication
 ****************************************************************************/

static void set_device_state(DeviceState state)
{
  g_device_state = state;
}

static void send_device_state(void)
{
  char state_string[64];
  int length;

  if (g_ipc_ep_ui == NULL)
    {
      return;
    }

  length = snprintf(state_string,
                    sizeof(state_string),
                    "{\"state\":%d}",
                    (int)g_device_state);

  if (length <= 0 || length >= (int)sizeof(state_string))
    {
      return;
    }

  g_ipc_ep_ui->send(g_ipc_ep_ui,
                    state_string,
                    length + 1);
}

static void send_stt_to_ui(const char *text)
{
  cJSON *json;
  char *json_string;

  if (g_ipc_ep_ui == NULL || text == NULL)
    {
      return;
    }

  json = cJSON_CreateObject();
  if (json == NULL)
    {
      printf("control_center: cannot create STT JSON\n");
      return;
    }

  /*
   * 增加 type 字段，使 express_ui 能区分用户语音识别结果
   * 和其他服务器文字。
   */

  cJSON_AddStringToObject(json, "type", "stt");
  cJSON_AddStringToObject(json, "text", text);
  cJSON_AddNumberToObject(json, "field", g_result_field);

  json_string = cJSON_PrintUnformatted(json);

  if (json_string != NULL)
    {
      g_ipc_ep_ui->send(g_ipc_ep_ui,
                        json_string,
                        strlen(json_string) + 1);

      printf("control_center: STT -> UI: %s\n", text);

      free(json_string);
    }

  cJSON_Delete(json);
}

static void send_message_to_ui(const char *type,
                               const char *message)
{
  cJSON *json;
  char *json_string;

  if (g_ipc_ep_ui == NULL ||
      type == NULL ||
      message == NULL)
    {
      return;
    }

  json = cJSON_CreateObject();
  if (json == NULL)
    {
      return;
    }

  cJSON_AddStringToObject(json, "type", type);
  cJSON_AddStringToObject(json, "message", message);

  json_string = cJSON_PrintUnformatted(json);

  if (json_string != NULL)
    {
      g_ipc_ep_ui->send(g_ipc_ep_ui,
                        json_string,
                        strlen(json_string) + 1);

      free(json_string);
    }

  cJSON_Delete(json);
}


static int send_sound_command(const char *command)
{
  struct sockaddr_in address;
  int fd;
  ssize_t sent;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    {
      printf("control_center: sound socket failed, errno=%d\n", errno);
      return -errno;
    }

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(SOUND_APP_PORT);

  sent = sendto(fd, command, strlen(command) + 1, 0,
                (struct sockaddr *)&address, sizeof(address));
  close(fd);

  if (sent < 0)
    {
      printf("control_center: send sound command failed, errno=%d\n", errno);
      return -errno;
    }

  printf("control_center: sound_app command=%s\n", command);
  return 0;
}

/****************************************************************************
 * Xiaozhi WebSocket commands
 ****************************************************************************/

static int send_start_listening_req(void)
{
  char request[256];
  int length;
  int ret;

  if (!g_websocket_ready || g_session_id[0] == '\0')
    {
      printf("control_center: WebSocket session is not ready\n");
      return -1;
    }

  length = snprintf(request,
                    sizeof(request),
                    "{\"session_id\":\"%s\","
                    "\"type\":\"listen\","
                    "\"state\":\"start\","
                    "\"mode\":\"manual\"}",
                    g_session_id);

  if (length <= 0 || length >= (int)sizeof(request))
    {
      return -1;
    }

  ret = websocket_send_text(request, length);

  printf("control_center: send listen start, ret=%d\n", ret);

  return ret;
}

static int send_stop_listening_req(void)
{
  char request[256];
  int length;
  int ret;

  if (!g_websocket_ready || g_session_id[0] == '\0')
    {
      printf("control_center: WebSocket session is not ready\n");
      return -1;
    }

  length = snprintf(request,
                    sizeof(request),
                    "{\"session_id\":\"%s\","
                    "\"type\":\"listen\","
                    "\"state\":\"stop\"}",
                    g_session_id);

  if (length <= 0 || length >= (int)sizeof(request))
    {
      return -1;
    }

  ret = websocket_send_text(request, length);

  printf("control_center: send listen stop, ret=%d\n", ret);

  return ret;
}

/****************************************************************************
 * WebSocket receive callbacks
 ****************************************************************************/

static void process_opus_data_downloaded(const char *buffer,
                                         size_t size)
{
  /*
   * 当前项目只使用语音识别结果，不播放小智返回的语音。
   */

  (void)buffer;
  (void)size;
}

static void process_hello_json(const char *buffer,
                               size_t size)
{
  cJSON *json;
  cJSON *session_id;
  cJSON *audio_params;
  cJSON *sample_rate;
  cJSON *channels;

  (void)size;

  json = cJSON_Parse(buffer);
  if (json == NULL)
    {
      printf("control_center: invalid hello JSON\n");
      return;
    }

  session_id = cJSON_GetObjectItem(json, "session_id");

  if (cJSON_IsString(session_id) &&
      session_id->valuestring != NULL)
    {
      snprintf(g_session_id,
               sizeof(g_session_id),
               "%s",
               session_id->valuestring);

      g_websocket_ready = true;

      printf("control_center: WebSocket ready, session=%s\n",
             g_session_id);
    }

  audio_params = cJSON_GetObjectItem(json, "audio_params");

  if (cJSON_IsObject(audio_params))
    {
      sample_rate =
          cJSON_GetObjectItem(audio_params, "sample_rate");

      channels =
          cJSON_GetObjectItem(audio_params, "channels");

      if (cJSON_IsNumber(sample_rate) &&
          cJSON_IsNumber(channels))
        {
          printf("control_center: server audio %d Hz, %d channel\n",
                 sample_rate->valueint,
                 channels->valueint);
        }
    }

  /*
   * 连接成功后保持待机。
   * 等 express_ui 发送 listening 后才开始识别。
   */

  g_audio_upload_enable = false;

  set_device_state(kDeviceStateIdle);
  send_device_state();

  send_message_to_ui("ready",
                     "xiaozhi websocket connected");

  cJSON_Delete(json);
}

static void process_server_json(const char *buffer,
                                size_t size)
{
  cJSON *json;
  cJSON *type;
  cJSON *text;

  (void)size;

  json = cJSON_Parse(buffer);
  if (json == NULL)
    {
      printf("control_center: invalid server JSON\n");
      return;
    }

  type = cJSON_GetObjectItem(json, "type");

  if (!cJSON_IsString(type) ||
      type->valuestring == NULL)
    {
      cJSON_Delete(json);
      return;
    }

  /*
   * 只处理用户语音识别结果。
   * tts、llm、iot 消息均忽略。
   */

  if (strcmp(type->valuestring, "stt") == 0)
    {
      text = cJSON_GetObjectItem(json, "text");

      if (cJSON_IsString(text) &&
          text->valuestring != NULL)
        {
          send_stt_to_ui(text->valuestring);
        }
    }
  else if (strcmp(type->valuestring, "tts") == 0)
    {
      /*
       * 不播放服务器返回的语音，也不自动重新进入聆听。
       */
    }
  else if (strcmp(type->valuestring, "llm") == 0)
    {
      /*
       * 当前不显示 AI 回答。
       */
    }
  else if (strcmp(type->valuestring, "iot") == 0)
    {
      /*
       * 当前不处理 LED 或其他 IoT 命令。
       */
    }

  cJSON_Delete(json);
}

static void process_txt_data_downloaded(const char *buffer,
                                        size_t size)
{
  cJSON *json;
  cJSON *type;

  if (buffer == NULL || size == 0)
    {
      return;
    }

  json = cJSON_Parse(buffer);
  if (json == NULL)
    {
      printf("control_center: cannot parse WebSocket message\n");
      return;
    }

  type = cJSON_GetObjectItem(json, "type");

  if (cJSON_IsString(type) &&
      type->valuestring != NULL &&
      strcmp(type->valuestring, "hello") == 0)
    {
      cJSON_Delete(json);
      process_hello_json(buffer, size);
      return;
    }

  cJSON_Delete(json);
  process_server_json(buffer, size);
}

/****************************************************************************
 * Audio upload from sound_app
 ****************************************************************************/

static int process_opus_data_uploaded(char *buffer,
                                      size_t size,
                                      void *user_data)
{
  static unsigned int packet_count;
  int ret;

  (void)user_data;

  if (buffer == NULL || size == 0)
    {
      return -1;
    }

  /*
   * 未点击开始录音时丢弃 sound_app 上传的音频。
   */

  if (!g_audio_upload_enable ||
      !g_websocket_ready)
    {
      return 0;
    }

  if (size > INT_MAX)
    {
      return -1;
    }

  ret = websocket_send_binary(buffer, (int)size);

  if ((packet_count++ % 100) == 0)
    {
      printf("control_center: upload Opus size=%zu ret=%d\n",
             size,
             ret);
    }

  return ret;
}

/****************************************************************************
 * UI command receive callback
 ****************************************************************************/

static int process_ui_data(char *buffer,
                           size_t size,
                           void *user_data)
{
  char command[UI_COMMAND_MAX_LENGTH];
  char *endptr;
  long field;
  int ret;

  (void)user_data;

  if (buffer == NULL || size == 0)
    {
      return -1;
    }

  if (size >= sizeof(command))
    {
      size = sizeof(command) - 1;
    }

  memcpy(command, buffer, size);
  command[size] = '\0';

  printf("control_center: UI command=%s\n", command);

  if (strncmp(command, "listening:", 10) == 0)
    {
      field = strtol(command + 10, &endptr, 10);

      if (endptr == command + 10 || field < 0 || field > 3)
        {
          printf("control_center: invalid field command\n");
          return -1;
        }

      if (!g_websocket_ready)
        {
          send_message_to_ui("error", "xiaozhi websocket is not ready");
          return -1;
        }

      g_listening_field = (int)field;

      ret = send_start_listening_req();
      if (ret < 0)
        {
          send_message_to_ui("error", "cannot start recognition");
          return ret;
        }

      g_audio_upload_enable = true;

      ret = send_sound_command("start");
      if (ret < 0)
        {
          g_audio_upload_enable = false;
          send_stop_listening_req();
          send_message_to_ui("error", "cannot start microphone");
          return ret;
        }

      set_device_state(kDeviceStateListening);
      send_device_state();
      send_message_to_ui("recording", "started");
      return 0;
    }

  if (strcmp(command, "standby") == 0)
    {
      g_audio_upload_enable = false;
      g_result_field = g_listening_field;

      send_sound_command("stop");
      ret = send_stop_listening_req();

      set_device_state(kDeviceStateIdle);
      send_device_state();
      send_message_to_ui("recording", "stopped");

      return ret < 0 ? ret : 0;
    }

  printf("control_center: unknown UI command=%s\n", command);
  return -1;
}

/****************************************************************************
 * UUID configuration
 ****************************************************************************/

static char *read_uuid_from_config(void)
{
  FILE *file;
  char *data;
  char *result;
  long length;
  size_t read_length;
  cJSON *json;
  cJSON *uuid;

  file = fopen(CFG_FILE, "r");
  if (file == NULL)
    {
      return NULL;
    }

  if (fseek(file, 0, SEEK_END) != 0)
    {
      fclose(file);
      return NULL;
    }

  length = ftell(file);

  if (length <= 0 || length > 4096)
    {
      fclose(file);
      return NULL;
    }

  rewind(file);

  data = malloc((size_t)length + 1);
  if (data == NULL)
    {
      fclose(file);
      return NULL;
    }

  read_length = fread(data, 1, (size_t)length, file);
  fclose(file);

  data[read_length] = '\0';

  json = cJSON_Parse(data);
  free(data);

  if (json == NULL)
    {
      return NULL;
    }

  result = NULL;
  uuid = cJSON_GetObjectItem(json, "uuid");

  if (cJSON_IsString(uuid) &&
      uuid->valuestring != NULL)
    {
      result = strdup(uuid->valuestring);
    }

  cJSON_Delete(json);

  return result;
}

static bool write_uuid_to_config(const char *uuid)
{
  FILE *file;
  cJSON *json;
  char *json_string;
  bool success;

  if (uuid == NULL)
    {
      return false;
    }

  /*
   * /data 通常已经存在。若不存在则尝试创建。
   */

  if (mkdir("/data", 0777) < 0 &&
      errno != EEXIST)
    {
      printf("control_center: cannot create /data, errno=%d\n",
             errno);
    }

  file = fopen(CFG_FILE, "w");
  if (file == NULL)
    {
      printf("control_center: cannot open %s, errno=%d\n",
             CFG_FILE,
             errno);

      return false;
    }

  json = cJSON_CreateObject();
  if (json == NULL)
    {
      fclose(file);
      return false;
    }

  cJSON_AddStringToObject(json, "uuid", uuid);

  json_string = cJSON_PrintUnformatted(json);

  success = false;

  if (json_string != NULL)
    {
      if (fwrite(json_string,
                 1,
                 strlen(json_string),
                 file) == strlen(json_string))
        {
          success = true;
        }

      free(json_string);
    }

  cJSON_Delete(json);
  fclose(file);

  return success;
}

/****************************************************************************
 * Main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  char active_code[64];
  char post_buffer[512];
  char headers_buffer[512];
  char ws_headers_buffer[512];

  char *mac;
  char *uuid;

  http_data_t http_data;
  websocket_data_t ws_data;

  int ret;

  (void)argc;
  (void)argv;

  printf("\n");
  printf("====================================\n");
  printf("CONTROL_CENTER BUILD: STT_LINK_V2\n");
  printf("====================================\n");

  active_code[0] = '\0';
  g_session_id[0] = '\0';

  g_ipc_ep_audio = NULL;
  g_ipc_ep_ui = NULL;

  g_websocket_ready = false;
  g_audio_upload_enable = false;
  g_listening_field = 0;
  g_result_field = 0;

  set_device_state(kDeviceStateStarting);

  /*
   * 等待 wlan0 注册并读取 MAC 地址。
   */

  mac = NULL;

  while (mac == NULL ||
         strcmp(mac, "00:00:00:00:00:00") == 0)
    {
      printf("control_center: waiting for wlan0 MAC\n");

      mac = get_wireless_mac_address();

      if (mac == NULL ||
          strcmp(mac, "00:00:00:00:00:00") == 0)
        {
          sleep(1);
        }
    }

  printf("control_center: MAC=%s\n", mac);

  uuid = read_uuid_from_config();

  if (uuid == NULL)
    {
      uuid = generate_uuid();

      if (uuid == NULL)
        {
          printf("control_center: cannot generate UUID\n");
          return -1;
        }

      printf("control_center: generated UUID=%s\n", uuid);

      if (!write_uuid_to_config(uuid))
        {
          printf("control_center: warning: UUID was not saved\n");
        }
    }
  else
    {
      printf("control_center: UUID=%s\n", uuid);
    }

  /*
   * AUDIO_PORT_UP=5676:
   * sound_app 向 control_center 上传 Opus。
   *
   * UI_PORT_UP=5678:
   * express_ui 向 control_center 发送 listening/standby。
   */

  g_ipc_ep_audio =
      ipc_endpoint_create_udp(AUDIO_PORT_UP,
                              AUDIO_PORT_DOWN,
                              process_opus_data_uploaded,
                              NULL);

  g_ipc_ep_ui =
      ipc_endpoint_create_udp(UI_PORT_UP,
                              UI_PORT_DOWN,
                              process_ui_data,
                              NULL);

  if (g_ipc_ep_audio == NULL ||
      g_ipc_ep_ui == NULL)
    {
      printf("control_center: cannot create IPC endpoints\n");
      return -1;
    }

  /*
   * 设备激活。
   */

  memset(&http_data, 0, sizeof(http_data));

  http_data.url = XIAOZHI_OTA_URL;

  snprintf(post_buffer,
           sizeof(post_buffer),
           "{\"uuid\":\"%s\","
           "\"application\":{"
             "\"name\":\"xiaozhi_linux_100ask\","
             "\"version\":\"1.0.0\""
           "},"
           "\"ota\":{},"
           "\"board\":{"
             "\"type\":\"100ask_openvela_board\","
             "\"name\":\"100ask_r528s3_board\""
           "}}",
           uuid);

  http_data.post = post_buffer;

  snprintf(headers_buffer,
           sizeof(headers_buffer),
           "{\"Content-Type\":\"application/json\","
           "\"Device-Id\":\"%s\","
           "\"User-Agent\":\"express-terminal-r528s3\","
           "\"Accept-Language\":\"zh-CN\"}",
           mac);

  http_data.headers = headers_buffer;

  while (1)
    {
      active_code[0] = '\0';

      printf("control_center: OTA request begin\n");
      ret = active_device(&http_data, active_code);
      printf("control_center: OTA request returned ret=%d code=%s\n", ret, active_code);

      if (ret == 0)
        {
          break;
        }

      if (active_code[0] != '\0')
        {
          char activation_message[96];

          snprintf(activation_message,
                   sizeof(activation_message),
                   "Active-Code: %s",
                   active_code);

          set_device_state(kDeviceStateActivating);
          send_device_state();

          send_message_to_ui("activation",
                             activation_message);

          printf("control_center: %s\n",
                 activation_message);
        }

      sleep(5);
    }

  set_device_state(kDeviceStateConnecting);
  send_device_state();

  /*
   * 建立小智 WebSocket。
   */

  memset(&ws_data, 0, sizeof(ws_data));

  snprintf(ws_headers_buffer,
           sizeof(ws_headers_buffer),
           "{\"Authorization\":\"Bearer test-token\","
           "\"Protocol-Version\":\"1\","
           "\"Device-Id\":\"%s\","
           "\"Client-Id\":\"%s\"}",
           mac,
           uuid);

  ws_data.headers = ws_headers_buffer;

  ws_data.hello =
      "{\"type\":\"hello\","
      "\"version\":1,"
      "\"transport\":\"websocket\","
      "\"audio_params\":{"
        "\"format\":\"opus\","
        "\"sample_rate\":16000,"
        "\"channels\":1,"
        "\"frame_duration\":60"
      "}}";

  ws_data.hostname = XIAOZHI_WS_HOST;
  ws_data.port = XIAOZHI_WS_PORT;
  ws_data.path = XIAOZHI_WS_PATH;

  websocket_set_callbacks(process_opus_data_downloaded,
                          process_txt_data_downloaded,
                          &ws_data);

  ret = websocket_start();

  if (ret < 0)
    {
      printf("control_center: websocket_start failed: %d\n",
             ret);

      set_device_state(kDeviceStateFatalError);
      send_device_state();

      return ret;
    }

  printf("control_center: waiting for WebSocket hello\n");

  while (1)
    {
      sleep(1);
    }

  return 0;
}
