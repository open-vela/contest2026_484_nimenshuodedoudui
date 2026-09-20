#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include <nuttx/ioexpander/gpio.h>

//vendor/allwinnertech/chips/r528/drv/gpio/drv_gpio.c
static int g_leds[2];
int leds_init(void)
{ 
  int ret;

  g_leds[0] = open("/dev/gpio0", O_RDWR);
  if (g_leds[0] <  0)
  {
    printf("Error opening LED1 GPIO device\n");
    return -1;

  }

  g_leds[1] = open("/dev/gpio2", O_RDWR);
  if (g_leds[1] <  0)
  {
    printf("Error opening LED2 GPIO device\n");
    return -1;

  }  

  for (int i = 0; i < sizeof(g_leds) / sizeof(g_leds[0]); i++)
  {
        ret = ioctl(g_leds[i], GPIOC_SETPINTYPE, GPIO_OUTPUT_PIN);
        if (ret < 0)
        {
            printf("Error setting LED %d GPIO pin type\n", i+1);
            return -1;  
        }
        ioctl(g_leds[i], GPIOC_WRITE, 1);
    }
    return 0;
}

int leds_ctl(int which, bool on)
{
    if (which < 0 || which >= sizeof(g_leds) / sizeof(g_leds[0]))
    {
      printf("Error: Invalid LED number\n");
      return -1;
    }

    int  ret = ioctl(g_leds[which], GPIOC_WRITE, on ? 0 : 1);
    return ret;
}