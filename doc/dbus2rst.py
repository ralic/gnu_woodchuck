import sys
import xml.parsers.expat

comment = None
interface = None
method = None
method_comment = None
args = None
output_file = None

output_dir = '.'

do_debug = False
def debug(*args):
    if do_debug:
        print (args)

def fix_whitespace(text, indent):
    """Chop off any leading whitespace.  Expand tabs.  Ignore the
    first line's white space.  Make sure there are two \ns at the end
    of the text."""
    lines = text.expandtabs ().splitlines ()
    if not lines:
        return "\n"

    min_len = None
    for l in range (len (lines)):
        spaces = 0
        while spaces < len (lines[l]) and lines[l][spaces] in (' ', '\t'):
            if lines[l][spaces] == ' ':
                spaces += 1
        lines[l] = "%*s%s" % (spaces, "", lines[l][spaces:])

        if spaces == len (lines[l]):
            lines[l] = ""
        else:
            if l == 0:
                # Trim any leading spaces from the first line.
                lines[0] = lines[0][spaces:]
            elif min_len is None or spaces < min_len:
                min_len = spaces

    if min_len is not None and lines[0] != "":
        lines[0] = "%*s%s" % (min_len, "", lines[0])

    lines = "".join (["%*s%s\n" % (indent, "", line[min_len:])
                      for line in lines]) + "\n"

    return lines

def start_element(name, attrs):
    global last_comment
    global interface
    global method
    global method_comment
    global args
    global output_file

    name = name.lower ()

    debug ('Start element:', name, attrs)

    if name == 'node':
        pass

    elif name == 'interface':
        if interface is not None:
            raise ValueError ("Nested <interface>s not allowed.")
        interface = attrs['name']

        if output_file is not None:
            output_file.close ()
        output_file = open (output_dir + '/' + interface + ".rst", "w")

        output_file.write (interface + "\n")
        output_file.write ('-' * len (interface) + "\n")
        output_file.write ("\n")

        output_file.write ('.. class:: %s\n\n' % (interface))
        if last_comment is not None:
            output_file.write (fix_whitespace (last_comment, 4))

    elif name == 'method':
        if interface is None:
            raise ValueError ("<method>s outside of <interface>s not allowed.")
        if method is not None:
            raise ValueError ("Nest <method>s not allowed.")

        method = attrs['name']

        method_comment = (fix_whitespace (last_comment, 8)
                          if last_comment is not None else "")

        args = []

    elif name == 'arg':
        if method is None:
            raise ValueError ("<arg>s outside of <method>s not allowed.")

        method_comment = method_comment \
            + "        :param " \
            + attrs.get ('direction', 'in') + " " \
            + attrs.get ('name', '') + " " \
            + attrs.get ('type', '') + ":\n" \
            + fix_whitespace (last_comment if last_comment is not None else "",
                              12)

        args.append (attrs.get ('name', None))

    elif name == 'property':
        if method:
            raise ValueError ("<property>s not allowed in <method>s.")
        if interface is None:
            raise ValueError ("<method>s outside of <interface>s not allowed.")

        output_file.write ("    .. data:: " + attrs['name'] + "\n\n"
                           + (fix_whitespace (last_comment, 8)
                              if last_comment is not None else ""))

    else:
        raise ValueError ("Unknown tag <" + name + ">");

    last_comment = None

# Got an end of tag.
def end_element(name):
    debug ('End element:', name)

    global last_comment
    global interface
    global method
    global method_comment
    global output_file

    name = name.lower ()

    if name == 'method':
        # Create the header (function (args)) and flush the comment.
        if method is None:
            raise ValueError ("</method>, but no <method>.")

        output_file.write ("    .. function:: %s (" % (method,))
        for i in range (len (args)):
            if i != 0:
                output_file.write (", ")
            if args[i] is None:
                output_file.write ("arg%d" % (i,))
            else:
                output_file.write (args[i])
        output_file.write (")\n"
                           + "\n"
                           + method_comment + "\n")

        method = None
        method_comment = None

    elif name == 'interface':
        # End of an interface.

        if interface is None:
            raise ValueError ("</interface>, but no <interface>.")
        if method is not None:
            raise ValueError ("</interface>, but no in a <method>.")

        interface = None

    elif name in ['node', 'arg', 'property']:
        # Nothing to do.
        pass

    else:
        raise ValueError ("</%s> unexpected.", name)

    last_comment = None

def comment(data):
    global last_comment

    # If we concatenate multiple comments, they might have different
    # spacing.  Clean it up now.
    fix_whitespace (data, 0)

    if last_comment is None:
        # First comment.
        last_comment = data
    else:
        # Append it to the previous comment.
        last_comment = last_comment + "\n\n" + data

file = [True] * len (sys.argv)

for i in range (1, len (sys.argv)):
    if not file[i]:
        continue
    arg = sys.argv[i]

    if arg == '-d':
        output_dir = sys.argv[i + 1]
        file[i] = False
        file[i + 1] = False

for i in range (1, len (sys.argv)):
    if not file[i]:
        continue
    arg = sys.argv[i]

    contents = open (arg, "r").read ()

    p = xml.parsers.expat.ParserCreate()

    p.StartElementHandler = start_element
    p.EndElementHandler = end_element
    p.CommentHandler = comment

    p.Parse(contents, 1)

    if output_file is not None:
        output_file.close ()
