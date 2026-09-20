#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void run_cmd(const char *cmd)
{
  int ret = system(cmd);

  if (ret < 0)
    {
      printf("wifi_auto: failed: %s (%d)\n", cmd, ret);
    }
}

int main(int argc, char *argv[])
{
  printf("==== WiFi auto connect start ====\n");

  run_cmd("ifup wlan0");
  sleep(2);

  run_cmd("wapi mode wlan0 2");
  run_cmd("wapi psk wlan0 cps123456 3");
  run_cmd("wapi essid wlan0 CPS 1");
  run_cmd("renew wlan0");

  printf("==== WiFi connect command finished ====\n");
  return 0;
}
