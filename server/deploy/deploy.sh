#!/bin/sh
# Compila anemometer-collector e lo installa su un server Devuan via SSH.
#   deploy/deploy.sh [-a amd64|arm64|armv7] [-p porta_ssh] utente@server
# L'utente deve essere root oppure poter usare sudo.
set -eu

ARCH=amd64
SSH_PORT=22
while getopts "a:p:" opt; do
	case "$opt" in
		a) ARCH=$OPTARG ;;
		p) SSH_PORT=$OPTARG ;;
		*) sed -n '2,4p' "$0" >&2; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
if [ $# -ne 1 ]; then
	sed -n '2,4p' "$0" >&2
	exit 2
fi
TARGET=$1

case "$ARCH" in
	armv7) GOARCH=arm GOARM=7 ;;
	*) GOARCH=$ARCH GOARM= ;;
esac

DEPLOY_DIR=$(cd "$(dirname "$0")" && pwd)
SERVER_DIR=$(dirname "$DEPLOY_DIR")
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

NAME=anemometer-collector

echo "==> Compilazione per linux/$ARCH"
# Statico: non dipende dalla versione di glibc del server
(cd "$SERVER_DIR" && CGO_ENABLED=0 GOOS=linux GOARCH=$GOARCH GOARM=$GOARM \
	go build -trimpath -ldflags="-s -w" -o "$BUILD_DIR/$NAME" .)

cp "$DEPLOY_DIR/$NAME.init" "$DEPLOY_DIR/$NAME.default" "$DEPLOY_DIR/$NAME.logrotate" \
	"$DEPLOY_DIR/install.sh" "$BUILD_DIR/"

echo "==> Copia su $TARGET"
REMOTE_DIR=$(ssh -p "$SSH_PORT" "$TARGET" 'mktemp -d /tmp/anemometer-deploy.XXXXXX')
tar -C "$BUILD_DIR" -cf - . | ssh -p "$SSH_PORT" "$TARGET" "tar -C '$REMOTE_DIR' -xf -"

echo "==> Installazione"
# -t: sudo potrebbe chiedere la password
ssh -t -p "$SSH_PORT" "$TARGET" "
	cd '$REMOTE_DIR'
	if [ \"\$(id -u)\" -eq 0 ]; then SUDO=; else SUDO=sudo; fi
	\$SUDO sh ./install.sh
	status=\$?
	cd /
	rm -rf '$REMOTE_DIR'
	exit \$status
"

echo "==> Fatto. Ricorda di aprire la porta UDP 5683 sul firewall del server."
