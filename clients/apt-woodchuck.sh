#! /bin/sh

exec /usr/bin/run-standalone.sh @PYTHON@ '@pythondir@/apt-woodchuck.pyo' ${1+"$@"}
