#!/bin/sh
set -e

if [ -d ../rose ]; then
    rose=../rose
elif [ -d ./rose ]; then
    rose=./rose
else
    rose="$HOME/rose"
fi

if [ ! -d "$rose" ]; then
    mkdir -p "$rose"
fi
cd "$rose"

for repo in mirage quicksand ros3; do
    url="https://github.com/ravengraves/$repo.git"
    if [ -d "$repo" ]; then
        echo "Upgrading $repo"
        git -C "$repo" pull "$url"
    else
        git clone "$url" "$repo"
    fi
    make -C "$repo"
    sudo make -C "$repo" install
    make -C "$repo" python
done

echo "Done"
