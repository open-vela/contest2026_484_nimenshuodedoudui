include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_EXAMPLES_SOUND_APP_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_SOUND_APP_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_SOUND_APP_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_SOUND_APP)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/component/aw-tiny-alsa-lib
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/osal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/osal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/opus/opus/include

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/opus/opus/include
MAINSRC = sound_app.c

include $(APPDIR)/Application.mk

LDLIBS += -lm
