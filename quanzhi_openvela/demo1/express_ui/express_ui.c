/****************************************************************************
 * apps/demo1/express_ui/express_ui.c
 ****************************************************************************/
#include <nuttx/config.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/boardctl.h>
#include <sys/socket.h>

#include <cJSON.h>
#include <lvgl/lvgl.h>

LV_FONT_DECLARE(express_font_20);

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

#define TEXT_LEN            128
#define UI_COMMAND_PORT     5678
#define UI_RECEIVE_PORT     5679
#define BUTTON_GUARD_MS     450
#define UART_DEVICE         "/dev/uart1"
#define UART_RX_LINE_LEN    1024
#define UART_WRITE_RETRIES  200

enum receiver_field_e
{
  FIELD_RECEIVER = 0,
  FIELD_SENDER,
  FIELD_PHONE,
  FIELD_ADDRESS,
  FIELD_COUNT
};

/****************************************************************************
 * UI objects
 ****************************************************************************/

static lv_obj_t *g_item_label;
static lv_obj_t *g_weight_label;
static lv_obj_t *g_package_label;
static lv_obj_t *g_receiver_label;
static lv_obj_t *g_sender_label;
static lv_obj_t *g_phone_label;
static lv_obj_t *g_address_label;
static lv_obj_t *g_receiver_box;
static lv_obj_t *g_sender_box;
static lv_obj_t *g_phone_box;
static lv_obj_t *g_address_box;
static lv_obj_t *g_mic_button;
static lv_obj_t *g_mic_label;
static lv_indev_t *g_touch_indev;

/****************************************************************************
 * Order data
 ****************************************************************************/

static char g_item[TEXT_LEN];
static char g_weight[TEXT_LEN];
static char g_package[TEXT_LEN];
static char g_receiver[TEXT_LEN];
static char g_sender[TEXT_LEN];
static char g_phone[TEXT_LEN];
static char g_address[TEXT_LEN];

static bool g_recording;
static enum receiver_field_e g_active_field = FIELD_RECEIVER;
static uint32_t g_last_action_tick;
static int g_ui_udp_fd = -1;

static int g_uart_fd = -1;
static char g_uart_rx_line[UART_RX_LINE_LEN];
static size_t g_uart_rx_length;
static bool g_uart_rx_discard;

/****************************************************************************
 * STT helpers
 ****************************************************************************/

static lv_obj_t *field_label(enum receiver_field_e field)
{
  switch (field)
    {
      case FIELD_RECEIVER:
        return g_receiver_label;

      case FIELD_SENDER:
        return g_sender_label;

      case FIELD_PHONE:
        return g_phone_label;

      case FIELD_ADDRESS:
        return g_address_label;

      default:
        return NULL;
    }
}

static char *field_data(enum receiver_field_e field)
{
  switch (field)
    {
      case FIELD_RECEIVER:
        return g_receiver;

      case FIELD_SENDER:
        return g_sender;

      case FIELD_PHONE:
        return g_phone;

      case FIELD_ADDRESS:
        return g_address;

      default:
        return NULL;
    }
}

