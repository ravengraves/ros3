## Build and Installation

### Repositories

- `ros3`: <https://git.sr.ht/~alecgraves/ros3>
- `mirage`: <https://git.sr.ht/~alecgraves/mirage>
- `quicksand`: <https://git.sr.ht/~alecgraves/quicksand>

### Requirements

- `clang` or `gcc`
- `make` or `ninja`
- `scdoc` [(source)](https://git.sr.ht/~sircmpwn/scdoc) (for manpages)
- `pandoc` (for web docs)

### Linux (e.g. Debian)

install scdoc (for docs) and build deps:
```
sudo apt install scdoc git build-essential llvm clang clang-tidy clang-format
```

**1. Install mirage:**

```sh
git clone https://git.sr.ht/~alecgraves/mirage
cd mirage
make
make check
make docs
```

Install (as root):
```
# make install install-docs
```

Install mirage Python package:
```sh
make python
```

Go back to source directory:
```
cd ..
```

**2. Install quicksand:**

```sh
git clone https://git.sr.ht/~alecgraves/quicksand
cd quicksand
make
make check
make docs
```

Install (as root):
```
# make install install-docs
```

Install quicksand Python package:
```sh
make python
```

Go back to source directory:
```
cd ..
```


**3. Install ros3 🌹:**
```sh
git clone https://git.sr.ht/~alecgraves/ros3
cd ros3
make
make check
make docs
```

Install (as root):
```
# make install install-docs
```

Install ros3 Python package:
```sh
make python
make check-python
```

Webdocs (this guide):
```
make docs
```

*Congrats, you have installed ros3!*


### macOS

Use the Darwin makefile for `ros3` and `quicksand`. `mirage` currently uses its regular `Makefile`.

Install Xcode Command Line Tools first:

```bash
xcode-select --install
```

Optional tools:

- `scdoc` is needed for `make docs`
- `pandoc` is needed for `make webdocs`
- the full `make -f Makefile.darwin check` target also expects working `clang-tidy` and `gcc` executables on `PATH`

Example with Homebrew:

```bash
brew install scdoc pandoc llvm gcc
```

If your package manager installs versioned binary names instead of plain `clang-tidy` or `gcc`, set `CLANG_TIDY=...` and `GCC_ANALYZER=...` when running the full `check` targets.

Pick one install prefix and use it consistently for `mirage`, `quicksand`, and `ros3`.

- Apple Silicon Homebrew: `PREFIX=/opt/homebrew`
- Intel Homebrew or a manual `/usr/local` install: `PREFIX=/usr/local`

Example:

```bash
export PREFIX=/opt/homebrew
```

**1. Install mirage**

```bash
git clone https://git.sr.ht/~alecgraves/mirage
cd mirage
make PREFIX="$PREFIX"
make PREFIX="$PREFIX" check-runtime
sudo make PREFIX="$PREFIX" install
make PREFIX="$PREFIX" docs
sudo make PREFIX="$PREFIX" install-docs
cd ..
```

**2. Install quicksand**

```bash
git clone https://git.sr.ht/~alecgraves/quicksand
cd quicksand
make -f Makefile.darwin PREFIX="$PREFIX"
make -f Makefile.darwin PREFIX="$PREFIX" check
sudo make -f Makefile.darwin PREFIX="$PREFIX" install
make -f Makefile.darwin PREFIX="$PREFIX" docs
sudo make -f Makefile.darwin PREFIX="$PREFIX" install-docs
cd ..
```

**3. Install ros3**

```bash
git clone https://git.sr.ht/~alecgraves/ros3
cd ros3
make -f Makefile.darwin PREFIX="$PREFIX"
make -f Makefile.darwin PREFIX="$PREFIX" check-runtime
sudo make -f Makefile.darwin PREFIX="$PREFIX" install
make -f Makefile.darwin PREFIX="$PREFIX" docs
sudo make -f Makefile.darwin PREFIX="$PREFIX" install-docs
```

Use the full analysis + sanitizer suite only if `clang-tidy` and `gcc` are configured and the dependency repos are also present as siblings at `../mirage` and `../quicksand` (or passed via `MIRAGE_DIR=... QUICKSAND_DIR=...`):

```bash
make -f Makefile.darwin PREFIX="$PREFIX" check
```

`mirage` also has a heavier `make check` target, but it similarly expects extra analysis tooling on `PATH`.

Python bindings:

```bash
make -f Makefile.darwin PREFIX="$PREFIX" python
make -f Makefile.darwin PREFIX="$PREFIX" check-python
```

Current limitation: `lang/python/setup.py` still hardcodes `/usr/local/include` for dependency headers. If you install dependencies somewhere else, the Python build may fail until that setup script is made prefix-aware.

### Windows

Use wsl for now.

(TODO Alec, 2026): add native windows support

### Build web docs

```bash
make webdocs
# or
ninja webdocs
```
