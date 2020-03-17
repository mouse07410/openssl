#!/bin/bash -ex

unset OPENSSL_INCLUDE_DIR
unset OPENSSL_LIB_DIR
unset OPENSSL_CFLAGS
unset OPENSSL_LIBS
unset OPENSSL_CONF
unset OPENSSL

OPENSSL_DIR="$HOME/openssl-3"
OPENSSL_GOST_ENGINE_SO=${OPENSSL_DIR}/lib/engines-3/gost.dylib

CFLAGS="$CFLAGS -g"

make distclean || true

# For OpenSSL-3 master (development branch)
./config --prefix=${OPENSSL_DIR} --debug --openssldir=${OPENSSL_DIR}/etc --with-rand-seed=rdcpu enable-aria enable-ec_nistp_64_gcc_128 enable-md2 enable-rc5 enable-weak-ssl-ciphers enable-zlib-dynamic enable-ssl-trace
#enable-tls1_3 enable-tls13downgrade

# For OpenSSL-1.1.1-stable
#./config --debug --prefix=$HOME/openssl-1.1 --openssldir=$HOME/openssl-1.1/etc enable-ec_nistp_64_gcc_128 enable-md2 enable-rc5 enable-weak-ssl-ciphers enable-zlib-dynamic enable-tls1_3 enable-tls13downgrade enable-ssl-trace

# For OpenSSL-1.0.2-stable
#./Configure debug-darwin64-x86_64-cc --debug --prefix=/opt/local --openssldir=/opt/local/etc/openssl shared threads enable-rfc3779

make update && make depend && make -j 2 all 2>&1 | tee make-out.txt && OPENSSL_GOST_ENGINE_SO="${OPENSSL_GOST_ENGINE_SO}" make test 2>&1 | tee test-out.txt  && make install && cp apps/openssl-3.cnf $OPENSSL_DIR/etc/openssl.cnf && tail test-out.txt

