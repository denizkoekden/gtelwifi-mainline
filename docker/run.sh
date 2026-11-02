#!/bin/bash
# Run a command in the gtelwifi build container.
#   /work = this project (bind mount), /src = named volume with the kernel tree.
# Usage: docker/run.sh <cmd...>     e.g. docker/run.sh ./buildkit/build.sh /src/linux
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=gtelwifi-build
VOLUME=gtelwifi-linux

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	docker build -t "$IMAGE" "$ROOT/docker"
fi
docker volume inspect "$VOLUME" >/dev/null 2>&1 || docker volume create "$VOLUME" >/dev/null

TTY=-i		# keep stdin open, e.g. for: docker export … | docker/run.sh tar x …
[ -t 0 ] && TTY=-it
exec docker run --rm $TTY -v "$ROOT:/work" -v "$VOLUME:/src" -w /work "$IMAGE" "$@"