static int ui_udp_init(void)
{
  struct sockaddr_in address;
  int flags;

  g_ui_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (g_ui_udp_fd < 0)
    {
      printf("express_ui: UDP socket failed, errno=%d\n", errno);
      return -errno;
    }

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(UI_RECEIVE_PORT);

  if (bind(g_ui_udp_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
      printf("express_ui: bind UDP %d failed, errno=%d\n",
             UI_RECEIVE_PORT, errno);
      close(g_ui_udp_fd);
      g_ui_udp_fd = -1;
      return -errno;
    }

  flags = fcntl(g_ui_udp_fd, F_GETFL, 0);
  if (flags >= 0)
    {
      fcntl(g_ui_udp_fd, F_SETFL, flags | O_NONBLOCK);
    }

  printf("express_ui: listening STT UDP %d\n", UI_RECEIVE_PORT);
  return 0;
}

static int ui_send_command(const char *command)
{
  struct sockaddr_in address;
  ssize_t sent;

  if (g_ui_udp_fd < 0)
    {
      return -1;
    }

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(UI_COMMAND_PORT);

  sent = sendto(g_ui_udp_fd, command, strlen(command) + 1, 0,
                (struct sockaddr *)&address, sizeof(address));

  if (sent < 0)
    {
      printf("express_ui: send %s failed, errno=%d\n", command, errno);
      return -errno;
    }

  printf("express_ui: UI -> control_center: %s\n", command);
  return 0;
}

static void ui_receive_stt(void)
{
  char packet[TEXT_LEN + 128];
  ssize_t length;

  if (g_ui_udp_fd < 0)
    {
      return;
    }

  while ((length = recv(g_ui_udp_fd, packet,
                        sizeof(packet) - 1, 0)) > 0)
    {
      cJSON *json;
      cJSON *type;
      cJSON *text;
      cJSON *field_json;
      int field;
      char *data;
      lv_obj_t *label;

      packet[length] = '\0';
      json = cJSON_Parse(packet);

      if (json == NULL)
        {
          continue;
        }

      type = cJSON_GetObjectItem(json, "type");
      text = cJSON_GetObjectItem(json, "text");
      field_json = cJSON_GetObjectItem(json, "field");

      if (!cJSON_IsString(type) ||
          !cJSON_IsString(text) ||
          !cJSON_IsNumber(field_json) ||
          strcmp(type->valuestring, "stt") != 0)
        {
          cJSON_Delete(json);
          continue;
        }

      field = (int)field_json->valueint;
      if (field < FIELD_RECEIVER || field >= FIELD_COUNT)
        {
          cJSON_Delete(json);
          continue;
        }

      data = field_data((enum receiver_field_e)field);
      label = field_label((enum receiver_field_e)field);

      if (data != NULL)
        {
          snprintf(data, TEXT_LEN, "%s", text->valuestring);
        }

      if (label != NULL)
        {
          lv_label_set_text(label, text->valuestring);
        }

      printf("express_ui: STT field=%d text=%s\n",
             field, text->valuestring);

      cJSON_Delete(json);
    }
}

/****************************************************************************
 * UART1 communication with Phytium
 ****************************************************************************/

static int uart_link_init(void)
{
  struct termios options;
  int flags;

  g_uart_fd = open(UART_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (g_uart_fd < 0)
    {
      printf("express_ui: open %s failed, errno=%d\n",
             UART_DEVICE, errno);
      return -errno;
    }

  if (tcgetattr(g_uart_fd, &options) == 0)
    {
      options.c_iflag = 0;
      options.c_oflag = 0;
      options.c_lflag = 0;
      options.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
      options.c_cflag |= CS8 | CLOCAL | CREAD;
      options.c_cc[VMIN] = 0;
      options.c_cc[VTIME] = 0;

      if (cfsetispeed(&options, B115200) < 0 ||
          cfsetospeed(&options, B115200) < 0 ||
          tcsetattr(g_uart_fd, TCSANOW, &options) < 0)
        {
          printf("express_ui: termios setup failed, using board config, "
                 "errno=%d\n", errno);
        }
    }
  else
    {
      printf("express_ui: termios unavailable, using board config, "
             "errno=%d\n", errno);
    }

  flags = fcntl(g_uart_fd, F_GETFL, 0);
  if (flags >= 0)
    {
      fcntl(g_uart_fd, F_SETFL, flags | O_NONBLOCK);
    }

  g_uart_rx_length = 0;
  g_uart_rx_discard = false;

  printf("express_ui: UART ready: %s, 115200 8N1\n", UART_DEVICE);
  return 0;
}

static int uart_write_all(const char *data, size_t length)
{
  size_t offset = 0;
  unsigned int retries = 0;

  if (g_uart_fd < 0)
    {
      return -ENODEV;
    }

  while (offset < length)
    {
      ssize_t written = write(g_uart_fd, data + offset, length - offset);

      if (written > 0)
        {
          offset += (size_t)written;
          retries = 0;
          continue;
        }

      if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
          return -errno;
        }

      if (++retries >= UART_WRITE_RETRIES)
        {
          return -ETIMEDOUT;
        }

      usleep(1000);
    }

  return 0;
}

static int uart_send_submit(void)
{
  cJSON *json;
  char *packet;
  int result;

  json = cJSON_CreateObject();
  if (json == NULL)
    {
      return -ENOMEM;
    }

  if (cJSON_AddStringToObject(json, "type", "submit") == NULL ||
      cJSON_AddStringToObject(json, "item", g_item) == NULL ||
      cJSON_AddStringToObject(json, "weight", g_weight) == NULL ||
      cJSON_AddStringToObject(json, "package", g_package) == NULL ||
      cJSON_AddStringToObject(json, "receiver", g_receiver) == NULL ||
      cJSON_AddStringToObject(json, "sender", g_sender) == NULL ||
      cJSON_AddStringToObject(json, "phone", g_phone) == NULL ||
      cJSON_AddStringToObject(json, "address", g_address) == NULL)
    {
      cJSON_Delete(json);
      return -ENOMEM;
    }

  packet = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);

  if (packet == NULL)
    {
      return -ENOMEM;
    }

  result = uart_write_all(packet, strlen(packet));
  if (result == 0)
    {
      result = uart_write_all("\n", 1);
    }

  if (result == 0)
    {
      printf("express_ui: UART TX: %s\n", packet);
    }

  free(packet);
  return result;
}

