#! /usr/bin/env python

# Copyright 2011 Neal H. Walfield <neal@walfield.org>
#
# This file is part of Woodchuck.
#
# Woodchuck is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# Woodchuck is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
# License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see
# <http://www.gnu.org/licenses/>.

import sys
import os
from optparse import OptionParser
import traceback
import shlex
import readline
import time

# Load woodchuck.

sys.path = ["../src"] + sys.path

try:
    import woodchuck
except ImportError, e:
    # If Woodchuck is not found, don't die horribly.
    print "Loading pywoodchuck failed: %s." % (str(e),)
    sys.exit(1)

try:
    wc = woodchuck.Woodchuck()
except woodchuck.WoodchuckUnavailableError, e:
    print "No Woodchuck server running: %s" % (str(e),)
    sys.exit(1)

# Object stack is a stack of "object lists".  The user can list the
# children of a selected object.  In this case, it is the parent for
# its depth and those children are pushed on to the stack.
#
# For instance:
#
# object_stack[0] == top-level managers
# object_stack_parent[0] = 1; object_stack[1] == list of manager[1]'s streams
object_stack_index = -1
object_stack = []
object_stack_parent = []

def object_to_str(object, print_type=True):
    """
    Create a nicely formatted string for specified object.

    If print_type is true, includes the object's type.
    """
    try:
        return ("%s%s (%s)"
                % (object.__class__.__name__ + ': ' if print_type else '',
                   object.human_readable_name, object.cookie))
    except AttributeError:
        # It's not a woodchuck object...
        return "%s" % str(object)

def objects_bt():
    """
    Print stack backtrace.
    """
    def dump(object, index, print_type=True, indent=0):
        """
        Pretty print an abbreviated version of object.  indent is the
        amount of indentation.
        """
        sys.stdout.write("  " * indent)
        # We use print rather than sys.stdout.write because print
        # knows how to encode strings for the terminal.
        # sys.stdout.write can break on unicode strings like this:
        #
        # UnicodeEncodeError: 'ascii' codec can't encode character u'\xfc' in position 9: ordinal not in range(128)
        print("%d: %s" % (index, object_to_str(object, print_type=print_type)))

    for i in xrange(object_stack_index):
        dump(object_stack[i][object_stack_parent[i]],
             object_stack_parent[i] + 1, True, i)

    have_one = False
    for i, o in enumerate(object_stack[object_stack_index]):
        have_one = True
        dump(o, i + 1, False, object_stack_index)
    if not have_one:
        print "%sNo children." % ("  " * object_stack_index,)

def objects():
    """
    Return the list of objects in the current stack frame.
    """
    if object_stack_index == -1:
        return []
    return object_stack[object_stack_index]

def objects_push(objects, parent=None):
    """
    Pushes a list of objects onto the stack after the current stack
    frame.

    The current stack frame is first made the top of the stack (i.e.,
    any stack frames further up the stack are removed).

    Parent is the parent object of all objects in the list.
    """
    global object_stack_index

    if object_stack_index >= 0:
        assert parent is not None
        object_stack_parent[object_stack_index:-1] = [parent]
    object_stack[object_stack_index + 1:-1] = [objects]
    object_stack_index += 1

def object_get(index):
    """
    Return the object at index INDEX (0 based) from the current stack
    frame.  Returns None if the index is invalid.
    """
    if 0 <= index < len(object_stack[object_stack_index]):
        return object_stack[object_stack_index][index]
    return None

def object_clear(index):
    """
    Replace the object at index INDEX (0 based) of the current stack
    frame with None.
    """
    object_stack[object_stack_index][index] = None

