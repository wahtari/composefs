#!/bin/bash

set -e -x

cd /src

# Recent git complains that the directory is owned by someone else,
# which happens if we run it via a Dockefile with a volume mounted.
git config --global --add safe.directory "$(pwd)"

make -C hacking/clang-format clang-format
git diff --ignore-submodules --exit-code
