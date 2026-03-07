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

Use MinGW + Ninja with the Windows build graph.

```bash
# Install toolchain (MSYS2 example)
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils ninja

# Build/install dependencies first
cd ../mirage && ninja -f build_windows.ninja install
cd ../quicksand && ninja -f build_windows.ninja install
cd ../ros3

# Point ros3 build at dependency install roots
export CFLAGS="-I../mirage/install/windows/include -I../quicksand/install/windows/include"
export LDFLAGS="-L../mirage/install/windows/lib -L../quicksand/install/windows/lib"

# Build/install ros3 Windows binaries
ninja -f build_windows.ninja all
ninja -f build_windows.ninja install
```

### Build web docs

```bash
make webdocs
# or
ninja -f build.ninja webdocs
```
