# Board-side PTY regression and exit-status wrapper for RT-Smart userspace.
# From the SDK root, after the SDK configuration has been generated:
#   make -f src/rtsmart/rtsmart/kernel/bsp/maix3/testcase/host/pty_lifetime_board.mk OUT=<dir>
# Upload <dir>/pty_lifetime_board.elf as /sdcard/pty-lifetime-board.elf and
# <dir>/exit_status_probe.elf as /sdcard/exit-status-probe.elf, then run
#   /sdcard/exit-status-probe.elf /sdcard/pty-lifetime-board.elf
HOST_TEST_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
include $(HOST_TEST_DIR)/../../../../../../examples/mkenv.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/toolchain_riscv64_musl.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/libmpp.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/librtsmart_hal.mk

OUT ?= $(SDK_SRC_ROOT_DIR)/output/pty-lifetime-board
PROGRAMS := $(OUT)/pty_lifetime_board.elf $(OUT)/exit_status_probe.elf
CFLAGS := -I$(SDK_RTSMART_SRC_DIR)/libs/rtsmart_hal/components/k230_syscalls -march=rv64imafdcv -mabi=lp64d -mcmodel=medany -Os -std=gnu11 -pthread -Wall -Wextra -ffunction-sections -fdata-sections $(LIB_CFLAGS) -I$(SDK_SRC_ROOT_DIR)/include -include generated/autoconf.h
LDFLAGS := -pthread -T $(SDK_RTSMART_SRC_DIR)/libs/mk/link.lds --static -Wl,--gc-sections $(LIB_LDFLAGS)

.PHONY: all clean
all: $(PROGRAMS)
$(OUT)/%.o: $(HOST_TEST_DIR)/%.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/%.elf: $(OUT)/%.o
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@
$(OUT):
	mkdir -p $@
clean:
	rm -f $(PROGRAMS) $(PROGRAMS:.elf=.o)
