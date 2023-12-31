#!/usr/bin/env perl

#
# PostGIS - Spatial Types for PostgreSQL
# http://postgis.net
#
# Copyright (C) 2017 Sandro Santilli <strk@kbt.io>
#
# This is free software; you can redistribute and/or modify it under
# the terms of the GNU General Public Licence. See the COPYING file.
#

use warnings;
use strict;
use POSIX 'strftime';

eval "exec perl -w $0 $@"
  if (0);


die "Usage: perl $0 <extname> [<sqlfile>]\n"
  . "  Prints SQL to drop objects dropped by given SQL file\n"
  . "  from extension with given name.\n"
unless @ARGV;

my $extname = shift(@ARGV);

while(<>)
{

  if ( /_postgis_drop_function_by_signature\(/ )
  {
    s/SELECT _postgis_drop_function_by_signature\('/DROP FUNCTION /;
    s/'.*/;/;
  }
  elsif ( /_postgis_drop_function_by_identity\(/ )
  {
    #print "-- FUNCTION by id: $_\n";
    s/SELECT _postgis_drop_function_by_identity\('/DROP FUNCTION /;
    s/' *, *'/(/;
    #print "-- FUNCTION by id (2): $_\n";
    s/'.*;/);/;
    #print "-- FUNCTION by id (3): $_\n";
  }

  /^DROP/i or next;
  s/ IF EXISTS//i;
  s/ CASCADE//i;
  chop;

  print 'DO $$ BEGIN ALTER EXTENSION ' . $extname
    . ' ' . $_ . 'EXCEPTION '
    . 'WHEN object_not_in_prerequisite_state OR undefined_function OR undefined_object THEN '
    . " RAISE NOTICE '%', SQLERRM; "
    . 'WHEN OTHERS THEN '
    . " RAISE EXCEPTION 'Got % - %', SQLSTATE, SQLERRM; "
    . 'END $$ LANGUAGE ' . "'plpgsql';\n";
}

1;
