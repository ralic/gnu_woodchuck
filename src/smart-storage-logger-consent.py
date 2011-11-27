# smart-storage-logger-consent.py - Obtain user's consent.
# Copyright (C) 2011 Neal H. Walfield <neal@walfield.org>
#
# Woodchuck is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 3, or (at
# your option) any later version.
#
# Woodchuck is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see
# <http://www.gnu.org/licenses/>.

import os
import sys
import gtk
import hildon
import pango
import re

consent_dir = "/home/user/.smart-storage"
consent_file = consent_dir + "/consent"
accept = "accept"
reject = "reject"

def reject_button_clicked(button, dialog):
    gtk.Widget.destroy (dialog)
 
def main():
    # If the user already consented in the past, we are done.
    consent = ""
    try:
        f = open (consent_file, "r")
     	consent = f.read ()
	print consent_file + ": '" + consent + "'"
	if consent.startswith (accept):
            print "user already accepted"
            sys.exit (0)
	if consent.startswith (reject):
            print "user already rejected"
            sys.exit (1)
    except Exception, e:
        print "Reading consent file: ", e

    def build_text_area (text):
        buffer = gtk.TextBuffer ()
    
        tag = buffer.create_tag("bold", weight=pango.WEIGHT_BOLD)
        tag = buffer.create_tag("underline", underline=pango.UNDERLINE_SINGLE)
        tag = buffer.create_tag("wordwrap",
                                wrap_mode=gtk.WRAP_WORD, wrap_mode_set=True)
        tag = buffer.create_tag("indent", left_margin=20, left_margin_set=True)
        
        # Split into paragraphs.
        paragraphs = re.split ("(?:\n[ ]*){1,}\n", text)
    
        # Split lists into paragraphs.
        q = []
        for p in paragraphs:
            match = re.match ("^[ ]+([0-9]+)[.][ ]", p)
            if match:
                # The start of the paragraph has the form " 1. "
                start = int (match.group (1))
    
                t = re.split ("[ ]*\n[ ]+[0-9]+[.][ ]", p)
                if len (t) == 1:
                    q.append (p)
                else:
                    q.append (t[0])
                    for i in range (1, len (t)):
                        q.append (" " + str (start + i) + ". " + t[i])
            else:
                q.append (p)
        paragraphs = q;
        
        for p in paragraphs:
            tags = [ "wordwrap" ];
    
            if p[0] == ' ':
                tags.append ("indent")
    
            # Replace \n's with spaces
            p = p.replace ("\n", " ")
            # Compress multiple spaces to a single space.
            p = re.sub (" +", " ", p)
            # Remove any leading spaces.
            p = re.sub ("^ ", "", p)
    
            if re.match ("^[0-9A-Z ]{5,60}:?[ ]*$", p):
                tags.append ("bold")
                tags.append ("underline")
    
            iter = buffer.get_iter_at_offset (-1)
            buffer.insert_with_tags_by_name (iter, p, *tags)
            buffer.insert (iter, "\n\n")
            
        textview = hildon.TextView ()
        textview.set_editable (False)
        textview.set_cursor_visible (False)
        textview.set_buffer (buffer)
        textview.show ()
    
        pannable = hildon.PannableArea ()
        pannable.set_size_request (800, 500)
        pannable.add (textview)
        pannable.show ()

        return pannable

    def show_consent_form ():
        notebook = gtk.Notebook()
        notebook.append_page (build_text_area (overview),
                              gtk.Label("Overview"))
        notebook.append_page (build_text_area (consent_form),
                              gtk.Label("Consent Form"))
     
        dialog = hildon.WizardDialog \
            (None, "Woodchuck Research Project Participation", notebook)
     
        for button in dialog.action_area:
            # We would like to check button.get_data
            #  ("gtk-dialog-response-data") ==
            #  hildon.WIZARD_DIALOG_FINISH, however, this causes a seg
            #  fault.
            if (button.get_label () == "Finish"):
                button.set_label ("Accept")
        reject_button = dialog.add_button("Reject", gtk.RESPONSE_DELETE_EVENT)
        reject_button.connect ("clicked", reject_button_clicked, dialog)
    
        dialog.show ()
    
        if dialog.run () == hildon.WIZARD_DIALOG_FINISH:
            return accept
        else:
            return reject

    def show_really (result):
        if result == accept:
            dialog = gtk.Dialog \
                ("Woodchuck Research Project Participation: Consent Granted")
            text_area = build_text_area (accept_text)
            finish_button = dialog.add_button("Install",
                                              hildon.WIZARD_DIALOG_FINISH)
        else:
            dialog = gtk.Dialog \
                ("Woodchuck Research Project Participation: Consent Declined")
            text_area = build_text_area (reject_text)
            finish_button = dialog.add_button("Uninstall",
                                              hildon.WIZARD_DIALOG_FINISH)

        dialog.vbox.pack_start (text_area)

        reject_button = dialog.add_button("Back", 10)
        reject_button.connect ("clicked", reject_button_clicked, dialog)

        dialog.show ()

        done = True
        if dialog.run () == 10:
            # User clicked back.
            done = False
    
        try:
            os.makedirs (consent_dir)
        except Exception, e:
            print "mkdir (" + consent_dir + "): ", e
        try:
            f = open (consent_file, "w")
            f.write (result + " 20101118")
            f.close ()
        except Exception, e:
            print "Writing to " + consent_file + ": ", e
    
        if done:
            if result == accept:
                sys.exit (0)
            else:
                sys.exit (1)

    while (1):
        show_really (show_consent_form ())

