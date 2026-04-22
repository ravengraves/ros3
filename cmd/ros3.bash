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
    _ros3_dedupe_compreply
}

_ros3_word_in() {
    local needle word
    needle="$1"
    shift
    for word in "$@"; do
        if [[ "$needle" == "$word" ]]; then
            return 0
        fi
    done
    return 1
}

_ros3_dedupe_compreply() {
    local word
    local deduped=()
    for word in "${COMPREPLY[@]}"; do
        if ! _ros3_word_in "$word" "${deduped[@]}"; then
            deduped+=("$word")
        fi
    done
    COMPREPLY=("${deduped[@]}")
}

_ros3_complete_help_args() {
    _ros3_compgen "$1" -h --help /h /help /?
}

_ros3_complete_hosts() {
    local cur
    cur="$1"
    COMPREPLY=( $(compgen -W "localhost 127.0.0.1 0.0.0.0" -- "$cur") )
    if [[ -n "$cur" ]]; then
        COMPREPLY+=( $(compgen -A hostname -- "$cur") )
    fi
    _ros3_dedupe_compreply
}

_ros3_complete_serial_devices() {
    local cur path
    local words=(
        /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyS0 /dev/ttyAMA0 /dev/serial0
        /dev/cu.usbserial /dev/cu.usbmodem /dev/tty.usbserial /dev/tty.usbmodem
    )
    cur="$1"

    for path in /dev/ttyUSB* /dev/ttyACM* /dev/ttyS* /dev/ttyAMA* /dev/serial* \
        /dev/serial/by-id/* /dev/serial/by-path/* \
        /dev/cu.usbserial* /dev/cu.usbmodem* /dev/tty.usbserial* /dev/tty.usbmodem*; do
        if [[ -e "$path" ]]; then
            words+=("$path")
        fi
    done

    COMPREPLY=()
    if [[ "$cur" == ./* || "$cur" == ../* || "$cur" == ~* \
        || ( "$cur" == */* && "$cur" != /dev/* ) ]]; then
        COMPREPLY=( $(compgen -f -- "$cur") )
    fi
    COMPREPLY+=( $(compgen -W "${words[*]}" -- "$cur") )
    _ros3_dedupe_compreply

    if type compopt >/dev/null 2>&1; then
        compopt -o filenames 2>/dev/null
    fi
}

_ros3_count_positionals() {
    local i word skip
    _ros3_comp_positional_count=0
    skip=0
    for (( i = 1; i < COMP_CWORD; ++i )); do
        word="${COMP_WORDS[i]}"
        if (( skip )); then
            skip=0
            continue
        fi
        if _ros3_word_in "$word" "$@"; then
            skip=1
            continue
        fi
        case "$word" in
            '-h'|'--help'|'/h'|'/help'|'/?'|'-9'|'--udp'|'/udp')
                ;;
            --*)
                ;;
            *)
                ((_ros3_comp_positional_count++))
                ;;
        esac
    done
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

_ros3_complete_rosecore() {
    local cur
    cur="${COMP_WORDS[COMP_CWORD]}"

    if (( COMP_CWORD == 1 )); then
        _ros3_complete_help_args "$cur"
        return 0
    fi

    COMPREPLY=()
}

_ros3_complete_rosenode() {
    local cur prev cmd
    local words=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    cmd="${COMP_WORDS[1]}"

    if (( COMP_CWORD == 1 )); then
        _ros3_compgen "$cur" list info kill -h --help /h /help /?
        return 0
    fi

    case "$cmd" in
        info)
            _ros3_refresh_nodes
            if (( COMP_CWORD == 2 )); then
                words=(--pid /pid)
                words+=("${_ros3_comp_nodes[@]}")
                _ros3_compgen "$cur" "${words[@]}"
                return 0
            fi
            if [[ "$prev" == "--pid" || "$prev" == "/pid" ]]; then
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
        _ros3_compgen "$cur" list info echo hz rm -h --help /h /help /?
        return 0
    fi

    case "$cmd" in
        info|echo|hz|rm)
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
        --read_last|/read_last)
            _ros3_compgen "$cur" 0 1
            return 0
            ;;
        --topic|/topic)
            _ros3_refresh_topics
            _ros3_compgen "$cur" "${_ros3_comp_topics[@]}"
            return 0
            ;;
        --host|/host)
            _ros3_complete_hosts "$cur"
            return 0
            ;;
        --port|/port)
            _ros3_compgen "$cur" 9000
            return 0
            ;;
        --message_size|/message_size)
            _ros3_compgen "$cur" 65536 1048576
            return 0
            ;;
    esac

    words=(
        --topic --host --port --udp --message_size --read_last
        /topic /host /port /udp /message_size /read_last
        -h --help /h /help /?
    )
    if (( COMP_CWORD == 1 )); then
        _ros3_refresh_topics
        words+=("${_ros3_comp_topics[@]}")
        _ros3_compgen "$cur" "${words[@]}"
        return 0
    fi

    if [[ "$cur" == -* ]]; then
        _ros3_compgen "$cur" "${words[@]}"
        return 0
    fi

    _ros3_count_positionals --topic /topic --host /host --port /port \
        --message_size /message_size --read_last /read_last

    case "$_ros3_comp_positional_count" in
        0)
            _ros3_refresh_topics
            _ros3_compgen "$cur" "${_ros3_comp_topics[@]}"
            return 0
            ;;
        1)
            _ros3_complete_hosts "$cur"
            return 0
            ;;
        2)
            _ros3_compgen "$cur" 9000
            return 0
            ;;
        3)
            _ros3_compgen "$cur" --udp /udp
            return 0
            ;;
    esac

    COMPREPLY=()
}

_ros3_complete_rosenet_rx() {
    local cur prev
    local words=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        --topic|/topic)
            _ros3_refresh_topics
            _ros3_compgen "$cur" "${_ros3_comp_topics[@]}"
            return 0
            ;;
        --port|/port)
            _ros3_compgen "$cur" 9000
            return 0
            ;;
        --message_size|/message_size)
            _ros3_compgen "$cur" 65536 1048576
            return 0
            ;;
        --message_rate|/message_rate|--queue_depth|/queue_depth)
            _ros3_compgen "$cur" 512 1024
            return 0
            ;;
    esac

    words=(
        --topic --port --udp --message_size --message_rate --queue_depth
        /topic /port /udp /message_size /message_rate /queue_depth
        -h --help /h /help /?
    )
    if (( COMP_CWORD == 1 )); then
        _ros3_refresh_topics
        words+=("${_ros3_comp_topics[@]}")
        _ros3_compgen "$cur" "${words[@]}"
        return 0
    fi

    if [[ "$cur" == -* ]]; then
        _ros3_compgen "$cur" "${words[@]}"
        return 0
    fi

    _ros3_count_positionals --topic /topic --port /port --message_size /message_size \
        --message_rate /message_rate --queue_depth /queue_depth

    case "$_ros3_comp_positional_count" in
        0)
            _ros3_refresh_topics
            _ros3_compgen "$cur" "${_ros3_comp_topics[@]}"
            return 0
            ;;
        1)
            _ros3_compgen "$cur" 9000
            return 0
            ;;
        2)
            _ros3_compgen "$cur" 65536 1048576
            return 0
            ;;
        3)
            _ros3_compgen "$cur" 512 1024
            return 0
            ;;
        4)
            _ros3_compgen "$cur" --udp /udp
            return 0
            ;;
    esac

    COMPREPLY=()
}

_ros3_complete_roseserial() {
    local cur
    local words=()
    cur="${COMP_WORDS[COMP_CWORD]}"

    case "$COMP_CWORD" in
        1)
            if [[ "$cur" == -* ]]; then
                _ros3_complete_help_args "$cur"
                return 0
            fi
            if [[ "$cur" == /h* || "$cur" == "/?"* ]]; then
                _ros3_compgen "$cur" /h /help /?
                return 0
            fi
            _ros3_complete_serial_devices "$cur"
            return 0
            ;;
        2)
            _ros3_refresh_topics
            words=(/mcu/rx)
            words+=("${_ros3_comp_topics[@]}")
            _ros3_compgen "$cur" "${words[@]}"
            return 0
            ;;
        3)
            _ros3_refresh_topics
            words=(/mcu/tx)
            words+=("${_ros3_comp_topics[@]}")
            _ros3_compgen "$cur" "${words[@]}"
            return 0
            ;;
        4)
            _ros3_compgen "$cur" 9600 19200 38400 57600 115200 230400 460800 \
                921600 1000000 2000000 3000000 4000000
            return 0
            ;;
        5)
            _ros3_compgen "$cur" 1024 4096
            return 0
            ;;
        6)
            _ros3_compgen "$cur" 256 512 1024
            return 0
            ;;
    esac

    COMPREPLY=()
}

complete -F _ros3_complete_rosecore rosecore
complete -F _ros3_complete_rosenode rosenode
complete -F _ros3_complete_rosetopic rosetopic
complete -F _ros3_complete_rosenet_tx rosenet_tx
complete -F _ros3_complete_rosenet_rx rosenet_rx
complete -F _ros3_complete_roseserial roseserial
