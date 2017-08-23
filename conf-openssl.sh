#!/bin/bash -ex

make distclean || true
./config --prefix=$HOME/openssl-1.1 --openssldir=$HOME/openssl-1.1/etc --with-rand-seed=rdcpu enable-aria enable-ec_nistp_64_gcc_128 enable-md2 enable-rc5 enable-weak-ssl-ciphers enable-zlib-dynamic enable-tls1_3 enable-tls13downgrade
make depend && make clean && make -j 2 all && make test && make install

