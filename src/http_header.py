# http_header.py - An RFC 2616 header parser.
# Copyright (C) 2009 Free Software Foundation, Inc.
# Written by Neal H. Walfield <neal@gnu.org>.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see
# <http://www.gnu.org/licenses/>.

import re

class http_headers ():
    """
    Implements a dictionary-like class for http headers with an
    interface similar to the mimetools.Message class.  Headers are
    automatically combined and unfolded according to RFC 2616.

    Unlike the mimetools.Message class, indexing is case insenstive
    and returns all headers that match the given key.  Given the
    following header:

      foo: bar
      Foo: bam

    http_headers['foo'] returns 'bar, bam'
    """
    def __init__ (self, file=None, seekable=True):
        """
        This routine provides a similar interface to the
        mimetools.Message class, however, file may also be a string.


        A note about the headers attributes:

        Headers are key, value pairs.  Keys are case insensitive.
        We'd like to be case preserving if possible.  We'd also like
        to preserve order when combining headers.  For instance:
        
          foo: bar
          Foo: bam
        
        should be combined as:
        
          foo: bar, bam
        
        not as:
        
          foo: bam, bar
        
        Also, operations should not be O(n) where n is the number of
        headers.
        
        To achieve, we use a dictionary.  The dictionary is keyed on
        the lower-case versions of the keys. Its values are tuples,
        the first of which is the value of the header the first time
        we saw it.  Thus, if we see 'Content-Type', the key is
        'content-type' and the first value of the tuple is
        'Content-Type'.  The remaining values in the tuple are the
        values of the headers with leading and trailing white space
        stripped.
        """
        self.headers = {}

        if not file:
            return

        k, v = None, None

        if isinstance (file, str):
            file = re.sub ('(\n\r|\r\n|\r)', '\n', file)
            lines = file.split ('\n')
            for i, l in enumerate (lines):
                if l == '':
                    # We're done.
                    lines = lines[0:i]
                    break
        elif isinstance (file, list):
            map (self.__add__, file)
            return
        else:
            lines = []
            while True:
                l = file.readline ()
                if l in ['', '\n', '\r\n']:
                    # We're done
                    break
                lines.append (l)

        for l in lines:
            if l[0] in ' \t\f\v':
                # The last header was folded.  Add this line to it's
                # value.
                v += ' ' + l.strip ()
            else:
                if k:
                    # The last line was not folded.  Flush it.
                    self.__add__ ((k, v))
                k, v = l.split (':', 1)

        # Flush the last item.
        if k:
            self.__add__ ((k, v))

    def __setitem__ (self, key, value):
        self.headers[key.lower ()] = [key, value]

    def __add__ (self, o):
        """
        Takes a (key, value) tuple.
        """
        k, v = o
        # Keys may not have white space on either side.
        assert k.strip () == k, k + ' has leading or trailing whitespace.'

        if self.headers.get (k.lower (), None):
            self.headers[k.lower ()] += [v.strip ()]
        else:
            self.headers[k.lower ()] = [k, v.strip ()]

        return self

    def __contains__ (self, key):
        return key.lower () in self.headers

    def has_key (self, key):
        return key in self

    def __getitem__ (self, key):
        return ', '.join (self.headers[key.lower ()][1:])

    def get (self, key, default=None):
        try:
            return self[key]
        except KeyError:
            return default

    def __delitem__ (self, key):
        del self.headers[key.lower ()]

    def clear (self):
        self.headers = {}

    def __len__ (self):
        return len (self.headers)

    def __str__ (self):
        return '\n'.join ([vs[0] + ': ' + ', '.join (vs[1:])
                           for vs in self.headers.itervalues ()])

    def __repr__ (self):
        return str (self.headers)

    def __iter__ (self):
        return self.keys ().__iter__ ()

    def keys (self):
        return [v[0] for v in self.headers.itervalues ()]

    def iterkeys (self):
        for v in self.headers.itervalues ():
            yield v[0]

    def iter (self):
        return self.iterkeys ()

    def values (self):
        return [', '.join (v[1:]) for v in self.headers.values ()]

    def itervalues (self):
        for v in self.headers.itervalues ():
            yield ', '.join (v[1:])

    def items (self):
        return [(v[0], ', '.join (v[1:]))
                for k, v in self.headers.iteritems ()]

    def iteritems (self):
        for v in self.headers.itervalues ():
            yield (v[0], ', '.join (v[1:]))

    def fromdict (cls, dict):
        """
        Instantiate an http_headers objet from a dict.
        """
        h = http_headers ()
        for k, v in dict.iteritems ():
            h += (k, v)

        return h
    fromdict = classmethod (fromdict)

