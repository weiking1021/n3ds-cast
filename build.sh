#!/usr/bin/env bash
# Builds with Docker only (see Dockerfile.build). Leaves no image behind.
#
#   ./build.sh                 build the .cia and .3dsx into out/
#   ./build.sh test            run the test suite
#   ./build.sh format-check    fail if a C file is not clang-formatted
#   ./build.sh formatted       reformat the C files in place
set -euo pipefail
cd "$(dirname "$0")"

target="${1:-out}"
case "$target" in
    out)
        docker build -f Dockerfile.build --target out --output type=local,dest=out .
        ;;
    formatted)
        docker build -f Dockerfile.build --target formatted --output type=local,dest=. .
        ;;
    test | format-check)
        # These targets end in an image; tag it so that it can be removed again.
        tag="n3ds-cast-$target:tmp"
        trap 'docker rmi -f "$tag" >/dev/null 2>&1 || true' EXIT
        docker build -f Dockerfile.build --target "$target" -t "$tag" .
        ;;
    *)
        echo "usage: $0 [out|test|format-check|formatted]" >&2
        exit 2
        ;;
esac
