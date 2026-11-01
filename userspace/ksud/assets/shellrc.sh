#!/system/bin/sh

case $- in *i*) ;; *) return ;; esac

if [ -n "${KSH_VERSION-}" ] && [ -r /system/etc/mkshrc ]; then
    # shellcheck source=/dev/null
    . /system/etc/mkshrc
fi

_ksu_device=$(/system/bin/getprop ro.product.device 2>/dev/null)
# shellcheck disable=SC3028
_ksu_device=${_ksu_device:-${HOSTNAME:-android}}

_ksu_red='' _ksu_blue='' _ksu_reset='' _ksu_prefix=''
if [ -n "${KSH_VERSION-}" ]; then
    _ksu_mark='#'
    [ "${USER_ID:-0}" = 0 ] || _ksu_mark='$'
    if [ -t 2 ] && [ -n "${TERM-}" ] && [ "$TERM" != dumb ] && [ -z "${NO_COLOR-}" ]; then
        _ksu_prefix=$(printf '\001\r')
        _ksu_red=$(printf '\001\033[1;31m\001')
        _ksu_blue=$(printf '\001\033[1;34m\001')
        _ksu_reset=$(printf '\001\033[0m\001')
    fi
else
    _ksu_mark='\$'
    if [ -t 2 ] && [ -n "${TERM-}" ] && [ "$TERM" != dumb ] && [ -z "${NO_COLOR-}" ]; then
        _ksu_red='\[\033[1;31m\]'
        _ksu_blue='\[\033[1;34m\]'
        _ksu_reset='\[\033[0m\]'
    fi
fi
_ksu_prompt() {
    # shellcheck disable=SC3043
    local status="$1" path="${PWD:-?}" identity="${_ksu_device}"
    # shellcheck disable=SC3060
    path=${path//[[:cntrl:]]/?}
    # shellcheck disable=SC3060
    identity=${identity//[[:cntrl:]]/?}
    if [ -z "${KSH_VERSION-}" ]; then
        # shellcheck disable=SC3060
        path=${path//\\/\\\\}
        # shellcheck disable=SC3060
        identity=${identity//\\/\\\\}
    fi
    REPLY=
    [ "$status" = 0 ] || REPLY="${_ksu_red}${status}${_ksu_reset}|"
    REPLY="${REPLY}${identity}:${_ksu_blue}${path}${_ksu_reset} ${_ksu_red}${_ksu_mark}${_ksu_reset} "
    return "$status"
}
if [ -n "${KSH_VERSION-}" ]; then
    PS1="${_ksu_prefix}"'${|_ksu_prompt "$?";}'
else
    # ash needs command substitution; the prompt function uses shell builtins only.
    PS1='$(_ksu_prompt "$?"; printf "%s" "$REPLY")'
fi
if [ "${KSU_SHELL_PS1+x}" = x ]; then
    PS1=$KSU_SHELL_PS1
fi
unset KSU_SHELL_PS1

# User customization is separate from the feature configuration and shipped defaults.
if [ -r /data/adb/ksu/.shellrc ]; then
    # shellcheck source=/dev/null
    . /data/adb/ksu/.shellrc
fi
