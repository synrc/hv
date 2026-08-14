cd third_party/libressl-3.1.5

./configure \
  --prefix=$PWD/install-host \
  --disable-shared

make -j$(sysctl -n hw.ncpu)
make install

# clean previous build
make distclean 2>/dev/null || true

export CC=aarch64-linux-musl-gcc
export CFLAGS="-static -O2 -fPIC"
export LDFLAGS="-static"

./configure \
  --host=aarch64-linux-musl \
  --prefix=$PWD/install-aarch64 \
  --disable-shared \
  --disable-asm          # often safer on musl/seL4

make -j$(sysctl -n hw.ncpu)
make install

