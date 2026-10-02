#!/usr/bin/env bash
# Local-only helper: run frame_arch.py against the Steam Frame without
# repeating connection options. All logic lives in frame_arch.py.
#
#   frame-env.sh list
#   frame-env.sh enter [NAME] [-- CMD...]   # default NAME: first listed env
#   frame-env.sh sync [status|push|pull]
#   frame-env.sh <any other frame_arch.py command>
#
# Connection options are NOT stored in this repo. Put them in
# ${FRAME_ENV_CONF:-~/.config/frame-env.conf} or in $FRAME_ARCH_ARGS, e.g.:
#   FRAME_ARCH_ARGS="--ssh USER@HOST --identity KEYFILE --ssh-option UserKnownHostsFile=FILE"
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
conf=${FRAME_ENV_CONF:-$HOME/.config/frame-env.conf}
# shellcheck disable=SC1090
[[ -r $conf ]] && . "$conf"
[[ -n ${FRAME_ARCH_ARGS:-} ]] || { echo "FRAME_ARCH_ARGS unset; see header of $0" >&2; exit 2; }
read -ra connection <<<"$FRAME_ARCH_ARGS"

exec python3 "$here/frame_arch.py" "${connection[@]}" "$@"
