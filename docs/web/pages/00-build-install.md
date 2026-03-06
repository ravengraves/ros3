## Build and Installation

### Repositories

- `ros3`: <https://git.sr.ht/~alecgraves/ros3>
- `mirage`: <https://git.sr.ht/~alecgraves/mirage>
- `quicksand`: <https://git.sr.ht/~alecgraves/quicksand>

### Requirements

- `clang` or `gcc`
- `make` or `ninja`
- `scdoc` (for manpages)
- `pandoc` (for web docs)
- `mirage` and `quicksand` libraries installed

### Linux

```bash
make
make check
make install PREFIX=/usr/local
```

### macOS

Install toolchain and docs tools (example with Homebrew), then build:

```bash
brew install llvm scdoc pandoc
make -f Makefile.darwin
make -f Makefile.darwin check
make -f Makefile.darwin install PREFIX=/usr/local
```

### Windows

Recommended: use WSL and follow Linux steps.  
Alternative: MSYS2/MinGW can work, but support is less complete than Linux/macOS.

```bash
# in WSL
make
make check
make install PREFIX=/usr/local
```

### Build web docs

```bash
make webdocs
# or
ninja -f build.ninja webdocs
```
