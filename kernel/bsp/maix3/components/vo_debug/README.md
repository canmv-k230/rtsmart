# VO debug command

Enable `RT_USING_VO_DEBUG` under **Components Configuration → Enable VO debug
shell command**. It defaults to `n` and requires MSH, POSIX file I/O and the MMU.
The disabled build excludes the module and both shell command names.

```text
vo_debug q
vo_debug set osd4 0
vo_debug set osd4 1
vo_debug dump layer1
```

`vo_debug` without arguments opens the interactive prompt. It accepts `q`,
`set`, `dump`, `help` and `exit`; EOF also exits. `canmv_vo_debug` is a compatibility
alias for the same command, including interactive mode.

Layer names use hardware numbering: `layer1` through `layer3`, and `osd0` through
`osd7`. These are different from MPP logical layer IDs. Layer0 is outside this
command's scope. `set` changes only the mixer enable bit, preserving other bits;
it does not configure or start a layer's DMA. MPP may overwrite these changes.

`dump` saves bank 0 to `./dump_<layer>.bin` in the current directory, replacing
any existing file. It uses the registered physical addresses of each plane
separately and strips stride padding. Video output contains Y followed by UV
(half-height for YUV420, full-height for YUV422); gray output contains Y only.
OSD output contains pixel data followed by a separate 8-bit alpha plane only
when alpha-block mode is selected. Register byte/channel order is preserved.
Mapping, write or close failures return an error and remove the incomplete file.

This reads live hardware state and does not acquire MPP buffer ownership. Keep
buffers allocated and prevent reconfiguration or recycling while dumping. A live
producer can change the contents during a dump; this is not a synchronized frame
capture. Register reads and writes also require the VO hardware to be accessible.

Run the host regression tests with:

```sh
python3 tests/run_tests.py
```

The tests exercise register decoding, command validation, the compatibility
alias, mixer updates above bit 7, separate physical planes, stride padding,
bounded mappings, I/O failures, interactive EOF, and the disabled source guard.
They use host mocks with AddressSanitizer and UndefinedBehaviorSanitizer; display
behavior still requires validation on hardware.
