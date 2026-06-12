#!/usr/bin/env python

# Copyright (c) 2019, Ulf Magnusson
# SPDX-License-Identifier: ISC

"""
Reads a configuration file, then writes a minimal defconfig.

The minimal defconfig omits symbols whose value matches their Kconfig default.
This is the counterpart to defconfig.py, which expands a minimal config into a
full .config file.
"""
import argparse
import os
import re
import shlex
import tempfile

import kconfiglib


_LEGACY_DERIVED_CONFIGS = {
    'CONFIG_LWP_TASK_STACK_SIZE': {'CONFIG_RTSMART_LWP_APP_STACK_SIZE'},
    'CONFIG_TOUCH_DEV_INT_EDGE': {
        'GPIO_PE_RISING',
        'GPIO_PE_FALLING',
        'GPIO_PE_BOTH',
        'GPIO_PE_HIGH',
        'GPIO_PE_LOW',
    },
}


def set_kconfig_env_defaults():
    defaults = {
        'BSP_ROOT': '.',
        'BSP_DIR': '.',
        'RTT_ROOT': '../../rt-thread',
        'RTT_DIR': '../../rt-thread',
        'PKGS_ROOT': 'packages',
        'PKGS_DIR': 'packages',
    }
    for key, value in defaults.items():
        os.environ.setdefault(key, value)


def _legacy_assignment_name_value(line):
    if '=' not in line or line.lstrip().startswith('#'):
        return None, None
    name, value = line.strip().split('=', 1)
    return name, _unquote_config_value(value)


def _normalized_config_for_load(filename):
    changed = False
    lines = []
    try:
        with open(filename, 'r') as config:
            for line in config:
                name, value = _legacy_assignment_name_value(line)
                if value in _LEGACY_DERIVED_CONFIGS.get(name, set()):
                    changed = True
                    continue
                lines.append(line)
    except OSError:
        return filename, None

    if not changed:
        return filename, None

    tmp = tempfile.NamedTemporaryFile('w', delete=False)
    with tmp:
        tmp.writelines(lines)
    return tmp.name, tmp.name


def _unquote_config_value(value):
    value = value.strip()
    try:
        return shlex.split(value)[0]
    except ValueError:
        return value.strip('"')


def _read_user_app_dir_from_config(filename):
    pattern = re.compile(r'^\s*CONFIG_USER_APP_DIR=(.*)$')
    try:
        with open(filename, 'r') as config:
            for line in config:
                match = pattern.match(line)
                if match:
                    return _unquote_config_value(match.group(1))
    except OSError:
        pass
    return None


def _read_user_app_dir_default(kconfig):
    in_user_app_dir = False
    try:
        with open(kconfig, 'r') as config:
            for line in config:
                stripped = line.strip()
                if stripped.startswith('config '):
                    in_user_app_dir = stripped.split(None, 1)[1] == 'USER_APP_DIR'
                    continue
                if not in_user_app_dir:
                    continue
                if stripped.startswith('default '):
                    value = stripped[len('default '):].split(' if ', 1)[0]
                    return _unquote_config_value(value)
                if stripped.startswith(('menuconfig ', 'choice', 'menu ', 'endmenu', 'source ', 'comment ')):
                    break
    except OSError:
        pass
    return None


def set_user_app_dir_env(config, kconfig):
    user_app_dir = (_read_user_app_dir_from_config(config) or
                    os.environ.get('USER_APP_DIR') or
                    _read_user_app_dir_default(kconfig))
    if user_app_dir:
        os.environ['USER_APP_DIR'] = user_app_dir


def main():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__)

    parser.add_argument(
        "--kconfig",
        default="Kconfig",
        help="Base Kconfig file (default: Kconfig)")

    parser.add_argument(
        "-o", "--output",
        default="defconfig",
        help="Output minimal defconfig file (default: defconfig)")

    parser.add_argument(
        "config",
        metavar="CONFIGURATION",
        help="Input full configuration file")

    args = parser.parse_args()

    set_kconfig_env_defaults()
    set_user_app_dir_env(args.config, args.kconfig)

    kconf = kconfiglib.Kconfig(args.kconfig)
    config_file, tmp_config = _normalized_config_for_load(args.config)
    try:
        print(kconf.load_config(config_file))
    finally:
        if tmp_config:
            os.unlink(tmp_config)
    print(kconf.write_min_config(args.output))


if __name__ == "__main__":
    main()
