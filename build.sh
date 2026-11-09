#!/bin/bash
# Build BBK from this checkout and package BBK.bar.
#
# Runs in WSL.  Sources are copied to the directory the bbndk container has
# mounted, built there, and the resulting package is copied back here.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=/root/q30-term50-kit/src/BBK
CONTAINER=bbndk-term50

if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    echo "build: 容器 $CONTAINER 没有在运行" >&2
    echo "build: 重建命令见 WSL 里的 docker run（--network host -v /root/q30-term50-kit:/workspace）" >&2
    exit 1
fi

# Sync the checkout into the mounted build directory.  Deleted files are
# removed too, so a source that leaves the repo also leaves the build.
mkdir -p "$BUILD" "$BUILD/assets"
cp -f "$HERE"/*.cpp "$HERE"/*.h "$HERE/BBK.pro" "$HERE/bar-descriptor.xml" "$BUILD/"
rm -f "$BUILD/assets"/*.png
cp -f "$HERE"/assets/*.png "$BUILD/assets/" 2>/dev/null || true

docker exec --user root -e LD_LIBRARY_PATH=/workspace/src/Term50/agentchat/dev/deps \
    "$CONTAINER" bash -c '
set -e
export QNX_HOST=/opt/bbndk/host_10_3_1_12/linux/x86
export QNX_TARGET=/opt/bbndk/target_10_3_1_995/qnx6
export PATH=$QNX_HOST/usr/bin:$PATH
export QMAKESPEC=unsupported/blackberry-armv7le-qcc
export JAVA_TOOL_OPTIONS="-Djava.security.manager=allow \
--add-exports=java.xml/com.sun.org.apache.xerces.internal.parsers=ALL-UNNAMED \
--add-exports=java.xml/com.sun.org.apache.xerces.internal.dom=ALL-UNNAMED \
--add-exports=java.xml/com.sun.org.apache.xerces.internal.jaxp=ALL-UNNAMED \
--add-exports=java.xml/com.sun.org.apache.xerces.internal.util=ALL-UNNAMED \
--add-opens=java.xml/com.sun.org.apache.xerces.internal.parsers=ALL-UNNAMED \
--add-opens=java.xml/com.sun.org.apache.xerces.internal.dom=ALL-UNNAMED"
cd /workspace/src/BBK
qmake -spec $QMAKESPEC -o arm/Makefile BBK.pro CONFIG+=device CONFIG+=release >/dev/null
make -C arm -f Makefile release -j4
rm -f BBK.bar
blackberry-nativepackager -devMode -package BBK.bar bar-descriptor.xml \
    -configuration Device-Release
'

cp -f "$BUILD/BBK.bar" "$HERE/BBK.bar"
echo "build: $HERE/BBK.bar ($(stat -c %s "$HERE/BBK.bar") bytes)"