static void uart_handle_line(const char *line)
{
  cJSON *json;
  cJSON *type;
  cJSON *item;
  cJSON *weight;
  cJSON *package;

  json = cJSON_Parse(line);
  if (json == NULL)
    {
      printf("express_ui: invalid UART JSON: %s\n", line);
      return;
    }

  type = cJSON_GetObjectItem(json, "type");
  item = cJSON_GetObjectItem(json, "item");
  weight = cJSON_GetObjectItem(json, "weight");
  package = cJSON_GetObjectItem(json, "package");

  if (!cJSON_IsString(type) ||
      strcmp(type->valuestring, "parcel") != 0 ||
      !cJSON_IsString(item) ||
      !cJSON_IsString(weight) ||
      !cJSON_IsString(package))
    {
      printf("express_ui: unsupported UART message: %s\n", line);
      cJSON_Delete(json);
      return;
    }

  snprintf(g_item, sizeof(g_item), "%s", item->valuestring);
  snprintf(g_weight, sizeof(g_weight), "%s", weight->valuestring);
  snprintf(g_package, sizeof(g_package), "%s", package->valuestring);

  if (g_item_label != NULL)
    {
      lv_label_set_text(g_item_label, g_item);
    }

  if (g_weight_label != NULL)
    {
      lv_label_set_text(g_weight_label, g_weight);
    }

  if (g_package_label != NULL)
    {
      lv_label_set_text(g_package_label, g_package);
    }

  printf("express_ui: parcel item=%s weight=%s package=%s\n",
         g_item, g_weight, g_package);

  cJSON_Delete(json);
}

static void uart_receive(void)
{
  char buffer[128];
  ssize_t length;

  if (g_uart_fd < 0)
    {
      return;
    }

  while ((length = read(g_uart_fd, buffer, sizeof(buffer))) > 0)
    {
      ssize_t index;

      for (index = 0; index < length; index++)
        {
          char byte = buffer[index];

          if (byte == '\r')
            {
              continue;
            }

          if (byte == '\n')
            {
              if (!g_uart_rx_discard && g_uart_rx_length > 0)
                {
                  g_uart_rx_line[g_uart_rx_length] = '\0';
                  uart_handle_line(g_uart_rx_line);
                }

              g_uart_rx_length = 0;
              g_uart_rx_discard = false;
              continue;
            }

          if (g_uart_rx_discard)
            {
              continue;
            }

          if (g_uart_rx_length + 1 >= sizeof(g_uart_rx_line))
            {
              printf("express_ui: UART line too long, discarded\n");
              g_uart_rx_length = 0;
              g_uart_rx_discard = true;
              continue;
            }

          g_uart_rx_line[g_uart_rx_length++] = byte;
        }
    }

  if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
      printf("express_ui: UART read failed, errno=%d\n", errno);
    }
}

/****************************************************************************
 * Page declarations
 ****************************************************************************/

static void show_home_page(void);
static void show_parcel_page(void);
static void show_receiver_page(void);

/****************************************************************************
 * Common functions
 ****************************************************************************/

static void set_chinese_font(lv_obj_t *obj)
{
  lv_obj_set_style_text_font(obj, &express_font_20, 0);
}

static bool action_allowed(void)
{
  uint32_t now = lv_tick_get();

  if ((uint32_t)(now - g_last_action_tick) < BUTTON_GUARD_MS)
    {
      return false;
    }

  g_last_action_tick = now;
  return true;
}

