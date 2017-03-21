#!/bin/bash
# simple bash script for executing build

RDIR=$(pwd)

TOOLCHAIN=~/toolchain

THREADS=5

[ "$VER" ] ||
# version number
VER=$(cat "$RDIR/VERSION")

export ARCH=arm
export CROSS_COMPILE=$TOOLCHAIN/bin/arm-linux-gnueabihf-

cd "$RDIR"

[ "$DEVICE" ] || DEVICE=surnia
[ "$TARGET" ] || TARGET=nethunter
DEFCONFIG=${TARGET}_${DEVICE}_defconfig

[ -f "$RDIR/arch/$ARCH/configs/${DEFCONFIG}" ] || {
	echo "Config $DEFCONFIG not found in $ARCH configs!"
	exit 1
}

KDIR=$RDIR/build/arch/arm/boot
export LOCALVERSION="$TARGET-$DEVICE-$VER"

ABORT()
{
	echo "Error: $*"
	exit 1
}

CLEAN_BUILD()
{
	echo "Cleaning build..."
	cd "$RDIR"
	rm -rf build
}

SETUP_BUILD()
{
	echo "Creating kernel config..."
	cd "$RDIR"
	mkdir -p build
	make -C "$RDIR" O=build "$DEFCONFIG" || ABORT "Failed to set up build"
}

BUILD_KERNEL()
{
	echo "Starting build for $LOCALVERSION..."
	while ! make -C "$RDIR" O=build -j"$THREADS" CONFIG_NO_ERROR_ON_MISMATCH=y; do
		read -p "Build failed. Retry? " do_retry
		case $do_retry in
			Y|y) continue ;;
			*) return 1 ;;
		esac
	done
}

BUILD_DTB()
{
	echo "Generating dtb.img..."
	"$RDIR/scripts/dtbTool/dtbTool" -o "$KDIR/dtb.img" "$KDIR/dts/" -s 2048 || ABORT "Failed to generate dtb.img!"
}

CLEAN_BUILD && SETUP_BUILD && BUILD_KERNEL && BUILD_DTB && echo "Finished building $LOCALVERSION!"
