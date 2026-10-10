XBE_TITLE = XBOX_SCREENSAVER
SRCS = $(CURDIR)/main.c
NXDK_SDL = y
NXDK_NET = y
NXDK_DIR ?= $(CURDIR)/../nxdk
include $(NXDK_DIR)/Makefile