static void reset_object_pointers(void)
{
  g_item_label = NULL;
  g_weight_label = NULL;
  g_package_label = NULL;
  g_receiver_label = NULL;
  g_sender_label = NULL;
  g_phone_label = NULL;
  g_address_label = NULL;
  g_receiver_box = NULL;
  g_sender_box = NULL;
  g_phone_box = NULL;
  g_address_box = NULL;
  g_mic_button = NULL;
  g_mic_label = NULL;
}

static void clear_all_order_data(void)
{
  g_item[0] = '\0';
  g_weight[0] = '\0';
  g_package[0] = '\0';
  g_receiver[0] = '\0';
  g_sender[0] = '\0';
  g_phone[0] = '\0';
  g_address[0] = '\0';
  g_recording = false;
  g_active_field = FIELD_RECEIVER;
}

static void reset_touch_state(void)
{
  if (g_touch_indev != NULL)
    {
      lv_indev_reset(g_touch_indev, NULL);
    }
}

static void prepare_screen(void)
{
  lv_obj_t *screen = lv_screen_active();

  reset_object_pointers();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0xf4f7fb), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_label(lv_obj_t *parent,
                              const char *text,
                              int32_t x,
                              int32_t y)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  set_chinese_font(label);
  lv_obj_set_style_text_color(label, lv_color_hex(0x243447), 0);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

  return label;
}

