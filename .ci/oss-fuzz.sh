#!/bin/bash -eu
# Copyright 2017 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

set -ex

apt-get install -y ninja-build
export CMAKE_GENERATOR=Ninja

RAWSPEED_SOURCE="$SRC/librawspeed/"
RAWSPEED_BUILD="$WORK/rawspeed"

ln -f -s /usr/local/bin/lld /usr/bin/ld

cd "$SRC"

LIBCXX_LLVM_VER="19.1.7"

wget -q https://github.com/llvm/llvm-project/releases/download/llvmorg-$LIBCXX_LLVM_VER/llvm-project-$LIBCXX_LLVM_VER.src.tar.xz
tar -xf llvm-project-$LIBCXX_LLVM_VER.src.tar.xz llvm-project-$LIBCXX_LLVM_VER.src/{runtimes,cmake,llvm/cmake,libcxx,libcxxabi}/
LIBCXX_LLVM_SOURCE="$SRC/llvm-project-$LIBCXX_LLVM_VER.src"

LIBCXX_BUILD="$WORK/llvm-project-$LIBCXX_LLVM_VER.libcxx.build"
cmake -S "$LIBCXX_LLVM_SOURCE/runtimes/" -B "$LIBCXX_BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DLLVM_INCLUDE_TESTS=OFF \
      -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
      -DLIBCXX_ENABLE_SHARED=OFF \
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
      -DLIBCXXABI_ENABLE_SHARED=OFF \
      -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
      -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
      -DLIBCXXABI_ADDITIONAL_COMPILE_FLAGS="-fno-sanitize=vptr"
cmake --build "$LIBCXX_BUILD" -- -j$(nproc) cxx cxxabi

CXXFLAGS="$CXXFLAGS -nostdinc++ -nostdlib++ -isystem $LIBCXX_BUILD/include -isystem $LIBCXX_BUILD/include/c++/v1 -L$LIBCXX_BUILD/lib -lc++ -lc++abi"

LIBOMP_LLVM_VER="20.1.5"

wget -q https://github.com/llvm/llvm-project/releases/download/llvmorg-$LIBOMP_LLVM_VER/llvm-project-$LIBOMP_LLVM_VER.src.tar.xz
tar -xf llvm-project-$LIBOMP_LLVM_VER.src.tar.xz llvm-project-$LIBOMP_LLVM_VER.src/{runtimes,cmake,llvm/cmake,openmp}/
OPENMP_LLVM_SOURCE="$SRC/llvm-project-$LIBOMP_LLVM_VER.src"

patch $OPENMP_LLVM_SOURCE/openmp/runtime/src/kmp.h $RAWSPEED_SOURCE/.ci/openmp.patch

OPENMP_BUILD="$WORK/llvm-project-$LIBOMP_LLVM_VER.omp.build"
cmake -S "$OPENMP_LLVM_SOURCE/openmp/" -B "$OPENMP_BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DLIBOMP_ENABLE_SHARED=OFF \
      -DOPENMP_ENABLE_LIBOMPTARGET=OFF \
      -DLIBOMP_CXXFLAGS="-fno-sanitize=undefined,integer"
cmake --build "$OPENMP_BUILD" -- -j$(nproc) omp

CXXFLAGS="$CXXFLAGS -isystem $OPENMP_BUILD/runtime/src -L$OPENMP_BUILD/runtime/src"

if [[ $SANITIZER = *undefined* ]]; then
  CFLAGS="$CFLAGS -fsanitize=unsigned-integer-overflow -fno-sanitize-recover=unsigned-integer-overflow"
  CXXFLAGS="$CXXFLAGS -fsanitize=unsigned-integer-overflow -fno-sanitize-recover=unsigned-integer-overflow"
fi

cmake -S "$RAWSPEED_SOURCE" -B "$RAWSPEED_BUILD" \
  -DBINARY_PACKAGE_BUILD=ON -DWITH_OPENMP=ON \
  -DWITH_PUGIXML=OFF -DUSE_XMLLINT=OFF -DWITH_JPEG=OFF -DWITH_ZLIB=OFF \
  -DBUILD_TESTING=OFF -DBUILD_TOOLS=OFF -DBUILD_BENCHMARKING=OFF \
  -DCMAKE_BUILD_TYPE=FUZZ -DBUILD_FUZZERS=ON \
  -DLIB_FUZZING_ENGINE:STRING="$LIB_FUZZING_ENGINE" \
  -DCMAKE_INSTALL_PREFIX:PATH="$OUT" -DCMAKE_INSTALL_BINDIR:PATH="$OUT"

cmake --build "$RAWSPEED_BUILD" -- -j$(nproc) all && cmake --build "$RAWSPEED_BUILD" -- -j$(nproc) install

du -hcs "$SRC"/* \
        "$WORK"/* \
        "$OUT"
