
# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2024 sven@turingpi.com
#
obj-y += turingpi2-eeprom.o
obj-y += turingpi2-spl.o
ifneq ($(CONFIG_XPL_BUILD),y)
obj-y += turingpi2-board.o
endif
ccflags-y += -Iboard/turing/pi2/
