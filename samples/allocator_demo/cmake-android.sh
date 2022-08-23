#!/bin/bash

SCRIPT_DIR=$(cd $(dirname ${BASH_SOURCE[0]}); pwd)
SAMPLES_DIR=${SCRIPT_DIR}/..
LIBRGA_PATH=${SAMPLES_DIR}/../build_android_ndk/install/lib
BUILD_DIR=build/build_android_ndk
BUILD_TYPE=Release	

# Modify to the local toolchain path.
TOOLCHAIN_PATH=../../toolchains/toolchain_android_ndk.cmake

rm -rf $BUILD_DIR
mkdir -p $BUILD_DIR
pushd $BUILD_DIR
	
cmake ../.. \
	-DLIBRGA_FILE_LIB=${LIBRGA_PATH} \
	-DBUILD_TOOLCHAINS_PATH=${TOOLCHAIN_PATH} \
	-DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
	-DCMAKE_INSTALL_PREFIX=install \

make -j8
make install

popd
