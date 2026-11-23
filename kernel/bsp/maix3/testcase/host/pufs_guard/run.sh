#!/usr/bin/env bash
# Host tests for the PUFS driver (OTP post-mask guard, DMA, CCM, hwrng, getrandom).
# Run with: bash kernel/bsp/maix3/testcase/host/pufs_guard/run.sh
set -eu
cd "$(dirname "$0")"
bsp=../../..
fw=$bsp/drivers/interdrv/cipher/puf_fw
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The kernel normally generates this board header.
touch "$tmp/rtconfig.h"
includes=(-I"$tmp" -Ishim -I"$fw")
for d in "$fw"/pufs_*/; do includes+=("-I$d"); done
"${CC:-cc}" -std=gnu11 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    "${includes[@]}" test_stream.c -o "$tmp/stream"
"$tmp/stream"
sed -n -e '/^static int key_io(/,/^}/p' -e '/^static int ecc_prk_gen(/,/^}/p' \
    "$fw/drv_pufs.c" > "$tmp/key_io.inc"
sed -n -e '/^#define IOCTL_COPY_IN(/,/^$/p' -e '/^#define IOCTL_COPY_BACK(/,/^$/p' \
    "$fw/drv_pufs.c" > "$tmp/status_copies.inc"
sed -n -e '/^    case PUFS_KEY_INOUT:/,/^    }/p' -e '/^    case PUFS_ECC_PRK_GEN:/,/^    }/p' \
    "$fw/drv_pufs.c" > "$tmp/status_cases.inc"
"${CC:-cc}" -std=gnu11 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    "${includes[@]}" -I"$bsp/../../rt-thread/include/libc" test_key_io.c "$fw/pufs_ka/pufs_ka.c" \
    "$fw/pufs_rt/pufs_rt.c" "$fw/pufs_common/pufs_internal.c" -o "$tmp/key_io"
"$tmp/key_io"
sed -n '/^int sys_getrandom(/,/^}/p' \
    "$bsp"/../../rt-thread/components/lwp/lwp_syscall.c > "$tmp/getrandom.inc"
for mode in -DRT_USING_USERSPACE -URT_USING_USERSPACE; do
    "${CC:-cc}" -std=gnu11 "$mode" -I"$tmp" test_getrandom.c -o "$tmp/getrandom"
    "$tmp/getrandom"
done
# Compile the ioctl handler with local user-copy plumbing, without the RTOS.
sed -n -e '/^static bool ctx_buffer_ok(/,/^}/p' -e '/^static bool ctx_key_ok(/,/^}/p' \
    -e '/^static bool hmac_ctx_ok(/,/^}/p' -e '/^static int hmac_op_handler(/,/^}/p' \
    "$fw/drv_pufs.c" > "$tmp/hmac_handler.inc"
# Guard on/off, each with OTP writes enabled and in the default dry-run build.
for guard in active disabled; do
    for write in enabled dry-run; do
        flags=()
        if [[ $guard == disabled ]]; then flags+=(-DRT_PUFS_OTP_POSTMASK_GUARD_DISABLE); fi
        if [[ $write == enabled ]]; then flags+=(-DRT_PUFS_OTP_WRITE_ENABLE); fi
        echo "== guard $guard, OTP write $write"
        "${CC:-cc}" -std=gnu11 -ffunction-sections -fdata-sections -Wl,--gc-sections -no-pie \
            -Werror=unused-function \
            "${includes[@]}" ${flags[@]+"${flags[@]}"} \
            test.c "$fw"/pufs_rt/{pufs_rt,pufs_rt_cde}.c "$fw"/pufs_dma/{pufs_dma,pufs_dma_desc}.c \
            "$fw"/pufs_hmac/pufs_hmac.c "$fw"/pufs_crypto/pufs_crypto.c \
            "$fw"/pufs_sp90a/pufs_sp90a.c "$fw"/pufs_sp38a/pufs_sp38a.c \
            "$fw"/pufs_ecc/pufs_ecc.c "$fw"/pufs_ecp/pufs_ecp_mprog.c \
            "$fw"/pufs_common/pufs_internal.c "$fw"/drv_hwrng.c \
            -o "$tmp/test"
        "$tmp/test"
    done
done
