#!/usr/bin/env bash
# Uploads a build to a console over FTP through Docker (see Dockerfile.deploy). Leaves no image behind.
#
#   ./deploy.sh [folder]       folder holds n3ds-cast.3dsx and/or n3ds-cast.cia (default: out)
#
# The console is 3ds-ftpd.local, port 5000. Override with the N3DS_FTP_HOST and N3DS_FTP_PORT environment variables.
set -euo pipefail
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1 # Git Bash on Windows: do not rewrite the container path

folder="$(cd "${1:-out}" && { pwd -W 2>/dev/null || pwd; })"
host="${N3DS_FTP_HOST:-3ds-ftpd.local}"
# Docker on Linux does not resolve mDNS names (*.local) inside the container: resolve it here, on the host.
if command -v getent >/dev/null 2>&1; then
    address="$(getent hosts "$host" | awk '{ print $1; exit }' || true)"
    host="${address:-$host}"
fi

tag="n3ds-cast-deploy:tmp"
trap 'docker rmi -f "$tag" >/dev/null 2>&1 || true' EXIT
docker build -q -f Dockerfile.deploy -t "$tag" . >/dev/null
docker run --rm -v "$folder:/build" -e N3DS_FTP_HOST="$host" -e N3DS_FTP_PORT="${N3DS_FTP_PORT:-5000}" "$tag"