static lv_obj_t *create_display_box(lv_obj_t *parent,
                                    const char *value,
                                    int32_t x,
                                    int32_t y,
                                    int32_t width,
                                    int32_t height,
                                    lv_obj_t **label_out)
{
  lv_obj_t *box;
  lv_obj_t *label;

  box = lv_obj_create(parent);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_size(box, width, height);
  lv_obj_set_style_bg_color(box, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(0xaab7c4), 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_set_style_pad_left(box, 6, 0);
  lv_obj_set_style_pad_right(box, 6, 0);
  lv_obj_set_style_pad_top(box, 4, 0);
  lv_obj_set_style_pad_bottom(box, 4, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

  label = create_label(box, value, 0, 0);
  lv_obj_set_width(label, LV_PCT(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

  if (label_out != NULL)
    {
      *label_out = label;
    }

  return box;
}

static lv_obj_t *create_button(lv_obj_t *parent,
                               const char *text,
                               int32_t x,
                               int32_t y,
                               int32_t width,
                               int32_t height,
                               lv_color_t color,
                               lv_event_cb_t callback)
{
  lv_obj_t *button;
  lv_obj_t *label;

  button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_bg_color(button, color, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(button, 9, 0);
  lv_obj_set_style_shadow_width(button, 6, 0);
  lv_obj_set_style_shadow_opa(button, LV_OPA_20, 0);
  lv_obj_set_style_shadow_offset_y(button, 2, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_PRESSED, NULL);

  label = create_label(button, text, 0, 0);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_center(label);

  return button;
}

/****************************************************************************
 * Receiver-page helpers
 ****************************************************************************/

static const char *field_name(enum receiver_field_e field)
{
  switch (field)
    {
      case FIELD_RECEIVER:
        return "收件人";

      case FIELD_SENDER:
        return "寄件人";

      case FIELD_PHONE:
        return "电话";

      case FIELD_ADDRESS:
        return "地址";

      default:
        return "收件人";
    }
}

static lv_obj_t *field_box(enum receiver_field_e field)
{
  switch (field)
    {
      case FIELD_RECEIVER:
        return g_receiver_box;

      case FIELD_SENDER:
        return g_sender_box;

      case FIELD_PHONE:
        return g_phone_box;

      case FIELD_ADDRESS:
        return g_address_box;

      default:
        return NULL;
    }
}

static void update_field_focus(void)
{
  enum receiver_field_e field;

  for (field = FIELD_RECEIVER; field < FIELD_COUNT; field++)
    {
      lv_obj_t *box = field_box(field);

      if (box == NULL)
        {
          continue;
        }

      if (field == g_active_field)
        {
          lv_obj_set_style_border_color(box, lv_color_hex(0x1677d2), 0);
          lv_obj_set_style_border_width(box, 3, 0);
        }
      else
        {
          lv_obj_set_style_border_color(box, lv_color_hex(0xaab7c4), 0);
          lv_obj_set_style_border_width(box, 2, 0);
        }
    }
}

static void update_mic_button(void)
{
  if (g_mic_button == NULL || g_mic_label == NULL)
    {
      return;
    }

  if (g_recording)
    {
      lv_label_set_text(g_mic_label, "停止录音");
      lv_obj_set_style_bg_color(g_mic_button, lv_color_hex(0xe74c3c), 0);
    }
  else
    {
      lv_label_set_text(g_mic_label, "开始录音");
      lv_obj_set_style_bg_color(g_mic_button, lv_color_hex(0x1677d2), 0);
    }

  lv_obj_center(g_mic_label);
}

static void start_recording(enum receiver_field_e field)
{
  char command[32];

  if (g_recording && g_active_field != field)
    {
      ui_send_command("standby");
    }

  g_active_field = field;
  g_recording = true;

  update_field_focus();
  update_mic_button();

  snprintf(command, sizeof(command), "listening:%d", (int)field);
  ui_send_command(command);

  printf("express_ui: start recording field=%s\n", field_name(field));
  fflush(stdout);
}

static void stop_recording(void)
{
  g_recording = false;
  update_mic_button();

  ui_send_command("standby");

  printf("express_ui: stop recording field=%s\n",
         field_name(g_active_field));
  fflush(stdout);
}

static void clear_one_field(enum receiver_field_e field)
{
  lv_obj_t *label = NULL;

  switch (field)
    {
      case FIELD_RECEIVER:
        g_receiver[0] = '\0';
        label = g_receiver_label;
        break;

      case FIELD_SENDER:
        g_sender[0] = '\0';
        label = g_sender_label;
        break;

      case FIELD_PHONE:
        g_phone[0] = '\0';
        label = g_phone_label;
        break;

      case FIELD_ADDRESS:
        g_address[0] = '\0';
        label = g_address_label;
        break;

      default:
        return;
    }

  if (label != NULL)
    {
      lv_label_set_text(label, "");
    }

  printf("express_ui: cleared field=%s\n", field_name(field));
  fflush(stdout);
}

static void clear_receiver_page(void)
{
  g_receiver[0] = '\0';
  g_sender[0] = '\0';
  g_phone[0] = '\0';
  g_address[0] = '\0';

  if (g_receiver_label != NULL)
    {
      lv_label_set_text(g_receiver_label, "");
    }

  if (g_sender_label != NULL)
    {
      lv_label_set_text(g_sender_label, "");
    }

  if (g_phone_label != NULL)
    {
      lv_label_set_text(g_phone_label, "");
    }

  if (g_address_label != NULL)
    {
      lv_label_set_text(g_address_label, "");
    }

  if (g_recording)
    {
      stop_recording();
    }

  printf("express_ui: cleared receiver page\n");
  fflush(stdout);
}

/****************************************************************************
 * Page switching
 ****************************************************************************/

static void show_home_page(void);
static void show_parcel_page(void);
static void show_receiver_page(void);

static void async_show_home_page(void *data)
{
  (void)data;
  show_home_page();
  reset_touch_state();
}

static void async_show_parcel_page(void *data)
{
  (void)data;
  show_parcel_page();
  reset_touch_state();
}

static void async_show_receiver_page(void *data)
{
  (void)data;
  show_receiver_page();
  reset_touch_state();
}

static void schedule_page(void (*callback)(void *))
{
  if (lv_async_call(callback, NULL) != LV_RESULT_OK)
    {
      LV_LOG_ERROR("Cannot schedule page");
    }
}

/****************************************************************************
 * Button callbacks
 ****************************************************************************/

static void home_start_event(lv_event_t *event)
{
  (void)event;

  if (!action_allowed())
    {
      return;
    }

  schedule_page(async_show_parcel_page);
}

static void parcel_next_event(lv_event_t *event)
{
  (void)event;

  if (!action_allowed())
    {
      return;
    }

  schedule_page(async_show_receiver_page);
}

static void receiver_box_event(lv_event_t *event)
{
  (void)event;

  if (action_allowed())
    {
      start_recording(FIELD_RECEIVER);
    }
}

static void sender_box_event(lv_event_t *event)
{
  (void)event;

  if (action_allowed())
    {
      start_recording(FIELD_SENDER);
    }
}

static void phone_box_event(lv_event_t *event)
{
  (void)event;

  if (action_allowed())
    {
      start_recording(FIELD_PHONE);
    }
}

static void address_box_event(lv_event_t *event)
{
  (void)event;

  if (action_allowed())
    {
      start_recording(FIELD_ADDRESS);
    }
}

static void mic_event(lv_event_t *event)
{
  (void)event;

  if (!action_allowed())
    {
      return;
    }

  if (g_recording)
    {
      stop_recording();
    }
  else
    {
      start_recording(g_active_field);
    }
}

static void ignore_event(lv_event_t *event)
{
  (void)event;
}

static void clear_one_event(lv_event_t *event)
{
  enum receiver_field_e field =
    (enum receiver_field_e)(uintptr_t)lv_event_get_user_data(event);

  if (!action_allowed())
    {
      return;
    }

  clear_one_field(field);
}

static void clear_all_event(lv_event_t *event)
{
  (void)event;

  if (!action_allowed())
    {
      return;
    }

  clear_receiver_page();
}

static void confirm_event(lv_event_t *event)
{
  int result;

  (void)event;

  if (!action_allowed())
    {
      return;
    }

  if (g_recording)
    {
      stop_recording();
    }

  printf("\n========== EXPRESS ORDER ==========\n");
  printf("item     : %s\n", g_item);
  printf("weight   : %s\n", g_weight);
  printf("package  : %s\n", g_package);
  printf("receiver : %s\n", g_receiver);
  printf("sender   : %s\n", g_sender);
  printf("phone    : %s\n", g_phone);
  printf("address  : %s\n", g_address);
  printf("===================================\n");
  fflush(stdout);

  result = uart_send_submit();
  if (result < 0)
    {
      printf("express_ui: UART submit failed, result=%d\n", result);
      return;
    }

  clear_all_order_data();
  schedule_page(async_show_home_page);
}

/****************************************************************************
 * Page 1: Home
 ****************************************************************************/

static void show_home_page(void)
{
  lv_obj_t *screen;
  lv_obj_t *title;
  lv_obj_t *subtitle;
  lv_obj_t *button;

  prepare_screen();
  screen = lv_screen_active();

  title = create_label(screen, "智能快递终端", 0, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x123b68), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

  subtitle = create_label(screen, "语音  |  视觉  |  分拣", 0, 0);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x607080), 0);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 83);

  button = create_button(screen, "开始寄件",
                         40, 235, 240, 78,
                         lv_color_hex(0x1677d2),
                         home_start_event);
  lv_obj_move_foreground(button);

  subtitle = create_label(screen, "点击开始进入包裹信息", 0, 0);
  lv_obj_set_width(subtitle, 280);
  lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x607080), 0);
  lv_obj_align(subtitle, LV_ALIGN_BOTTOM_MID, 0, -52);
}

