#!/bin/bash -ex

unset OPENSSL_CFLAGS
unset OPENSSL_LIBS
unset OPENSSL

CFLAGS="$CFLAGS -g"

make distclean || true

# For OpenSSL-3 master (development branch)
./config --prefix=$HOME/openssl-3 --debug --openssldir=$HOME/openssl-3/etc --with-rand-seed=rdcpu enable-aria enable-ec_nistp_64_gcc_128 enable-md2 enable-rc5 enable-weak-ssl-ciphers enable-zlib-dynamic enable-ssl-trace
#enable-tls1_3 enable-tls13downgrade

# For OpenSSL-1.1.1-stable
#./config --debug --prefix=$HOME/openssl-1.1 --openssldir=$HOME/openssl-1.1/etc enable-ec_nistp_64_gcc_128 enable-md2 enable-rc5 enable-weak-ssl-ciphers enable-zlib-dynamic enable-tls1_3 enable-tls13downgrade enable-ssl-trace

# For OpenSSL-1.0.2-stable
#./Configure debug-darwin64-x86_64-cc --debug --prefix=/opt/local --openssldir=/opt/local/etc/openssl shared threads enable-rfc3779

make update && make depend && make clean && make -j 2 all && make test && make install