def _test():
    # Parsing.
    h = http_headers ('A: foo\na:  bar\nA: bam\n')
    assert str (h) == 'A: foo, bar, bam'
    assert h['a'] == 'foo, bar, bam', h['a']
    assert h['A'] == 'foo, bar, bam', h['A']

    # Assignment.
    h['a'] = 'foo'
    assert h['a'] == 'foo', h['a']
    assert h['A'] == 'foo', h['A']
    assert str (h) == 'a: foo'

    # Parsing with unfolding.
    h = http_headers ('A: foo\n  a:  bar\n     \tA: bam')
    assert str (h) == 'A: foo a:  bar A: bam', str (h)

    h = http_headers ('a: 1\nB: 2\nc: 3\na: x\nb: Y\nC:  Z\nC: zyzzy')
    d = {'a': '1, x', 'B': '2, Y', 'c': '3, Z, zyzzy'}

    assert (len (h) == 3)

    # Test in and get
    assert 'a' in h
    assert h.get ('a') == d['a']
    assert 'A' in h
    assert h.get ('A') == d['a']
    assert 'b' in h
    assert h.get ('b') == d['B']
    assert 'B' in h
    assert h.get ('B') == d['B']
    assert 'c' in h
    assert h.get ('c') == d['c']
    assert 'C' in h
    assert h.get ('C') == d['c']
    assert 'd' not in h
    assert not h.get ('d')
    assert 'D' not in h
    assert not h.get ('D')

    # Test __iter__ ()
    keys = set (d.keys ())
    for k in h:
        assert k in keys, k + ' in ' + str (keys)
        keys -= set ((k,))
    assert not keys, keys

    # Test keys ()
    keys = set (d.keys ())
    for k in h.keys ():
        assert k in keys, k + ' in ' + str (keys)
        keys -= set ((k,))
    assert not keys, keys

    # Test iterkeys ()
    keys = set (d.keys ())
    for k in h.iterkeys ():
        assert k in keys, k + ' in ' + str (keys)
        keys -= set ((k,))
    assert not keys, keys

    # Test values ()
    values = set (d.values ())
    for v in h.values ():
        assert v in values, v + ' in ' + str (values)
        values -= set ((v,))
    assert not values, values

    # Test itervalues ()
    values = set (d.values ())
    for v in h.itervalues ():
        assert v in values, v + ' in ' + str (values)
        values -= set ((v,))
    assert not values, values

    # Test items ()
    keys = set (d.keys ())
    values = set (d.values ())
    for k, v in h.items ():
        assert k in keys, k + ' in ' + str (keys)
        keys -= set ((k,))
        assert v in values, v + ' in ' + str (values)
        values -= set ((v,))
        assert d[k] == v
    assert not keys, keys
    assert not values, values

    # Test iteritems ()
    keys = set (d.keys ())
    values = set (d.values ())
    for k, v in h.iteritems ():
        assert k in keys, k + ' in ' + str (keys)
        keys -= set ((k,))
        assert v in values, v + ' in ' + str (values)
        values -= set ((v,))
        assert d[k] == v
    assert not keys, keys
    assert not values, values

    # Test fromdict.
    h = http_headers.fromdict ({'a': '1', 'b': '2'})
    assert h['a'] == '1'
    assert h['b'] == '2'

    # Test del
    del h['a']
    assert len (h) == 1
    assert h.get ('a') == None
    h['b'] == '2'

    # Test clear
    h.clear ()
    assert h.get ('a') == None
    assert h.get ('b') == None
    assert len (h) == 0

if __name__ == "__main__":
    _test ()
