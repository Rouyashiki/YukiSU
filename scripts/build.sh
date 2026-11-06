#!/usr/bin/env bash
# YukiSU local build: DDK LKMs -> ksuinit -> ksud -> Manager App
# Signing env: YUKISU_KEYSTORE, YUKISU_KEYSTORE_PASSWORD, YUKISU_KEY_ALIAS, YUKISU_KEY_PASSWORD
# Usage: ./scripts/build.sh [-k KMI] [--clean] [--yukizygisk|--yukizygisk-off] [--skip-lkm] [-i] [-h]
# Without --kmi, all supported LKM targets are built and embedded in ksud.
# --clean deletes Native CMake build directories before building.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$REPO_ROOT/out"
# Keep this list in sync with build-lkm.yml and ksud.yml.
SUPPORTED_KMIS=(
	android12-5.10
	android13-5.10
	android13-5.15
	android14-5.15
	android14-6.1
	android15-6.6
	android16-6.12
	android17-6.18
)
KMI_TARGETS=("${SUPPORTED_KMIS[@]}")
ANDROID_ABI="arm64-v8a"
CLEAN_BUILD=false
SKIP_LKM=false
DDK_RELEASE="20260828"
DO_INSTALL=false
ENABLE_YUKIZYGISK=true

while [[ $# -gt 0 ]]; do
	case "$1" in
	-k | --kmi)
		KMI_TARGETS=("$2")
		shift 2
		;;
	--clean)
		CLEAN_BUILD=true
		shift
		;;
	--skip-lkm)
		SKIP_LKM=true
		shift
		;;
	--yukizygisk)
		ENABLE_YUKIZYGISK=true
		shift
		;;
	--yukizygisk-off)
		ENABLE_YUKIZYGISK=false
		shift
		;;
	-i | --install)
		DO_INSTALL=true
		shift
		;;
	-h | --help)
		head -5 "$0" | tail -n +2 | sed 's/^# \?//'
		exit 0
		;;
	*)
		echo "Unknown option: $1"
		exit 1
		;;
	esac
done

for kmi in "${KMI_TARGETS[@]}"; do
	if [[ ! "$kmi" =~ ^[A-Za-z0-9._-]+$ ]]; then
		echo "Invalid KMI/DDK target: $kmi"
		exit 1
	fi
done

# Android API floor. CMake overrides the versioned NDK wrapper's --target with one
# it derives from CMAKE_SYSTEM_VERSION, so that variable -- not the wrapper path --
# is what actually selects the API level. Enforced at compile time by
# userspace/common/api_floor.hpp.
ANDROID_API=31
ANDROID_TARGET=aarch64-linux-android${ANDROID_API}

detect_ndk_host() {
	if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
		echo "ANDROID_NDK_HOME is required"
		exit 1
	fi
	local prebuilt="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt"
	if [[ -d "$prebuilt/darwin-x86_64" ]]; then
		echo "darwin-x86_64"
	elif [[ -d "$prebuilt/darwin-arm64" ]]; then
		echo "darwin-arm64"
	elif [[ -d "$prebuilt/linux-x86_64" ]]; then
		echo "linux-x86_64"
	else
		echo "Cannot detect NDK prebuilt toolchain"
		exit 1
	fi
}

detect_jobs() {
	if command -v nproc >/dev/null 2>&1; then
		nproc --all
	elif command -v getconf >/dev/null 2>&1; then
		getconf _NPROCESSORS_ONLN
	elif command -v sysctl >/dev/null 2>&1; then
		sysctl -n hw.logicalcpu
	else
		echo 8
	fi
}

prepare_build_dir() {
	local build_dir="$1"

	if [[ "$CLEAN_BUILD" == "true" ]]; then
		case "$build_dir" in
		"$REPO_ROOT"/*/build | "$REPO_ROOT"/*/build-*) ;;
		*)
			echo "Refusing to clean unsafe build directory: $build_dir"
			exit 1
			;;
		esac
		rm -rf -- "$build_dir"
	fi
	mkdir -p "$build_dir"
}

