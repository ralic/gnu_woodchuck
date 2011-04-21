# To generate the keys:
#
# openssl req -x509 -nodes -days 3650 \
#  -subj '/C=US/ST=MD/L=Baltimore/CN=hssl.cs.jhu.edu' -newkey rsa:2048 \
#  -keyout ssl-receiver.key -out ssl-receiver.cert
#
# See http://www.madboa.com/geek/openssl/#cert-self

# We only include the public key, obviously :-)
dist_pkgdata_DATA = ssl-receiver.cert
