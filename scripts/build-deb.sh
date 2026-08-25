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
MSQUIC_VERSION="2.5.9"
MSQUIC_SHA256="1baa61ade0b7b4a99f6dcb6b00d9aedb12b5566d00918a325be7425e878e51ba"
MSQUIC_URL="https://packages.microsoft.com/debian/13/prod/pool/main/libm/libmsquic/libmsquic_${MSQUIC_VERSION}_amd64.deb"
MSQUIC_HEADER_BASE="https://raw.githubusercontent.com/microsoft/msquic/v${MSQUIC_VERSION}"

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
	        build-essential devscripts equivs dpkg-dev ca-certificates curl \
	 && rm -rf /var/lib/apt/lists/*
DOCKERFILE

mkdir -p "$HERE/dist"

# mk-build-deps installs exactly what debian/control asks for, so the
# dependency list has one source of truth rather than two.
say "Building the package"
docker run --rm --network host \
	-e TELEMETRY_URL="${TELEMETRY_URL:-}" \
	-e MSQUIC_SHA256="$MSQUIC_SHA256" \
	-e MSQUIC_URL="$MSQUIC_URL" \
	-e MSQUIC_HEADER_BASE="$MSQUIC_HEADER_BASE" \
	-v "$HERE:/src" \
	-v "$HERE/dist:/dist" \
	-w /src \
	"$IMAGE" bash -euc '
		apt-get update
		curl -fL --retry 3 -o /tmp/libmsquic.deb "$MSQUIC_URL"
		echo "$MSQUIC_SHA256  /tmp/libmsquic.deb" | sha256sum -c -
		apt-get install -y --no-install-recommends /tmp/libmsquic.deb
		mk-build-deps --install --remove \
			--tool "apt-get -y --no-install-recommends" debian/control
		# Build in a copy so the source tree keeps its submodules and stays clean.
		rm -rf /build && mkdir -p /build/selene
		tar -c --exclude=./dist --exclude=./.git . | tar -x -C /build/selene
		# libs/ is intentionally ignored because the desktop build downloads a
		# large prebuilt dependency bundle there. The Debian build only needs
		# these three public headers, so fetch and pin the matching upstream
		# sources rather than depending on an ignored developer-machine copy.
		mkdir -p /build/selene/libs/msquic/include
		for spec in \
			"src/inc/msquic.h:c9abfdd02c45910649dd335d6bd82718e4ddd2fdb35fe550567c78f032551e0c" \
			"src/inc/msquic_posix.h:b285fa66b9c9bdc886c30ef92910da472692b25f5c6192416fb40f08f64e22ec" \
			"src/inc/quic_sal_stub.h:9b13328d9aec8807a754b2bc391b31b5d09b1c5f6cec064012051683ed169055"; do
			path="${spec%%:*}"
			hash="${spec#*:}"
			name="${path##*/}"
			dest="/build/selene/libs/msquic/include/$name"
			curl -fL --retry 3 -o "$dest" "$MSQUIC_HEADER_BASE/$path"
			echo "$hash  $dest" | sha256sum -c -
		done
		cd /build/selene
		dpkg-buildpackage -us -uc -b
		cp /build/*.deb /dist/
	'

say "Built:"
ls -1 "$HERE"/dist/*.deb
