#! /usr/bin/env perl
# Copyright 2015-2018 The OpenSSL Project Authors. All Rights Reserved.
#
# Licensed under the Apache License 2.0 (the "License").  You may not use
# this file except in compliance with the License.  You can obtain a copy
# in the file LICENSE in the source distribution or at
# https://www.openssl.org/source/license.html


use strict;
use warnings;

use OpenSSL::Test qw(:DEFAULT data_file bldtop_dir srctop_file srctop_dir bldtop_file);
use OpenSSL::Test::Utils;

BEGIN {
setup("test_evp");
}

use lib srctop_dir('Configurations');
use lib bldtop_dir('.');
use platform;

# Default config depends on if the legacy module is built or not
my $defaultcnf = disabled('legacy') ? 'default.cnf' : 'default-and-legacy.cnf';

my @configs = ( $defaultcnf );
# Only add the FIPS config if the FIPS module has been built
push @configs, 'fips.cnf' unless disabled('fips');

my @files = qw( evpciph.txt evpdigest.txt );
my @defltfiles = qw( evpencod.txt evpkdf.txt evppkey_kdf.txt evpmac.txt
    evppbe.txt evppkey.txt evppkey_ecc.txt evpcase.txt evpaessiv.txt
    evpccmcavs.txt );

plan tests => (scalar(@configs) * scalar(@files)) + scalar(@defltfiles) + 1;

my $infile = bldtop_file('providers', platform->dso('fips'));
$ENV{OPENSSL_MODULES} = bldtop_dir("providers");
$ENV{OPENSSL_CONF_INCLUDE} = bldtop_dir("providers");

ok(run(app(['openssl', 'fipsinstall', '-out', bldtop_file('providers', 'fipsinstall.conf'),
            '-module', $infile,
            '-provider_name', 'fips', '-mac_name', 'HMAC',
            '-macopt', 'digest:SHA256', '-macopt', 'hexkey:00',
            '-section_name', 'fips_sect'])), "fipinstall");

foreach (@configs) {
    $ENV{OPENSSL_CONF} = srctop_file("test", $_);

    foreach my $f ( @files ) {
        ok(run(test(["evp_test", data_file("$f")])),
           "running evp_test $f");
    }
}

#TODO(3.0): As more operations are converted to providers we can move more of
#           these tests to the loop above

$ENV{OPENSSL_CONF} = srctop_file("test", $defaultcnf);

foreach my $f ( @defltfiles ) {
    ok(run(test(["evp_test", data_file("$f")])),
       "running evp_test $f");
}
