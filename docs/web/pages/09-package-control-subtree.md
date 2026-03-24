## ROS3 Development Tips

ROS3 projects prefer `git subtree` for package control and dependency vendoring.

This keeps builds reproducible and atomic during robotics competition and demo prep as well as during production deployment.
Code you use is your responsibility as a developer, and it is therefore advantageous to actually have offline local access to all sources depended upon by your project.

### Why the following project structure is preferred for ROS3

- one repo snapshot contains all required code
- stable and repeatable builds without surprise upstream changes
- easy rollback by reverting one commit
- controlled update windows between competition milestones
- package source stays self-contained under `src/packageX/`
- installed artifacts still land in the standard workspace `bin/`, `lib/`, `include/`, and `share/` trees - or optionally can be installed globally on minimal unix distributions into standard directories (e.g., `/usr/local/`)


### Recommended Structure

```text
rose_ws/
  src/
    package1/
      cmd/
        node1/
          main.c
          node1_math.h
          node1_math.c
        node2/
          main.c
      libraries/  # source for exported/installed libraries
        exported_lib1/
          src/
          include/
      internal/
        internal_library_1/
          src/
          include/
      vendor/
        some_external_project_subtree/
      scripts/
        my_tool.py
      docs/
      test/
        check_1.c
        check_1.sh
      README.md
    package2/
      cmd/
      test/
  bin/      # installed node/script executables
  lib/      # installed library files (.so/.a)
  include/  # installed headers (.h)
  share/    # installed config/data/weights
  Makefile  # build script (Makefile or build.ninja preferred)
```

Treat each `src/packageX/` directory as a package source root.
Treat `rose_ws/` itself as the install prefix.

That means:

- source code lives under `src/packageX/`
- installed executables land in `rose_ws/bin/`
- installed libraries land in `rose_ws/lib/`
- installed public headers land in `rose_ws/include/`
- installed runtime data lands in `rose_ws/share/<package>/`

Within a package:

- `cmd/` contains executable entrypoints such as nodes and small tools
- `libraries/` contains source for exported libraries that will be installed
- `internal/` contains package-private code used by `cmd/` and `libraries/`, but not installed as public API
- `vendor/` contains vendored dependencies brought in via `git subtree`
- `scripts/` contains package-local helper scripts
- `docs/` contains package-local documentation and manpage sources
- `test/` contains checks, fixtures, and test scripts

Do not install `internal/` headers into the workspace `include/` tree.
Only install public headers from exported libraries.

## Using `git subtree`

Subtree is a minimal tool for maintaining an internal copy of a git repository in your own git repository.

### Add vendored dependencies

```bash
PKG=src/package1

git subtree add --prefix "$PKG/vendor/ros3" https://git.sr.ht/~alecgraves/ros3 master --squash
git subtree add --prefix "$PKG/vendor/mirage" https://git.sr.ht/~alecgraves/mirage master --squash
git subtree add --prefix "$PKG/vendor/quicksand" https://git.sr.ht/~alecgraves/quicksand master --squash
```

### Update vendored dependencies (when planned)

```bash
PKG=src/package1

git subtree pull --prefix "$PKG/vendor/ros3" https://git.sr.ht/~alecgraves/ros3 master --squash
git subtree pull --prefix "$PKG/vendor/mirage" https://git.sr.ht/~alecgraves/mirage master --squash
git subtree pull --prefix "$PKG/vendor/quicksand" https://git.sr.ht/~alecgraves/quicksand master --squash
```

## Build Target Conventions

The following build command (Makefile/ninja) targets are recommended:
- all - default, builds executables and libraries
- cmd - build rose nodes
- libraries - build rose libraries (.a/.so)
- check - run sanitizers and checks
- docs - build docs
- install - install 
- install-docs - install manpages

When `package1` is built and installed into the workspace:

- `cmd/node1/` builds an executable such as `rose_ws/bin/node1`
- `libraries/exported_lib1/include/` installs public headers into `rose_ws/include/`
- `libraries/exported_lib1/src/` builds a library into `rose_ws/lib/`
- `internal/internal_library_1/` is compiled as needed but not installed publicly
- runtime assets such as config, maps, calibration files, and model blobs should install into `rose_ws/share/package1/`

A practical convention for runtime assets is:

```text
rose_ws/
  share/
    package1/
      config/
      launch/
      models/
      calibration/
      examples/
```

### Practical notes

- Prefer one owning package for each vendored dependency to avoid duplicate subtree copies.
- Keep `cmd/*/main.c` thin; move reusable logic into `libraries/` or `internal/`.
- If a helper is only used inside one node, keep it local to that node directory.
- If a helper is shared across nodes in the package but should not be public, put it in `internal/`.
- If other packages are expected to link against it, promote it into `libraries/`.
