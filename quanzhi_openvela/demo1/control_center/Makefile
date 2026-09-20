############################################################################
# apps/demo1/control_center/Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = $(CONFIG_EXAMPLES_CONTROL_CENTER_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_CONTROL_CENTER_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_CONTROL_CENTER_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_CONTROL_CENTER)

MAINSRC = control_center.c

CSRCS = http.c
CSRCS += ipc_udp.c
CSRCS += uuid.c
CSRCS += websocket_client.c


CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/curl/curl/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/libwebsockets/libwebsockets/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/libwebsockets
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/openssl_mbedtls_wrapper/include

include $(APPDIR)/Application.mk
