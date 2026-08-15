#!/usr/bin/env bash
# Build the Selene .deb inside a Debian container.
#
# The package must be built against the same Debian release the appliance is
# based on, so this never builds on the host -- the host is usually not Debian,
# and even when it is, its Qt/FFmpeg/VA-API sonames would not match the image.
#
#   ./scripts/build-deb.sh                 build for the default suite
#   MLOS_SUITE=trixie ./scripts/build-deb.sh
#
# The finished .deb lands in dist/.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUITE="${MLOS_SUITE:-trixie}"
IMAGE="selene-deb-builder:${SUITE}"

say() { printf '\033[1;35m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mError:\033[0m %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null || die "docker is required."
docker info >/dev/null 2>&1 || die "cannot talk to the docker daemon."

say "Preparing the ${SUITE} build container"
docker build -q --network host -t "$IMAGE" - >/dev/null <<-DOCKERFILE
	FROM debian:${SUITE}
	ENV DEBIAN_FRONTEND=noninteractive
	RUN apt-get update \
	 && apt-get install -y --no-install-recommends \
	        build-essential devscripts equivs dpkg-dev ca-certificates \
	 && rm -rf /var/lib/apt/lists/*
DOCKERFILE

mkdir -p "$HERE/dist"

# mk-build-deps installs exactly what debian/control asks for, so the
# dependency list has one source of truth rather than two.
say "Building the package"
docker run --rm --network host \
	-v "$HERE:/src" \
	-v "$HERE/dist:/dist" \
	-w /src \
	"$IMAGE" bash -euc '
		apt-get update
		mk-build-deps --install --remove \
			--tool "apt-get -y --no-install-recommends" debian/control
		# Build in a copy so the source tree keeps its submodules and stays clean.
		rm -rf /build && mkdir -p /build/selene
		tar -c --exclude=./dist --exclude=./.git . | tar -x -C /build/selene
		cd /build/selene
		dpkg-buildpackage -us -uc -b
		cp /build/*.deb /dist/
	'

say "Built:"
ls -1 "$HERE"/dist/*.deb