build_android_cmake() {
	local source_dir="$1"
	local build_dir="$2"
	local abi="$3"
	local api="$4"
	local target

	case "$abi" in
	arm64-v8a) target="aarch64-linux-android${api}" ;;
	armeabi-v7a) target="armv7a-linux-androideabi${api}" ;;
	*)
		echo "Unsupported Android ABI: $abi"
		return 1
		;;
	esac

	prepare_build_dir "$build_dir"
	cmake -S "$source_dir" -B "$build_dir" -G Ninja \
		-DCMAKE_SYSTEM_NAME=Android \
		-DCMAKE_ANDROID_ARCH_ABI="$abi" \
		-DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
		-DCMAKE_SYSTEM_VERSION="$api" \
		-DCMAKE_C_COMPILER="$TOOLCHAIN/bin/${target}-clang" \
		-DCMAKE_CXX_COMPILER="$TOOLCHAIN/bin/${target}-clang++" \
		-DCMAKE_AR="$TOOLCHAIN/bin/llvm-ar" \
		-DCMAKE_RANLIB="$TOOLCHAIN/bin/llvm-ranlib" \
		-DCMAKE_BUILD_TYPE=Release &&
		cmake --build "$build_dir" --parallel "$MAKE_JOBS"
}

NDK_HOST=$(detect_ndk_host)
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$NDK_HOST"
MAKE_JOBS=$(detect_jobs)

echo "=== YukiSU local build ==="
echo "KMIs: ${KMI_TARGETS[*]} | ABI: $ANDROID_ABI | NDK: $ANDROID_NDK_HOME"
if [[ "$CLEAN_BUILD" == "true" ]]; then
	echo "Native cache: clean rebuild"
else
	echo "Native cache: reuse build directories"
fi
echo ""

KSU_YUKIZYGISK_MAKE=""
if [[ "$ENABLE_YUKIZYGISK" == "true" ]]; then
	KSU_YUKIZYGISK_MAKE="CONFIG_KSU_YUKIZYGISK=y"
	echo "YukiZygisk kernel hooks: enabled"
else
	echo "YukiZygisk kernel hooks: disabled"
fi
echo ""

if [[ "$SKIP_LKM" != "true" ]]; then
	python3 "$REPO_ROOT/userspace/zygisk/inject/generate.py" --ndk "$ANDROID_NDK_HOME"
	echo ">>> [1/5] Build KernelSU LKMs (DDK) ..."
	mkdir -p "$OUT_DIR"
	for kmi in "${KMI_TARGETS[@]}"; do
		echo "    Building LKM: $kmi"
		docker run --platform linux/amd64 --rm -v "$REPO_ROOT:/src" -w /src \
			"ghcr.io/ylarod/ddk-min:${kmi}-${DDK_RELEASE}" \
			bash -c "cd kernel && test -f include/uapi/supercall.h && \
		             make clean && \
		             CONFIG_KSU=m CONFIG_KSU_SUPERKEY=y ${KSU_YUKIZYGISK_MAKE} CC=clang make -j${MAKE_JOBS} && \
		             mkdir -p /src/out && cp kernelsu.ko /src/out/${kmi}_kernelsu.ko && \
		             (llvm-strip -d /src/out/${kmi}_kernelsu.ko 2>/dev/null || true)"
		echo "    LKM: $OUT_DIR/${kmi}_kernelsu.ko"
	done
else
	echo ">>> [1/5] Skip LKM builds; reuse outputs"
fi

echo ">>> [2/5] Build ksuinit ..."
KSUINIT_DIR="$REPO_ROOT/userspace/ksuinit"
prepare_build_dir "$KSUINIT_DIR/build"
cd "$KSUINIT_DIR/build"

export CC="$TOOLCHAIN/bin/${ANDROID_TARGET}-clang"
export CXX="$TOOLCHAIN/bin/${ANDROID_TARGET}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"

