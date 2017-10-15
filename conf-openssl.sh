#!/bin/bash -ex

make distclean || true

# For OpenSSL-1.1.1 master (development branch)
#./config --prefix=$HOME/openssl-1.1 --debug --openssldir=$HOME/openssl-1.1/etc --with-rand-seed=rdcpu enable-aria enable-ec_nistp_64_gcc_128 enable-md2 enable-rc5 enable-weak-ssl-ciphers enable-zlib-dynamic enable-tls1_3 enable-tls13downgrade

# For OpenSSL-1.1.0-stable
#./config --debug --prefix=$HOME/openssl-1.1 --openssldir=$HOME/openssl-1.1/etc enable-ec_nistp_64_gcc_128 enable-md2 enable-rc5 enable-weak-ssl-ciphers enable-zlib-dynamic enable-tls1_3 enable-tls13downgrade

# For OpenSSL-1.0.2-stable
./Configure darwin-x86_64-cc --debug --prefix=/opt/local --openssldir=/opt/local/etc/openssl shared threads enable-rfc3779

make depend && make clean && make -j 2 all && make test && sudo make install