overview = """
OVERVIEW

Before you participate in the research study, you need to read,
understand and agree to the informed consent form.  *If you have any
doubts, click REJECT*, the software will not be installed and no data
will be collected.

The informed consent form consists of 9 parts:

  1. Purpose of research study
  2. Participation
  3. Procedures
  4. Risks/Discomforts
  5. Confidentiality
  6. Benefits
  7. Voluntary participation and your right to withdraw
  8. Compensation
  9. What to do if you have questions or concerns

Note: You can also view this consent form online at anytime at:
<http://hssl.cs.jhu.edu/~neal/woodchuck/consent_form/>

Click Next to continue.
"""

consent_form = """
INFORMED CONSENT FORM

View online at <http://hssl.cs.jhu.edu/~neal/woodchuck/consent_form/>.

TITLE OF RESEARCH PROJECT:

Smart Storage: Improving the User Experience on Mobile Devices by More
Intelligently Managing Data

Principal Investigator: Randal Burns, the John Hopkins University (USA)
Whiting School of Engineering

Date: November 18, 2010

WARNING:

This study includes the COLLECTION OF PERSONALLY IDENTIFYING
INFORMATION, including the names of files on your mobile computer, and
web sites that you visit.  Before installing the software, you should
carefully read this document to understand what information is
collected and the potential risks.  IF YOU DO NOT COMPLETELY
UNDERSTAND THE RISKS, which are explained in detail below, DO NOT
INSTALL THE SOFTWARE.

PURPOSE OF RESEARCH STUDY:

The purpose of this user study is to understand what types of data
people use on mobile computers, such as smart phones and laptops, how
they use that data, and when network connectivity and power are
available.  The overarching goal of the research project is to improve
the user experience on mobile computers by better automating the
management of data--automatically downloading data the user is likely
to use, and deleting (or suggesting for deletion) data that the user
is likely to no longer need when free storage space becomes scarce.

PARTICIPATION:

Anyone 18 years of age or older may participate in the study.  There
are no further restrictions on who can fill out the questionnaire.
Anyone may install the logging software who has a compatible device
and operating system or who borrows a device from us.  We only have a
limited number of devices and will select recipients based on need and
responses to the questionnaire.

We expect to have hundreds of participants who fill out the
questionnaire and/or install the software.

PROCEDURES:

This experiment consists of two optional parts.

The first part is a questionnaire, which asks you about how you use
computers and the Internet and some specific technologies including
RSS feeds and PodCasts.  The questions do not request data that can be
used to identify you individually.  We estimate that filling out the
questionnaire will take approximately 20 minutes.

In the second part, you run a logging program on one or more computers
for approximately one year.  Installing the program will take
approximately 10 minutes.  After installing the program, you will not
need to interact with it again.

(If you don't have a smart phone and would like to borrow one, we have
a limited number available.  In this case, you need to fill out the
questionnaire and indicate that you would like to borrow one.  Based
on the responses, we will select a number of people.)

The logging software collects data about the files you use, the
programs you run, your network connectivity and battery status.
Specifically, the collected data are:

   1. File accesses (the filename of the accessed file, the type of
      access, create, delete, read, write, and the time of the access;
      the contents of the file will not be recorded);
   
   2. Programs run (their names, the time at which they were started, and
      the time at which they exited);
   
   3. Battery status (the available power as reported by the operating
      system and the time the sample was taken, when the device is
      attached to a power source and when it is detached);
   
   4. Network connection statistics (the name of the network,
      anonymized using a one-way hash (explained in the section
      CONFIDENTIALITY, below), e.g., the cell phone provider's common
      name or the WiFi access point's SSID, when a connection is
      established, when a connection is disconnected, the number of
      bytes transferred, the connection's signal strength, and the
      time of the sample);
   
   5. Available networks (the identity of networks, anonymized using a
      one way hash, available in the vicinity of the device, but which
      the device has not connected to, and the time stamp at which the
      sample was made);
   
   6. Connected cell tower (time at which the device connect to the
      cell tower and the tower's identification including its location
      area code, the cell identifier, the network code and the country
      identifier, anonymized using a one-way hash, except the country
      identifier);
   
   7. User activity (when the user starts interacting with the system,
      and when the user stops interacting with the system, e.g., when
      the screen saver is deactivated or activated);
   
   8. System boot, shutdown, suspend and resume (the time when the
      system boots, when the time when the system is turned off, when
      the system is suspended and when it is resumed);
   
   9. Web sites that the user visits, anonymized using a one-way hash
      for each URL component except for the name of the file, e.g.,
      http://porn.com/transvestites/pic1.jpg would be reported as
      something along the lines of
      http://a1040356.3275c2/0016fff24/pic1.jpg; and,
   
   10. Warnings and error messages emitted by the logging program (the
      time at which the message was emitted and the message itself;
      for debugging purposes).

The program is designed to be lightweight so as to minimally interfere
with you.  Moreover, the program will be run with a low priority.

Approximately once per day, the logging program will upload the data
over an encrypted connection to hssl.cs.jhu.edu, a server under PI
Burn's control. This will only be done if:

  1. There is an ethernet or WiFi connection available; a cellular
     connection will never be used to avoid the chance of the logging
     program incurring monetary charges.
  2. The user appears to not be interacting with the system (e.g., the
     screen saver is active).
  3. The server's reported identity matches the expected identity
     (this is done using cryptographic techniques and ensures that the
     data is not accidentally reveal data to the wrong server).

If the data is successfully uploaded, the synchronized data will be
deleted from the device thereby reducing the storage space used by the
logging program on the device and avoiding the case that someone who
later obtains the device gets access to the logged data.

The data will be identified by a random, secret identifier that is
generated on the device when the program is first run.

When the experiment is completed, an updated version of the program
will be made available, which should automatically be made available
to you using the operating system's usual software update mechanism.
This version of the program will delete any remaining logged data,
disable logging, and remove the logging software.


RISKS/DISCOMFORTS:

The risks associated with participation in this study are minor; we
try to be aware of, and carefully avoid, any problems that could arise
for participants as a result of taking part in this research.
Disclosure of data collected about you (detailed above in the section
PROCEDURES) is the primary risk.  The data that may have personally
identifying information includes:

  1. File names (the standard email client on the Nokia N900 includes
     the email address of the email account in the file name when saving
     email messages);
  2. URLs of web pages the user visited; and,
  3. Location information (names of WiFi access points and cellular
     towers).

If collected data is exposed, you may be embarassed: the data may
include URLs of web sites that embarass you, e.g., pornography.

A person who accesses your device (e.g., a thief) is able to access
recently logged data.  As data is anonymized prior to it being saved,
the amount of information an attacker could gain is reduced.  Further,
this is limited to data logged since the last upload, which typically
occurs every 24 hours, since logged data is deleted from the device.
Some information an attacker could learn include: The names of files
that you have recently deleted.  If the person were interested in
knowing whether you were at a particular location and knew the
identity of cell phone towers in that area, that person could verify
whether you were there at a particular time.


CONFIDENTIALITY:

Any study records that identify you will be kept confidential to the
extent possible by law.  The records from your participation may be
reviewed by people responsible for making sure that research is done
properly, including members of the Homewood Institutional Review Board
of the Johns Hopkins University, or officials from government agencies
such as the National Institutes of Health or the Office for Human
Research Protections.  (All of these people are required to keep your
identity confidential.)  Otherwise, records that identify you will be
available only to people working on the study, unless you give
permission for other people to see the records.

To protect your privacy, we partially anonymize the URLs and location
information using a one-way hash (as described in the section
PROCEDURES).  A one-way hash is a deterministic procedure for
transforming data.  It has two important properties: given the hashed
data, it is impractical to recover the original data; and, the same
data always results in the same transformation.  These properties
allow us to determine how often participants frequent a particular
location or web site without being able to identify the web site or
location.  However, because the hash transforms the same data in the
same way, it is possible to verify the existence of particular data.
For instance, determining what corresponds to "2q34lkjgfd.9808" is
difficult.  However, if someone with access to the data wants to
determine whether a trace includes particular data, e.g., "porn.com,"
it is only necessary to compute the hash corresponding to "porn.com,"
perhaps, "2q34lkjgfd.9808," and checking if that data appears in the
trace.

Data collected from you will be stored on password protected
computers.  The data collected by the logging program will be
transferred using an encrypted connection.  The identity of the
server will also be cryptographically verified.  Ten years after the
study has been completed, the data will be deleted.


BENEFITS:

You will not receive any direct benefits from participating in this
study.  This study may benefit society if the results lead to a better
understanding of how data is used on mobile devices.  Specifically,
this study tries to increase data availability, improve the
performance of data access, improve battery life, decrease internet
connectivity costs, and facilitate managing data by determining which
files to prefetch and which files are least valuable.


VOLUNTARY PARTICIPATION AND RIGHT TO WITHDRAW:

Your participation in this study is entirely voluntary: you choose
whether to participate.  If you decide not to participate, there are
no penalties, and you will not lose any benefits to which you would
otherwise be entitled.

If you choose to participate in the study, you can stop your
participation at any time, without any penalty or loss of benefits.
If you want to withdraw from the study, you need either stop filling
out the questionnaire, or uninstall the software on your devices.
 

COMPENSATION:

You will not receive any payment or other compensation for
participating in this study.


IF YOU HAVE QUESTIONS OR CONCERNS:

You can ask questions about this research study now or at any time
during the study, by talking to the researcher(s) working with you, or
by emailing Neal Walfield (<mailto:neal@cs.jhu.edu>), the student
investigator for this study.  If you have questions about your rights
as a research subject, or feel that you have not been treated fairly,
please call the Homewood IRB at Johns Hopkins University at (410)
516-6580.
"""

accept_text = """
CONSENT GRANTED

Thanks for your granting your consent!  *If you didn't mean to accept,
click the BACK button.*

Even if you accept now, you can still withdraw your consent at any
time by starting the application manager and uninstalling the
smart-storage-logger program.  In this case, no further data will be
gathered.

To follow the project, visit
<http://hssl.cs.jhu.edu/~neal/woodchuck>.
"""

reject_text = """
CONSENT DECLINED

You've declined to give your consent.  No problem!  *No data will be
collected and the smart-storage-logger package will be automatically
uninstalled* once you exit the application manager.

To can still follow the project by visiting
<http://hssl.cs.jhu.edu/~neal/woodchuck>.

If you didn't mean to decline, click the BACK button.
"""

if __name__ == "__main__":
    main()
