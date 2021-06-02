#!/bin/sh
# Copyright 2021 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# This script runs an action with rewrapper and reproxy. Use as follows:
#
# ./scripts/rbe_action.sh -- echo Hello
#
# To override the default config:
#
# ./scripts/rbe_action.sh --cfg reclient.cfg -- echo Hello
#
# Note that overriding RBE_cfg will set the same config file for both
# rewrapper and reproxy, so you need to make sure it contains all needed
# arguments for both.
#
# To override the location of reclient binaries, set the RBE_dir env var
# before using the script.

set -e

script=$0
script_dir="$(dirname "$script")"

# defaults
config="$script_dir"/fuchsia-re-client.cfg
# location of reclient binaries relative to output directory where build is run
reclient_bindir=../../prebuilt/proprietary/third_party/reclient/linux-x64

usage() {
  cat <<EOF
$script [rewrapper options] -- command [args...]

options:
  --cfg reclient config for reproxy and rewrapper tools
  All other options forwarded to rewrapper until -- is encountered.
EOF
}

rewrapper_options=()
# Extract script options before --
for opt
do
  # handle --option arg
  if test -n "$prev_opt"
  then
    eval "$prev_opt"=\$opt
    prev_opt=
    shift
    continue
  fi
  # Extract optarg from --opt=optarg
  case "$opt" in
    *=?*) optarg=$(expr "X$opt" : '[^=]*=\(.*\)') ;;
    *=) optarg= ;;
  esac
  case "$opt" in
    --cfg=*) config="$optarg" ;;
    --cfg) prev_opt=config ;;
    # stop option processing
    --) shift; break ;;
    # Forward all other options to rewrapper
    *) rewrapper_options=("${rewrapper_options[@]}" "$opt")
  esac
  shift
done

reproxy_cfg="$config"
rewrapper_cfg="$config"

# command is in "$@"

rewrapper="$reclient_bindir"/rewrapper
bootstrap="$reclient_bindir"/bootstrap
reproxy="$reclient_bindir"/reproxy

# Use the same config for bootstrap as for reproxy
"$bootstrap" --re_proxy="$reproxy" --cfg="$reproxy_cfg"
"$rewrapper" --cfg="$rewrapper_cfg" "${rewrapper_options[@]}" "$@"
# b/188923283 -- added --cfg to shut down properly
"$bootstrap" --shutdown --cfg="$reproxy_cfg"

# TODO(fangism): bootstrap should be done as a setup/shutdown of 'fx build'