/****************************************************************************
 * Page 2: Parcel information
 ****************************************************************************/

static void show_parcel_page(void)
{
  lv_obj_t *screen;
  lv_obj_t *title;
  lv_obj_t *next_button;

  prepare_screen();
  screen = lv_screen_active();

  title = create_label(screen, "包裹信息", 0, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x123b68), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  create_label(screen, "物品", 20, 72);
  create_display_box(screen, g_item, 90, 60, 210, 46, &g_item_label);

  create_label(screen, "重量", 20, 132);
  create_display_box(screen, g_weight, 90, 120, 210, 46,
                     &g_weight_label);

  create_label(screen, "包装", 20, 192);
  create_display_box(screen, g_package, 90, 180, 210, 46,
                     &g_package_label);

  next_button = create_button(screen, "下一步",
                              40, 250, 240, 70,
                              lv_color_hex(0x28a745),
                              parcel_next_event);
  lv_obj_move_foreground(next_button);
}

/****************************************************************************
 * Page 3: Receiver information
 ****************************************************************************/

static void show_receiver_page(void)
{
  lv_obj_t *screen;
  lv_obj_t *title;
  lv_obj_t *button;

  prepare_screen();
  screen = lv_screen_active();

  title = create_label(screen, "收件信息", 0, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x123b68), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 7);

  create_label(screen, "收件人", 15, 44);
  g_receiver_box = create_display_box(screen, g_receiver,
                                      95, 34, 210, 38,
                                      &g_receiver_label);
  lv_obj_add_event_cb(g_receiver_box, receiver_box_event,
                      LV_EVENT_PRESSED, NULL);

  create_label(screen, "寄件人", 15, 86);
  g_sender_box = create_display_box(screen, g_sender,
                                    95, 76, 210, 38,
                                    &g_sender_label);
  lv_obj_add_event_cb(g_sender_box, sender_box_event,
                      LV_EVENT_PRESSED, NULL);

  create_label(screen, "电话", 15, 128);
  g_phone_box = create_display_box(screen, g_phone,
                                   95, 118, 210, 38,
                                   &g_phone_label);
  lv_obj_add_event_cb(g_phone_box, phone_box_event,
                      LV_EVENT_PRESSED, NULL);

  create_label(screen, "地址", 15, 170);
  g_address_box = create_display_box(screen, g_address,
                                     15, 192, 290, 48,
                                     &g_address_label);
  lv_obj_add_event_cb(g_address_box, address_box_event,
                      LV_EVENT_PRESSED, NULL);

  g_recording = false;
  g_active_field = FIELD_RECEIVER;
  update_field_focus();

  g_mic_button = create_button(screen, "开始录音",
                               70, 250, 180, 48,
                               lv_color_hex(0x1677d2),
                               mic_event);
  g_mic_label = lv_obj_get_child(g_mic_button, 0);

  button = create_button(screen, "1",
                         15, 315, 58, 42,
                         lv_color_hex(0xef8c2f),
                         ignore_event);
  lv_obj_remove_event_cb(button, ignore_event);
  lv_obj_add_event_cb(button, clear_one_event, LV_EVENT_PRESSED,
                      (void *)(uintptr_t)FIELD_RECEIVER);

  button = create_button(screen, "2",
                         87, 315, 58, 42,
                         lv_color_hex(0xef8c2f),
                         ignore_event);
  lv_obj_remove_event_cb(button, ignore_event);
  lv_obj_add_event_cb(button, clear_one_event, LV_EVENT_PRESSED,
                      (void *)(uintptr_t)FIELD_SENDER);

  button = create_button(screen, "3",
                         159, 315, 58, 42,
                         lv_color_hex(0xef8c2f),
                         ignore_event);
  lv_obj_remove_event_cb(button, ignore_event);
  lv_obj_add_event_cb(button, clear_one_event, LV_EVENT_PRESSED,
                      (void *)(uintptr_t)FIELD_PHONE);

  button = create_button(screen, "4",
                         231, 315, 58, 42,
                         lv_color_hex(0xef8c2f),
                         ignore_event);
  lv_obj_remove_event_cb(button, ignore_event);
  lv_obj_add_event_cb(button, clear_one_event, LV_EVENT_PRESSED,
                      (void *)(uintptr_t)FIELD_ADDRESS);

  button = create_button(screen, "清空全部",
                         15, 375, 135, 52,
                         lv_color_hex(0xe74c3c),
                         clear_all_event);

  button = create_button(screen, "确认提交",
                         170, 375, 135, 52,
                         lv_color_hex(0x28a745),
                         confirm_event);

  lv_obj_move_foreground(g_mic_button);
}

