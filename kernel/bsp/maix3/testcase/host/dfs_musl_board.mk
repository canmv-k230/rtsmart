# Board-side DFS/musl ABI probe for RT-Smart userspace (links k230_syscalls
# through librtsmart_hal.mk, as userapps do). From the SDK root, after the SDK
# and its rtsmart_hal libraries are built:
#   make -f src/rtsmart/rtsmart/kernel/bsp/maix3/testcase/host/dfs_musl_board.mk OUT=<dir>
# Upload <dir>/dfs_musl_board.elf as /sdcard/dfs-musl-board.elf and run it
# (with an optional directory argument, default /sdcard); expect
# "DFS_MUSL failures=0".
HOST_TEST_DIR := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
include $(HOST_TEST_DIR)/../../../../../../examples/mkenv.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/toolchain_riscv64_musl.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/librtsmart_hal.mk

OUT ?= $(SDK_SRC_ROOT_DIR)/output/dfs-musl-board
PROGRAM := $(OUT)/dfs_musl_board.elf
CFLAGS := -march=rv64imafdcv -mabi=lp64d -mcmodel=medany -Os -std=gnu11 -Wall -Wextra -ffunction-sections -fdata-sections $(LIB_CFLAGS)
LDFLAGS := -T $(SDK_RTSMART_SRC_DIR)/libs/mk/link.lds --static -Wl,--gc-sections $(LIB_LDFLAGS)

.PHONY: all clean
all: $(PROGRAM)
$(OUT)/%.o: $(HOST_TEST_DIR)/%.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/%.elf: $(OUT)/%.o
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@
$(OUT):
	mkdir -p $@
clean:
	rm -f $(PROGRAM) $(PROGRAM:.elf=.o)
