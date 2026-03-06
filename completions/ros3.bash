# Bash completion for ROS3 CLI tools.
# Fast path: static completions only, no subprocess calls.

_ros3_complete_rosemaster() {
    COMPREPLY=()
}

_ros3_complete_rosenode() {
    local cur prev cmd
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    cmd="${COMP_WORDS[1]}"

    if (( COMP_CWORD == 1 )); then
        COMPREPLY=( $(compgen -W "list info kill" -- "$cur") )
        return 0
    fi

    case "$cmd" in
        info)
            if (( COMP_CWORD == 2 )); then
                COMPREPLY=( $(compgen -W "--pid" -- "$cur") )
                return 0
            fi
            if [[ "$prev" == "--pid" ]]; then
                COMPREPLY=()
                return 0
            fi
            ;;
        kill)
            if (( COMP_CWORD == 2 )); then
                COMPREPLY=( $(compgen -W "-9" -- "$cur") )
                return 0
            fi
            ;;
    esac

    COMPREPLY=()
}

_ros3_complete_rosetopic() {
    local cur
    cur="${COMP_WORDS[COMP_CWORD]}"

    if (( COMP_CWORD == 1 )); then
        COMPREPLY=( $(compgen -W "list info echo" -- "$cur") )
        return 0
    fi

    COMPREPLY=()
}

_ros3_complete_rosenet_tx() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        --read_last)
            COMPREPLY=( $(compgen -W "0 1" -- "$cur") )
            return 0
            ;;
        --topic|--host|--port|--message_size)
            COMPREPLY=()
            return 0
            ;;
    esac

    COMPREPLY=( $(compgen -W "--topic --host --port --udp --message_size --read_last" -- "$cur") )
}

_ros3_complete_rosenet_rx() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        --topic|--port|--message_size|--queue_depth)
            COMPREPLY=()
            return 0
            ;;
    esac

    COMPREPLY=( $(compgen -W "--topic --port --udp --message_size --queue_depth" -- "$cur") )
}

_ros3_complete_roseserial() {
    local cur
    cur="${COMP_WORDS[COMP_CWORD]}"

    if (( COMP_CWORD == 1 )); then
        COMPREPLY=( $(compgen -W "/dev/ttyUSB0 /dev/ttyACM0 /dev/ttyS0 /dev/ttyAMA0 /dev/serial0" -- "$cur") )
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