/****************************************************************************
 * Main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

  (void)argc;
  (void)argv;

  printf("\n====================================\n");
  printf("EXPRESS_UI BUILD: UART1_LINK_V7\n");
  printf("====================================\n");
  fflush(stdout);

  g_touch_indev = NULL;
  g_last_action_tick = 0;

  if (lv_is_initialized())
    {
      LV_LOG_ERROR("LVGL already initialized");
      return -1;
    }

#ifdef NEED_BOARDINIT
  boardctl(BOARDIOC_INIT, 0);
#endif

  lv_init();
  lv_nuttx_dsc_init(&info);

#ifdef CONFIG_LV_USE_NUTTX_LCD
  info.fb_path = "/dev/lcd0";
#endif

#if LV_USE_NUTTX_TOUCHSCREEN
  info.input_path = "/dev/input0";
#endif

  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      LV_LOG_ERROR("Display initialization failed");
      return -1;
    }

#if LV_USE_NUTTX_TOUCHSCREEN
  g_touch_indev = result.indev;
#endif

  ui_udp_init();
  uart_link_init();

  clear_all_order_data();
  show_home_page();

  while (1)
    {
      uint32_t idle;

      ui_receive_stt();
      uart_receive();

      idle = lv_timer_handler();

      if (idle == 0)
        {
          idle = 1;
        }
      else if (idle > 20)
        {
          idle = 20;
        }

      usleep(idle * 1000);
    }

  return 0;
}
