# UntouchFuzz

## Environment

* **LLVM-11** （**Required** or pass may be built failed）

* Already tested under **Ubuntu 20.04**

## Configuration

To be noticed that you should modify `LLVM_CONFIG`, `CC`, `CXX` in `fuzzer/llvm_mode/[Makefile,afl-clang-fast.c]` and `fuzzer/llvm_mode_orig/[Makefile,afl-clang-fast.c]` to your own compiler before using `UntouchFuzz`.

## Build

```bash
cd fuzzer
make
cd llvm_mode
make
cd ../llvm_mode_orig
make

# Example for build xpdf 4.04
cd ..
wget https://dl.xpdfreader.com/xpdf-4.04.tar.gz
tar -xzvf xpdf-4.04.tar.gz
cp -r xpdf-4.04 xpdf-4.04-untouch
cd xpdf-4.04
mkdir build && cd build
CC=~/fuzzer/afl-clang-fast-orig CXX=~/fuzzer/afl-clang-fast++-orig cmake ..
AFL_USE_ASAN=1 make -j4

cd ../../xpdf-4.04-untouch
mkdir build && cd build
CC=~/fuzzer/afl-clang-fast CXX=~/fuzzer/afl-clang-fast++ cmake ..
AFL_USE_ASAN=1 make -j4
cd ../../

# use UntouchFuzz to fuzz!
./afl-fuzz -m none -i ./testcases/others/pdf/ -o out -d -r ./xpdf-4.04-untouch/build/xpdf/pdftotext ./xpdf-4.04/build/xpdf/pdftotext @@ /dev/null

# just enjoy it~
```