class Shell(object):
    def getline(self):
        """
        Get a line of input.  If the end of file is reached, quit.
        """
        try:
            line = raw_input("> ")
        except EOFError:
            sys.exit(0)
        line = line.strip()
        return line

    def cmd_up(self, args):
        """
        Move up a stack frame (i.e., move towards the bottom of the stack)
        """
        if len(args) > 1:
            print "up takes at most one integer argument"
            return

        global object_stack_index

        if object_stack_index == 0:
            print "Already at bottom of stack."
            return

        try:
            delta = int(args[0])
        except ValueError:
            print "%s is not an integer" % args[0]
            return
        except IndexError:
            delta = 1

        object_stack_index = max(object_stack_index - delta, 0)

        objects_bt()
    
    def cmd_down(self, args):
        """
        Move down a stack frame (i.e., move towards the top of the stack)
        """
        if len(args) > 1:
            print "down takes at most one integer argument"
            return
    
        global object_stack_index

        if object_stack_index == len(object_stack) - 1:
            print "Already at top of stack."
            return

        try:
            delta = int(args[0])
        except ValueError:
            print "%s is not an integer" % args[0]
            return
        except IndexError:
            delta = 1

        object_stack_index = min(object_stack_index + delta,
                                 len(object_stack) - 1)

        objects_bt()
    
    def cmd_select(self, args):
        """
        Select the specified object.  Enumerates all of its children
        and pushes them on the object stack.
        """
        if len(args) > 1:
            print "select takes one argument, the index of the object to select"
            return
    
        try:
            i = int(args[0]) - 1
        except ValueError:
            print "Argument ('%s') is not an integer." % (args[0])
            return
    
        o = object_get(i)
    
        have_one = False
        objs = []
        try:
            objs += o.list_managers()
            have_one = True
        except AttributeError:
            pass
        try:
            objs += o.list_streams()
            have_one = True
        except AttributeError:
            pass
        try:
            objs += o.list_objects()
            have_one = True
        except AttributeError:
            pass
        if have_one:
            objects_push(objs, i)
        else:
            print "Cannot select object."
            return
        objects_bt()

    def cmd_print(self, args):
        """
        Print the specified object's properties.
        """
        def dump(index, indent=0):
            o = object_get(index)
            if o is None:
                return

            for prop in sorted(o.property_map.keys(), key=str.lower):
                try:
                    value = o.__getattribute__(prop)

                    if prop.endswith('_time'):
                        try:
                            if int(value) > 0:
                                t = time.localtime(int(value))
                                value = ("%s (%s)"
                                         % (time.strftime ("%c", t), value))
                        except Exception, e:
                            import logging
                            logging.exception("%s", e)
                except Exception, e:
                    value = "Error: %s" % e
                print("%s%s: %s" % ("  " * indent, prop, value))
            
        if len(args) == 0:
            for i, _ in enumerate(objects()):
                print "%d: %s" % (i + 1, object_to_str(object_get(i)))
                dump(i, 1)
                print ""
        for arg in args:
            try:
                dump(int(arg) - 1)
            except ValueError, e:
                print "%s: %s" % (arg, str(e))

    def cmd_del(self, args):
        """
        Delete (i.e., unregister) the specified object.
        """
        for k in args:
            i = int(k) - 1
            o = object_get(i)
            print "Remove %s? y/N " % object_to_str(o),
            response = self.getline()
            if response != 'y':
                print "Not removing."
            else:
                try:
                    try:
                        o.unregister()
                    except TypeError:
                        o.unregister(False)
                    object_clear(i)
                except woodchuck.Error, e:
                    print "Error removing: %s" % (str(e),)
        objects_bt()

    def cmd_set(self, args):
        """
        Set an object's attribute to a specified value.  For example:
        set 1 freshness 3600
        """
        i = int(args[0]) - 1
        property = args[1]
        value = args[2]

        o = object_get(i)

        print("Changing %s: %s => %s"
              % (property, o.__getattribute__(property), value))
        o.__setattr__(property, value)

    def cmd_help(self, args):
        """
        Display an overview of the available commands.
        """
        longest = max(len(c) for c, f in self.commands)
        for command in sorted(self.commands, key=lambda x: str.lower(x[0])):
            if command[0] == 'help':
                continue

            doc = command[1].__doc__
            if not doc:
                doc = ""
            lines = doc.strip().split('\n')

            leading_whitespace = 1000
            for line in lines[1:]:
                for i, c in enumerate(line):
                    if c != ' ':
                        leading_whitespace = min(leading_whitespace, i)
                        break

            if leading_whitespace:
                for i, line in enumerate(lines[1:]):
                    lines[i + 1] = line[leading_whitespace:]

            doc = ('\n%s' % (' ' * (longest + 3))).join(lines)
            print("%-*s - %s" % (longest, command[0], doc))

    @property
    def commands(self):
        """
        The list of known commands.
        """
        cls = self.__class__
        if not hasattr(cls, '_commands'):
            c = []
            for name, value in cls.__dict__.items():
                if name.startswith('cmd_'):
                    c.append((name[len('cmd_'):], value))
            cls._commands = c
        return cls._commands

    def process(self, command, args):
        matches = []
        for name, value in self.commands:
            if name.startswith(command):
                matches.append((name, value))

        if len(matches) == 0:
            print "Unknown command %s.  Type 'help' for help." % command
            return

        if len(matches) > 1:
            print("'%s' is ambiguous (matches: %s)"
                  % (command, ' '.join(m[0][len('cmd_'):] for m in matches)))
            return

        matches[0][1](self, args)

    def repl(self):
        """
        read-eval-print loop.
        """
        objects_push(woodchuck.Woodchuck().list_managers())
        objects_bt()
    
        while True:
            line = self.getline()
            if not line:
                objects_bt()
                continue
    
            args = shlex.split(line)
            try:
                shell.process(args[0], args[1:])
            except Exception, e:
                print("Executing %s: %s\n%s"
                      % (args[0], str(e), traceback.format_exc()))
    
shell = Shell()

# If we have command arguments, just treat it as a single command.
if len(sys.argv) > 1:
    shell.process(sys.argv[1], sys.argv[2:])
else:
    shell.repl()
