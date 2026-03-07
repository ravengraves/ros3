## Package Control (Git Subtree)

ROS3 projects prefer `git subtree` for package control and vendoring.
This keeps builds reproducible and atomic during competition prep.

### Recommended layout

```text
your_project/
  vendor/
    mirage/
    quicksand/
  src/
  include/
```

### Add vendored dependencies

```bash
git subtree add --prefix vendor/mirage https://git.sr.ht/~alecgraves/mirage master --squash
git subtree add --prefix vendor/quicksand https://git.sr.ht/~alecgraves/quicksand master --squash
```

### Update vendored dependencies (when planned)

```bash
git subtree pull --prefix vendor/mirage https://git.sr.ht/~alecgraves/mirage master --squash
git subtree pull --prefix vendor/quicksand https://git.sr.ht/~alecgraves/quicksand master --squash
```

### Why this is preferred for ROS3

- one repo snapshot contains all required code
- stable and repeatable builds without surprise upstream changes
- easy rollback by reverting one commit
- controlled update windows between competition milestones

### Build against vendored deps

```bash
cc -Ivendor/mirage/include -Ivendor/quicksand/include -Iinclude \
  src/app.c \
  -Lvendor/mirage/build -Lvendor/quicksand/build -Lbuild \
  -lros3 -lmirage -lquicksand -o app
```
