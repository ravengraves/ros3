# Bash completion for ROS3 CLI tools.
# Dynamic completion is cached for one second and queried with a short timeout.

: "${ROSE_COMPLETE_TIMEOUT_MS:=10}"

_ros3_comp_nodes_sec=-1
_ros3_comp_topics_sec=-1
_ros3_comp_nodes=()
_ros3_comp_topics=()

_ros3_compgen() {
    local cur
    cur="$1"
    shift
    COMPREPLY=()
    if (( $# == 0 )); then
        return 0
    fi
    COMPREPLY=( $(compgen -W "$*" -- "$cur") )
}

_ros3_refresh_nodes() {
    local now line
    now=$SECONDS
    if (( now == _ros3_comp_nodes_sec )); then
        return 0
    fi

    _ros3_comp_nodes_sec=$now
    _ros3_comp_nodes=()

    if ! command -v rosenode >/dev/null 2>&1; then
        return 0
    fi

    while IFS= read -r line; do
        if [[ -n "$line" ]]; then
            _ros3_comp_nodes+=("$line")
        fi
    done < <(ROSE_QUERY_TIMEOUT_MS="$ROSE_COMPLETE_TIMEOUT_MS" rosenode list 2>/dev/null)
}

_ros3_refresh_topics() {
    local now line
    now=$SECONDS
    if (( now == _ros3_comp_topics_sec )); then
        return 0
    fi

    _ros3_comp_topics_sec=$now
    _ros3_comp_topics=()

    if ! command -v rosetopic >/dev/null 2>&1; then
        return 0
    fi

    while IFS= read -r line; do
        if [[ -n "$line" ]]; then
            _ros3_comp_topics+=("$line")
        fi
    done < <(ROSE_QUERY_TIMEOUT_MS="$ROSE_COMPLETE_TIMEOUT_MS" rosetopic list 2>/dev/null)
}

_ros3_complete_rosemaster() {
    COMPREPLY=()
}

_ros3_complete_rosenode() {
    local cur prev cmd
    local words=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    cmd="${COMP_WORDS[1]}"

    if (( COMP_CWORD == 1 )); then
        _ros3_compgen "$cur" list info kill
        return 0
    fi

    case "$cmd" in
        info)
            _ros3_refresh_nodes
            if (( COMP_CWORD == 2 )); then
                words=(--pid)
                words+=("${_ros3_comp_nodes[@]}")
                _ros3_compgen "$cur" "${words[@]}"
                return 0
            fi
            if [[ "$prev" == "--pid" ]]; then
                COMPREPLY=()
                return 0
            fi
            _ros3_compgen "$cur" "${_ros3_comp_nodes[@]}"
            return 0
            ;;
        kill)
            _ros3_refresh_nodes
            if (( COMP_CWORD == 2 )); then
                words=(-9)
                words+=("${_ros3_comp_nodes[@]}")
                _ros3_compgen "$cur" "${words[@]}"
                return 0
            fi
            if [[ "$prev" == "-9" ]]; then
                _ros3_compgen "$cur" "${_ros3_comp_nodes[@]}"
                return 0
            fi
            _ros3_compgen "$cur" "${_ros3_comp_nodes[@]}"
            return 0
            ;;
    esac

    COMPREPLY=()
}

_ros3_complete_rosetopic() {
    local cur cmd
    cur="${COMP_WORDS[COMP_CWORD]}"
    cmd="${COMP_WORDS[1]}"

    if (( COMP_CWORD == 1 )); then
        _ros3_compgen "$cur" list info echo
        return 0
    fi

    case "$cmd" in
        info|echo)
            if (( COMP_CWORD == 2 )); then
                _ros3_refresh_topics
                _ros3_compgen "$cur" "${_ros3_comp_topics[@]}"
                return 0
            fi
            ;;
    esac

    COMPREPLY=()
}

_ros3_complete_rosenet_tx() {
    local cur prev
    local words=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        --read_last)
            _ros3_compgen "$cur" 0 1
            return 0
            ;;
        --topic)
            _ros3_refresh_topics
            _ros3_compgen "$cur" "${_ros3_comp_topics[@]}"
            return 0
            ;;
        --host|--port|--message_size)
            COMPREPLY=()
            return 0
            ;;
    esac

    words=(--topic --host --port --udp --message_size --read_last)
    if (( COMP_CWORD == 1 )) && [[ "$cur" != -* ]]; then
        _ros3_refresh_topics
        words+=("${_ros3_comp_topics[@]}")
    fi
    _ros3_compgen "$cur" "${words[@]}"
}

_ros3_complete_rosenet_rx() {
    local cur prev
    local words=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        --topic)
            _ros3_refresh_topics
            _ros3_compgen "$cur" "${_ros3_comp_topics[@]}"
            return 0
            ;;
        --port|--message_size|--queue_depth)
            COMPREPLY=()
            return 0
            ;;
    esac

    words=(--topic --port --udp --message_size --queue_depth)
    if (( COMP_CWORD == 1 )) && [[ "$cur" != -* ]]; then
        _ros3_refresh_topics
        words+=("${_ros3_comp_topics[@]}")
    fi
    _ros3_compgen "$cur" "${words[@]}"
}

_ros3_complete_roseserial() {
    local cur
    cur="${COMP_WORDS[COMP_CWORD]}"

    if (( COMP_CWORD == 1 )); then
        _ros3_compgen "$cur" /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyS0 /dev/ttyAMA0 /dev/serial0
        return 0
    fi

    COMPREPLY=()
}

complete -F _ros3_complete_rosemaster rosemaster
complete -F _ros3_complete_rosenode rosenode
complete -F _ros3_complete_rosetopic rosetopic
complete -F _ros3_complete_rosenet_tx rosenet_tx
complete -F _ros3_complete_rosenet_rx rosenet_rx
complete -F _ros3_complete_roseserial roseserial