cmake .. \
	-G Ninja \
	-DCMAKE_SYSTEM_NAME=Android \
	-DCMAKE_ANDROID_ARCH_ABI="$ANDROID_ABI" \
	-DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
	-DCMAKE_SYSTEM_VERSION="$ANDROID_API" \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_BUILD_TYPE=Release

ninja
echo "    ksuinit built"

echo ">>> [3/5] Build ksud ..."
KSUD_ASSETS="$REPO_ROOT/userspace/ksud/assets"
mkdir -p "$KSUD_ASSETS"
mkdir -p "$OUT_DIR"
find "$KSUD_ASSETS" -maxdepth 1 -type f -name '*.ko' -delete
rm -f -- "$KSUD_ASSETS/ksuinit" "$KSUD_ASSETS/su" \
	"$KSUD_ASSETS/zygiskd64" "$KSUD_ASSETS/zygiskd32" \
	"$KSUD_ASSETS/libzygisk64.so" "$KSUD_ASSETS/libzygisk32.so" \
	"$KSUD_ASSETS/libyloader64.so" "$KSUD_ASSETS/libyloader32.so" \
	"$KSUD_ASSETS/libyukizncore64.so" "$KSUD_ASSETS/libyukizncore32.so" \
	"$KSUD_ASSETS/libzygisk.so" "$KSUD_ASSETS/libyukilinker.so" \
	"$KSUD_ASSETS/libyukilinker32.so" "$KSUD_ASSETS/libyukilinker64.so" \
	"$KSUD_ASSETS/libyukizncore.so"

for kmi in "${KMI_TARGETS[@]}"; do
	lkm="$OUT_DIR/${kmi}_kernelsu.ko"
	if [[ ! -f "$lkm" ]]; then
		echo "Required LKM not found: $lkm"
		exit 1
	fi
	cp "$lkm" "$KSUD_ASSETS/"
done
echo "    staged LKMs: ${KMI_TARGETS[*]}"

cp "$KSUINIT_DIR/build/ksuinit" "$KSUD_ASSETS/"

# YukiZygisk payload.
echo ">>> Build YukiZygisk payload ..."
ZCORE_DIR="$REPO_ROOT/userspace/zygisk/core"
ZYGISKD_DIR="$REPO_ROOT/userspace/zygisk/daemon"
build_android_cmake "$ZCORE_DIR" "$ZCORE_DIR/build" arm64-v8a 31
build_android_cmake "$ZYGISKD_DIR" "$ZYGISKD_DIR/build" arm64-v8a 31
build_android_cmake "$ZCORE_DIR" "$ZCORE_DIR/build-armv7" armeabi-v7a 31
build_android_cmake "$ZYGISKD_DIR" "$ZYGISKD_DIR/build-armv7" armeabi-v7a 31
cp "$ZCORE_DIR/build/libzygisk64.so" "$KSUD_ASSETS/"
cp "$ZCORE_DIR/build/libyloader64.so" "$KSUD_ASSETS/"
cp "$ZCORE_DIR/build/libyukizncore64.so" "$KSUD_ASSETS/"
cp "$ZYGISKD_DIR/build/zygiskd64" "$KSUD_ASSETS/"
cp "$ZCORE_DIR/build-armv7/libzygisk32.so" "$KSUD_ASSETS/"
cp "$ZCORE_DIR/build-armv7/libyloader32.so" "$KSUD_ASSETS/"
cp "$ZCORE_DIR/build-armv7/libyukizncore32.so" "$KSUD_ASSETS/"
cp "$ZYGISKD_DIR/build-armv7/zygiskd32" "$KSUD_ASSETS/"
echo "    staged arm64/armv7 payloads + zygiskd64/zygiskd32"

KSUD_DIR="$REPO_ROOT/userspace/ksud"
prepare_build_dir "$KSUD_DIR/build"
cd "$KSUD_DIR/build"

