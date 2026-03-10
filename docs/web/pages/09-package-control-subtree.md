## Package Control (Git Subtree)

ROS3 projects prefer `git subtree` for package control and vendoring.
This keeps builds reproducible and atomic during competition prep.

### Recommended layout

```text
  src/
    package1/
      src/
      include/
      lib/
      bin/
      vendor/
        ros3/
        mirage/
        quicksand/
    package2/
      src/
      include/
      lib/
      bin/
      vendor/
```

Treat each `src/packageX/` directory as that package's root and install prefix.
This is a ROS1/ROS2-style workspace layout with a Unix-style per-package prefix:
build each package and its vendored dependencies with `PREFIX=<package-root>`,
so headers land in `include/`, libraries land in `lib/`, and executables land in `bin/`.

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
- each package installs into its own `include/`, `lib/`, and `bin/` tree without polluting the workspace root

### Build against vendored deps

```bash
PKG=src/package1
ROOT="$PWD/$PKG"

PREFIX="$ROOT" ninja -C "$PKG/vendor/mirage/build" install
PREFIX="$ROOT" ninja -C "$PKG/vendor/quicksand/build" install
PREFIX="$ROOT" ninja -C "$PKG/vendor/ros3/build" install

cc -I"$ROOT/include" \
  "$PKG/src/app.c" \
  -L"$ROOT/lib" \
  -lros3 -lmirage -lquicksand \
  -o "$ROOT/bin/app"
```
