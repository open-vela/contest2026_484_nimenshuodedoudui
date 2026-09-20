include $(APPDIR)/Make.defs

PROGNAME  = express_ui
PRIORITY  = $(CONFIG_DEMO1_EXPRESS_UI_PRIORITY)
STACKSIZE = $(CONFIG_DEMO1_EXPRESS_UI_STACKSIZE)
MODULE    = $(CONFIG_DEMO1_EXPRESS_UI)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
MAINSRC = express_ui.c
CSRCS   = express_font_20.c

include $(APPDIR)/Application.mk

