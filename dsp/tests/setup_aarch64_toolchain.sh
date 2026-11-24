#!/bin/bash
# Recreates the rootless ARM64 cross toolchain used to test the NEON backend under qemu:
# clang (host) + Ubuntu arm64 cross sysroot packages + lld, unpacked locally (no sudo needed).
# Usage: tests/setup_aarch64_toolchain.sh [dir]   -> creates <dir>/a64c++ (static aarch64 binaries)
# Then:  <dir>/a64c++ -O2 -std=c++17 pw_cli.cpp -o pw_cli_a64 && qemu-aarch64 ./pw_cli_a64 ...
set -e
D=${1:-/tmp/opencode/a64}
GCCV=15; LLVMV=22
mkdir -p "$D" && cd "$D"
apt-get download libc6-arm64-cross libc6-dev-arm64-cross libgcc-$GCCV-dev-arm64-cross libgcc-s1-arm64-cross \
    libstdc++-$GCCV-dev-arm64-cross libstdc++6-arm64-cross linux-libc-dev-arm64-cross lld-$LLVMV liblld-$LLVMV
for d in *arm64-cross*.deb; do dpkg-deb -x "$d" root; done
for d in lld-$LLVMV*.deb liblld-$LLVMV*.deb; do dpkg-deb -x "$d" lld; done
cat > a64c++ <<EOF
#!/bin/bash
R=$D/root
export LD_LIBRARY_PATH=$D/lld/usr/lib/llvm-$LLVMV/lib:$D/lld/usr/lib/x86_64-linux-gnu
exec clang++ --target=aarch64-linux-gnu --sysroot=\$R -isystem \$R/usr/aarch64-linux-gnu/include/c++/$GCCV \\
  -isystem \$R/usr/aarch64-linux-gnu/include/c++/$GCCV/aarch64-linux-gnu -isystem \$R/usr/aarch64-linux-gnu/include \\
  -B \$R/usr/lib/gcc-cross/aarch64-linux-gnu/$GCCV -L \$R/usr/lib/gcc-cross/aarch64-linux-gnu/$GCCV -L \$R/usr/aarch64-linux-gnu/lib \\
  --ld-path=$D/lld/usr/lib/llvm-$LLVMV/bin/ld.lld -static "\$@"
EOF
chmod +x a64c++
echo "toolchain ready: $D/a64c++"
