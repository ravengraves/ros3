windows.md

• What ros3.h and ros3_platform.h give you today is very limited:

  - src/ros3_platform.h (/home/alec/source/rose/ros3/src/ros3_platform.h#L6) only abstracts:
      - parent-death signal setup
      - shutdown handler installation
      - current PID lookup
  - include/ros3.h (/home/alec/source/rose/ros3/include/ros3.h#L190) also exposes portable sleep/time/topic helpers and
    rose_set_pdeathsig(...)

  That is not enough for the CLI layer. The CLI tools still reach around those abstractions and call POSIX APIs directly.

  Missing For Cross-Platform CLI Support

  - Process management abstraction.
      - rosecore uses kill(pid, 0) for liveness checks in cmd/rosecore/core.c (/home/alec/source/rose/ros3/cmd/rosecore/
        core.c#L158).
      - rosenode uses kill(pid, SIGINT/SIGKILL) in cmd/rosenode/core.c (/home/alec/source/rose/ros3/cmd/rosenode/
        core.c#L311).
      - Windows needs an abstraction like rose_platform_process_alive(pid) and probably rose_platform_signal_process(pid,
        mode) or equivalent.
      - SIGKILL has no real Windows equivalent, so rosenode kill -9 needs explicit Windows semantics or should be
        unsupported there.
  - Real shutdown-handler abstraction usage.
      - ros3_platform.h already has rose_platform_install_shutdown_handlers(...), but the CLI tools do not use it.
      - rosecore and rosetopic install raw signal(SIGINT/SIGTERM, ...) handlers in cmd/rosecore/core.c (/home/alec/source/
        rose/ros3/cmd/rosecore/core.c#L581), cmd/rosetopic/core.c (/home/alec/source/rose/ros3/cmd/rosetopic/core.c#L499),
        cmd/rosetopic/core.c (/home/alec/source/rose/ros3/cmd/rosetopic/core.c#L559).
      - On Windows, the current platform shim explicitly says CTRL handler wiring is not implemented in src/
        ros3_platform_windows.c (/home/alec/source/rose/ros3/src/ros3_platform_windows.c).
  - PID abstraction usage.
      - rose_platform_getpid() exists, but rosecore still uses raw getpid() in cmd/rosecore/core.c (/home/alec/source/rose/
        ros3/cmd/rosecore/core.c#L576).
      - More CLI code should consistently use the platform helper instead of POSIX getpid().
  - Socket platform layer for the network tools.
      - rosenet_tx and rosenet_rx are still POSIX socket code:
          - cmd/rosenet_tx/core.c (/home/alec/source/rose/ros3/cmd/rosenet_tx/core.c#L9)
          - cmd/rosenet_rx/core.c (/home/alec/source/rose/ros3/cmd/rosenet_rx/core.c#L9)
      - Missing Windows support pieces include:
          - Winsock init/cleanup (WSAStartup/WSACleanup)
          - SOCKET vs int
          - closesocket() vs close()
          - errno/WSA error mapping
          - writev() replacement in cmd/rosenet_tx/core.c (/home/alec/source/rose/ros3/cmd/rosenet_tx/core.c#L101)
          - poll() replacement or wrapper in cmd/rosenet_rx/core.c (/home/alec/source/rose/ros3/cmd/rosenet_rx/core.c#L234)
          - SIGPIPE logic in cmd/rosenet_tx/core.c (/home/alec/source/rose/ros3/cmd/rosenet_tx/core.c#L124), which is
            POSIX-specific
  - Serial port abstraction.
      - roseserial is Linux/Unix-specific right now:
          - termios.h in cmd/roseserial/core.c (/home/alec/source/rose/ros3/cmd/roseserial/core.c#L18)
          - poll() in cmd/roseserial/core.c (/home/alec/source/rose/ros3/cmd/roseserial/core.c#L147)
          - Unix device naming assumptions in the shell completion file, e.g. /dev/ttyUSB0
      - Windows needs a COM-port implementation, likely via CreateFile, DCB, COMMTIMEOUTS, ReadFile, WriteFile.
  - POSIX header and macro cleanup.
      - Several tools hardcode POSIX-only compile assumptions:
          - _POSIX_C_SOURCE in cmd/rosecore/core.c (/home/alec/source/rose/ros3/cmd/rosecore/core.c#L6), cmd/rosenode/
            core.c (/home/alec/source/rose/ros3/cmd/rosenode/core.c#L10), cmd/rosetopic/core.c (/home/alec/source/rose/
            ros3/cmd/rosetopic/core.c#L10)
          - _GNU_SOURCE in cmd/roseserial/core.c (/home/alec/source/rose/ros3/cmd/roseserial/core.c#L6)
          - unistd.h / sys/types.h includes in several tools
      - Those either need guarding or replacement via platform helpers.
  - Build wiring for real Windows implementations.
      - Every Windows CLI entrypoint except the library is still a stub that prints “native Windows implementation
        pending”, e.g. cmd/rosecore/main+windows.c (/home/alec/source/rose/ros3/cmd/rosecore/main+windows.c#L14), cmd/
        rosenode/main+windows.c (/home/alec/source/rose/ros3/cmd/rosenode/main+windows.c#L14).
      - build_windows.ninja still builds those stubs instead of the actual cores for most tools in build_windows.ninja (/
        home/alec/source/rose/ros3/build_windows.ninja#L36).

  Tool-By-Tool Gaps

  - rosecore
      - Needs process-alive abstraction
      - Needs shutdown handler abstraction usage
      - Needs getpid() replaced with rose_platform_getpid()
      - Needs Windows build to point at the real core
  - rosenode
      - Needs process termination abstraction
      - Needs a Windows-compatible replacement or downgrade for kill -9
      - Needs POSIX includes removed/guarded
      - Needs Windows build to point at the real core
  - rosetopic
      - Mostly closer than the others
      - Still needs shutdown handler abstraction usage instead of raw signal()
      - Needs Windows build to point at the real core
  - rosenet_tx
      - Needs full socket portability layer
      - Needs writev() replacement
      - Needs SIGPIPE handling removed/replaced
      - Needs Windows build to point at the real core
  - rosenet_rx
      - Needs full socket portability layer
      - Needs poll() replacement/wrapper
      - Needs Windows build to point at the real core
  - roseserial
      - Needs a separate Windows serial backend
      - Needs poll() replacement/wrapper
      - Needs Windows COM device conventions
      - Needs Windows build to point at the real core

  Bottom Line

  The main missing piece is not just “fix the mains.” The real gap is that the CLI layer needs a broader platform API than
  src/ros3_platform.h (/home/alec/source/rose/ros3/src/ros3_platform.h#L1) currently provides. Right now it only covers PID
  and signal setup, while the CLIs also need:

  - process liveness / termination
  - console shutdown handling
  - socket lifecycle and polling
  - serial I/O
  - error and handle normalization

turn this into a concrete refactor plan for ros3_platform.h and identify the minimum new functions needed before touching any CLI code.