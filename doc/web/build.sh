#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
REPO="$(CDPATH= cd -- "$ROOT/../.." && pwd)"
PAGES_DIR="$ROOT/pages"
OUT_DIR="$REPO/build/doc/web"
FRAG_DIR="$OUT_DIR/.fragments"
TEMPLATE="$ROOT/index.html"
OUT_FILE="$OUT_DIR/index.html"

require_cmd() {
	command -v "$1" >/dev/null 2>&1 || {
		echo "missing required command: $1" >&2
		exit 1
	}
}

require_cmd pandoc
require_cmd sed
require_cmd mktemp

mkdir -p "$OUT_DIR" "$FRAG_DIR"
cp "$ROOT/logo.svg" "$OUT_DIR/logo.svg"
cp "$TEMPLATE" "$OUT_FILE"

render_page() {
	key="$1"
	md="$2"
	pandoc -f gfm -t html5 "$PAGES_DIR/$md" -o "$FRAG_DIR/$key.html"
}

replace_block() {
	key="$1"
	frag="$FRAG_DIR/$key.html"
	start="<!-- PAGE:${key}:start -->"
	end="<!-- PAGE:${key}:end -->"

	grep -q "$start" "$OUT_FILE" || { echo "missing marker: $start" >&2; exit 1; }
	grep -q "$end" "$OUT_FILE" || { echo "missing marker: $end" >&2; exit 1; }

	tmp_file="$(mktemp "$OUT_DIR/.index.XXXXXX")"
	sed "/$start/,/$end/{
/$start/{p
r $frag
}
/$end/p
d
}" "$OUT_FILE" > "$tmp_file"
	mv "$tmp_file" "$OUT_FILE"
}

render_page build-install 00-build-install.md
render_page project-setup 01-project-setup.md
render_page pubsub-c 02-pubsub-c.md
render_page pubsub-cpp 03-pubsub-cpp.md
render_page pubsub-python 04-pubsub-python.md
render_page tools 05-tools.md
render_page roseserial 06-roseserial.md
render_page multi-machine 07-multi-machine.md
render_page mirage-datatypes 08-mirage-datatypes.md
render_page package-control-subtree 09-package-control-subtree.md

replace_block build-install
replace_block project-setup
replace_block pubsub-c
replace_block pubsub-cpp
replace_block pubsub-python
replace_block tools
replace_block roseserial
replace_block multi-machine
replace_block mirage-datatypes
replace_block package-control-subtree

echo "generated $OUT_FILE"
