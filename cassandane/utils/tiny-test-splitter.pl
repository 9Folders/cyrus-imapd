#!/usr/bin/perl
use v5.20.0;
use warnings;

use File::Basename qw(fileparse);

my $file;
my @buffer;

my $filename = shift @ARGV;

my $prefix = fileparse($filename, '.pm');

unless (-d "tiny-tests/$prefix") {
  system "mkdir -p tiny-tests/$prefix";
}

open my $infile, '<', $filename
  or die "can't open $filename for reading: $!";

my @lines = <$infile>;

close $infile or die "error reading $infile: $!";

LINE: while ($_ = shift @lines) {
  if (/^sub test_(\S+)/) {
    my $file = $1 =~ s/_/-/gr;
    say $file;

    my @test_buffer;

    while (@buffer && $buffer[-1] =~ /^#/) {
      push @test_buffer, pop @buffer;
    }

    @test_buffer = reverse @test_buffer if @test_buffer;

    push @test_buffer, $_;

    my $in_heredoc;

    TESTLINE: while ($_ = shift @lines) {
      push @test_buffer, $_;

      if (/<<\s?(['"])?EOF/) {
        $in_heredoc = 1;
        next TESTLINE;
      }

      if (/^EOF$/m) {
        undef $in_heredoc;
        next TESTLINE;
      }

      if (!$in_heredoc && /^}$/ && (!@lines || $lines[0] =~ /^$/)) {
        open my $fh, '>', "tiny-tests/$prefix/$file"
          or die "can't open $file: $!";
        print {$fh} "#!perl\nuse Cassandane::Tiny;\n\n", @test_buffer;
        close $fh;
        next LINE;
      }
    }
  }

  # Skip double blanks lines.
  next LINE if @buffer && $buffer[-1] =~ /^$/ && /^$/;

  push @buffer, $_;
}

open my $outfile, '>', $filename
  or die "can't open $filename for writing: $!";

print {$outfile} @buffer;
close $outfile or die "error writing to $filename and closing: $!";
