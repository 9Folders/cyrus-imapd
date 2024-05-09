#!/bin/bash

set -e

: ${CYRUSLIBS:=cyruslibs}
: ${LIBSDIR:=/usr/local/$CYRUSLIBS}
: ${TARGET:=/usr/local/cyrus}
: ${CONFIGOPTS:="--enable-jmap --enable-http --enable-calalarmd --enable-unit-tests --enable-replication --enable-nntp --enable-murder --enable-idled --enable-xapian --enable-autocreate --enable-backup --enable-silent-rules"}
export LDFLAGS="-L$LIBSDIR/lib/x86_64-linux-gnu -L$LIBSDIR/lib -Wl,-rpath,$LIBSDIR/lib/x86_64-linux-gnu -Wl,-rpath,$LIBSDIR/lib"
export PKG_CONFIG_PATH="$LIBSDIR/lib/x86_64-linux-gnu/pkgconfig:$LIBSDIR/lib/pkgconfig:\$PKG_CONFIG_PATH"
export CFLAGS="-Wno-unused-parameter -g -O0 -fPIC -W -Wall -Wextra"
export PATH="$LIBSDIR/bin:$PATH"
autoreconf -v -i -s
echo "./configure --prefix=$TARGET $CONFIGOPTS XAPIAN_CONFIG=$LIBSDIR/bin/xapian-config-1.5"
./configure --prefix=$TARGET $CONFIGOPTS XAPIAN_CONFIG=$LIBSDIR/bin/xapian-config-1.5
sudo make
sudo make install
#sudo make install-binsymlinks
#sudo cp tools/mkimap /usr/local/cyrus/bin/mkimap
#sudo cp tools/mknewsgroups /usr/local/cyrus/bin/mknewsgroups
