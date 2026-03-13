## Package Control (Git Subtree)

ROS3 projects prefer `git subtree` for package control and vendoring.
This keeps builds reproducible and atomic during competition prep.

### Recommended layout

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
      libs/  # source for exported/installed libraries
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
      doc/
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
- `libs/` contains source for exported libraries that will be installed
- `internal/` contains package-private code used by `cmd/` and `libs/`, but not installed as public API
- `vendor/` contains vendored dependencies brought in via `git subtree`
- `scripts/` contains package-local helper scripts
- `doc/` contains package-local documentation and manpage sources
- `test/` contains checks, fixtures, and test scripts

Do not install `internal/` headers into the workspace `include/` tree.
Only install public headers from exported libraries.

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

### Why this is preferred for ROS3

- one repo snapshot contains all required code
- stable and repeatable builds without surprise upstream changes
- easy rollback by reverting one commit
- controlled update windows between competition milestones
- package source stays self-contained under `src/packageX/`
- installed artifacts still land in the standard workspace `bin/`, `lib/`, `include/`, and `share/` trees

### Build against vendored deps

```bash
WS="$PWD"
PKG=src/package1

make -C "$PKG/vendor/mirage" PREFIX="$WS" install
make -C "$PKG/vendor/quicksand" PREFIX="$WS" install
make -C "$PKG/vendor/ros3" PREFIX="$WS" \
  MIRAGE_DIR="$WS/$PKG/vendor/mirage" \
  QUICKSAND_DIR="$WS/$PKG/vendor/quicksand" \
  install

cc -I"$WS/include" \
  "$PKG/cmd/node1/main.c" \
  "$PKG/cmd/node1/node1_math.c" \
  -L"$WS/lib" \
  -lros3 -lmirage -lquicksand \
  -o "$WS/bin/node1"
```

On macOS, use the Darwin makefiles where the vendored project provides one:

```bash
WS="$PWD"
PKG=src/package1

make -C "$PKG/vendor/mirage" PREFIX="$WS" install
make -C "$PKG/vendor/quicksand" -f Makefile.darwin PREFIX="$WS" install
make -C "$PKG/vendor/ros3" -f Makefile.darwin PREFIX="$WS" \
  MIRAGE_DIR="$WS/$PKG/vendor/mirage" \
  QUICKSAND_DIR="$WS/$PKG/vendor/quicksand" \
  install
```

### Package install conventions

When `package1` is built and installed into the workspace:

- `cmd/node1/` builds an executable such as `rose_ws/bin/node1`
- `libs/exported_lib1/include/` installs public headers into `rose_ws/include/`
- `libs/exported_lib1/src/` builds a library into `rose_ws/lib/`
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
- Keep `cmd/*/main.c` thin; move reusable logic into `libs/` or `internal/`.
- If a helper is only used inside one node, keep it local to that node directory.
- If a helper is shared across nodes in the package but should not be public, put it in `internal/`.
- If other packages are expected to link against it, promote it into `libs/`.
