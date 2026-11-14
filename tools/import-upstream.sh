#!/bin/sh
# Imports the Chromium release named in CHROMIUM_VERSION.
#
# The branch has this shape:
#
#   Import chromium-A  <-  Import chromium-B  <-  patch 1  <-  ...  <-  patch N
#
# This script adds "Import chromium-C" as a child of the last import commit
# and replays the patches on top of it with git rebase. The existing import
# commits are never rewritten.
#
# The import consists of
#   - what tools/include.txt selects, minus what tools/exclude.txt matches, and
#   - what tools/include-unfiltered.txt selects, complete.
#
# The tarball is read from $CHROMIUM_TARBALL or /tmp/chromium-C.tar.xz if that
# file exists, and is downloaded otherwise.
#
# Commit the new CHROMIUM_VERSION before running this. Needs GNU tar.
set -ex
have_version=$(cut -d= -f2 src/chrome/VERSION | tr '\n' . | cut -d. -f1-4)
want_version=$(cat CHROMIUM_VERSION)
if [ "$have_version" = "$want_version" ]; then
  exit 0
fi
name="chromium-$want_version"
tarball="${CHROMIUM_TARBALL:-/tmp/$name.tar.xz}"
url="https://commondatastorage.googleapis.com/chromium-browser-official/$name.tar.xz"
branch=$(git branch --show-current)
if [ ! "$branch" ]; then
  echo "Not on a branch" >&2
  exit 1
fi
if ! git diff --quiet HEAD; then
  echo "There are uncommitted changes" >&2
  exit 1
fi
base=$(git log --first-parent --format='%H %s' HEAD |
  grep -m1 ' Import chromium-[0-9.]*$' | cut -d' ' -f1)
if [ ! "$base" ]; then
  echo "No 'Import chromium-*' commit found in the history of $branch" >&2
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed "s/^\^/$name\//" tools/include.txt >"$tmp/include.txt"
sed "s/^\^/$name\//" tools/include-unfiltered.txt >"$tmp/include-unfiltered.txt"
cp tools/exclude.txt "$tmp/exclude.txt"

git config core.autocrlf false
git config core.safecrlf false
git -c advice.detachedHead=false checkout "$base"
rm -rf src "$name"

# The archive is read once and fed to two extractions, because the exclusions
# of tar apply to everything it extracts.
mkdir "$tmp/unfiltered"
mkfifo "$tmp/fifo"
tar xJf "$tmp/fifo" -C "$tmp/unfiltered" --wildcards --wildcards-match-slash \
  -T "$tmp/include-unfiltered.txt" &
unfiltered_pid=$!
if [ -f "$tarball" ]; then
  cat "$tarball"
else
  curl -fL "$url" -o-
fi | tee "$tmp/fifo" |
  tar xJf - --wildcards --wildcards-match-slash \
    -T "$tmp/include.txt" -X "$tmp/exclude.txt"
wait "$unfiltered_pid"
cp -a "$tmp/unfiltered/$name/." "$name/"

mv "$name" src
git add --all src
git commit --quiet -m "Import $name"
git rebase --onto HEAD "$base" "$branch"
