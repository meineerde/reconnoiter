#!/bin/bash

perl -ni -e 's/(\s)\/\S+/$1/g; s/(\s)\S+dtrace_probes\.h/$1/g; next if /^\s*\\?$/; print "\n" if /^\S/; s/^\s+(?=\S)/  /g; print;' $*