cmake .. \
	-G Ninja \
	-DCMAKE_SYSTEM_NAME=Android \
	-DCMAKE_ANDROID_ARCH_ABI="$ANDROID_ABI" \
	-DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
	-DCMAKE_SYSTEM_VERSION="$ANDROID_API" \
	-DCMAKE_C_COMPILER="$CC" \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_BUILD_TYPE=Release

# ksuinit and the .ko assets are staged into assets/ before this configure.
ninja
echo "    ksud built"

echo ">>> [4/5] Build Manager App ..."
BUSYBOX_DIR="$REPO_ROOT/userspace/busybox"
prepare_build_dir "$BUSYBOX_DIR/build"
cmake -S "$BUSYBOX_DIR" -B "$BUSYBOX_DIR/build" -G Ninja \
	-DCMAKE_SYSTEM_NAME=Android \
	-DCMAKE_ANDROID_ARCH_ABI="$ANDROID_ABI" \
	-DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
	-DCMAKE_SYSTEM_VERSION="$ANDROID_API" \
	-DCMAKE_BUILD_TYPE=Release
cmake --build "$BUSYBOX_DIR/build"
MANAGER_DIR="$REPO_ROOT/manager"
JNILIBS="$MANAGER_DIR/app/src/main/jniLibs/$ANDROID_ABI"
mkdir -p "$JNILIBS"
cp "$KSUD_DIR/build/ksud" "$JNILIBS/libksud.so"
cp "$BUSYBOX_DIR/build/busybox" "$JNILIBS/libbusybox.so"

# Signing is passed through env, not gradle.properties.
if [[ -n "${YUKISU_KEYSTORE:-}" && -n "${YUKISU_KEYSTORE_PASSWORD:-}" && -n "${YUKISU_KEY_ALIAS:-}" && -n "${YUKISU_KEY_PASSWORD:-}" ]]; then
	export KEYSTORE_FILE="$YUKISU_KEYSTORE"
	export KEYSTORE_PASSWORD="$YUKISU_KEYSTORE_PASSWORD"
	export KEY_ALIAS="$YUKISU_KEY_ALIAS"
	export KEY_PASSWORD="$YUKISU_KEY_PASSWORD"
	export ORG_GRADLE_PROJECT_KEYSTORE_FILE="$YUKISU_KEYSTORE"
	export ORG_GRADLE_PROJECT_KEYSTORE_PASSWORD="$YUKISU_KEYSTORE_PASSWORD"
	export ORG_GRADLE_PROJECT_KEY_ALIAS="$YUKISU_KEY_ALIAS"
	export ORG_GRADLE_PROJECT_KEY_PASSWORD="$YUKISU_KEY_PASSWORD"
fi

cd "$MANAGER_DIR"
./gradlew assembleRelease --build-cache --no-daemon -PABI="$ANDROID_ABI"
echo "    APK built"

APK_DIR="$MANAGER_DIR/app/build/outputs/renamed_apk/release"
echo ""
echo "=== Build complete ==="
echo "APK: $APK_DIR"
ls -la "$APK_DIR"/*.apk 2>/dev/null || true
echo ""

if [[ "$DO_INSTALL" == "true" ]]; then
	apk_files=("$APK_DIR"/*.apk)
	APK_FILE=""
	if [[ ${#apk_files[@]} -gt 0 ]]; then
		APK_FILE="${apk_files[0]}"
	fi
	if [[ -n "$APK_FILE" ]]; then
		echo ">>> Install to device ..."
		adb install -r "$APK_FILE" && echo "APK installed" || echo "APK install failed"
		echo ""
		echo "ksud is embedded in the APK; sync it from the app before reboot."
	fi
else
	echo "Install: adb install -r $APK_DIR/*.apk"
	echo "Or: ./scripts/build.sh --skip-lkm -i"
	echo ""
	echo "After installing, sync ksud from the app before reboot."
fi
