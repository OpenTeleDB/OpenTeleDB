#!/bin/bash
workdir=$(pwd)
platform=$(uname -m)
package="ctg-pg-xproxy-${platform}-${PACKAGE_VERSION}.tar.gz"

BUILD_CMD="build_dbg"
if [[ "${BUILD_TARGET}" == "debug" ]]; then
      echo "Building debug version"
      BUILD_CMD="build_dbg"
else
      echo "Building release version"
      BUILD_CMD="build_release"
fi


declare -a exclusion=("linux-vdso.so" "librt.so" "libdl.so" "libm.so" "libpthread.so" "libc.so")

isExclude() {
      for i in "${exclusion[@]}"; do
            if [[ "$1" == *"$i"* ]]; then
                return 0
            fi
      done
      return 1
}

if [ -e /usr/include/openssl11 ] && [ -e /usr/lib64/openssl11/libcrypto.so ]; then
      OPENSSL_INCLUDE_DIR=/usr/include/openssl11
      OPENSSL_LIBRARIES='/usr/lib64/openssl11/libssl.so;/usr/lib64/openssl11/libcrypto.so'
      OPENSSL_VERSION=11 make  ${BUILD_CMD};
else
      make ${BUILD_CMD};
fi

mkdir xproxy xproxy/bin xproxy/etc xproxy/lib
cp build/sources/odyssey xproxy/bin/xproxy
cp scripts/tools/* xproxy/bin/
cp third_party/machinarium/gdb/machinarium-gdb.py xproxy/bin
cp odyssey.conf xproxy/etc/odyssey-template.conf
chmod +x xproxy/bin/*


for dep in $(ldd build/sources/odyssey |grep -oP '/.*\.so\S*'); do 
      if [ ! -f $dep ]; then
            continue
      fi
      if isExclude $dep; then
            echo "skip $dep";
            continue
      fi
      cp -v $dep xproxy/lib;
done

tar zcf $package xproxy
ls -l $package
