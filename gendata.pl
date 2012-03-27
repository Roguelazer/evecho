#!/usr/bin/perl
#
# Create a data file of the given length.
# Usage: gendata.pl length_in_bytes > outfile

use warnings;
use strict;

my $length = shift;
die "usage: gendata.pl length_in_bytes > outfile" if (!defined($length));

my $chr = 0;
my $written = 0;
for (my $written = 0; $written < $length; $written++) {
    syswrite(STDOUT, chr($chr + 65), 1);
    $chr = ($chr + 1) % 61;
}
