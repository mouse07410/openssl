#!/bin/bash -ex

make distclean || true
./config --prefix=$HOME/openssl-1.1 --openssldir=$HOME/openssl-1.1/etc enable-aria enable-ec_nistp_64_gcc_128 enable-md2 enable-rc5 enable-weak-ssl-ciphers enable-zlib-dynamic enable-tls1_3 enable-tls13downgrade
make depend && make clean && make -j 4 all && make test

