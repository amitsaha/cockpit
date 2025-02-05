
Protocol Documentation
======================

The protocol is an implementation detail of Cockpit. It's a simple wrapper over
some other protocols, like DBus, REST, and so on. Use those instead.

Don't use this. This document ends here. It will self-destruct within 10
seconds. Leave now while you still can.

Channels
--------

Each message is part of a channel, which has a id. This channel id
identifies the type of data in the message payload, the host where it is
headed to/from and things like the service or user credentials.

An empty/missing channel id represents a special channel called the control
channel. It contains command messages. These are described below. Initially
only the control channel exists. Additional channels are opened and closed
via command messages.

Channels operate across all participants, and they help the cockpit-ws
forwarding layer to know where to send messages.

Framing
-------

The channel id is a string and ends with a newline.  It may not
contain a newline, and must be utf8 valid. After this comes the
channel dependent payload.

For example if you had a payload of the string 'abc', and it was being
sent through the channel 'a5', that might look like:

    a5\nabc

When a message is sent over a stream transport that does not have distinct
messages (such as SSH or stdio), the message is also prefixed with a 32-bit MSB
length in bytes of the message. The length does not include the 4 bytes
of the length itself.

An example. When going over a stream transport with a payload of the 3 byte
string 'abc', and a channel of 'a5', would have a message length of 6: 3 bytes of
payload, 2 bytes for the channel number, 1 for the new line. It would look like
this on the wire:

    |----msb length---| |----chan----| |---payload--|
    0x00 0x00 0x00 0x06 0x61 0x35 0x0A 0x61 0x62 0x63

Command Messages
----------------

Command messages let the various components such as cockpit-web, cockpit-ws and
cockpit-bridge communicate about what's going on.

Command messages are always sent in the control channel. They always have a
channel number of zero. The payload of a control message is always a json
object. There is always a "command" field:

    {
        "command": <command>,
        "channel": <channel>,
        ...
    }

If a command message pertains to a specific channel it has a "channel" field
containing the id of the channel. It is invalid to have a present but empty
"channel" field.

Unknown command messages are ignored, and forwarded as appropriate.

Command: init
-------------

The "init" command is the first message sent over a new transport. It is
an error if any other message is received first. The transport is not considered
open until the "init" message has been received.

The following fields are defined:

 * "version": The version of the protocol. Currently zero, and unstable.
 * "channel-seed": A seed to be used when generating new channel ids.
 * "user": An object containing information about the logged in user.

This is a single hop message. It is never forwarded.

The "init" command message may be sent multiple times across an already open
transport, if certain parameters need to be renegotiated.

Command: open
-------------

The "open" command opens a new channel for payload.

The following fields are defined:

 * "channel": A uniquely chosen channel id
 * "payload": A payload type, see below
 * "host": The destination host for the channel, defaults to "localhost"
 * "user": Optional alternate user for authenticating with host

These optional fields are used when establishing a channel over a new
connection with a host. If a connection is already open for the given
"host" and "user" then these will not be used.

 * "password": Optional alternate password for authenticating with host
 * "host-key": Optional ssh public hostkey to expect when connecting to machine

After the command is sent, then the channel is assumed to be open. No response
is sent. If for some reason the channel shouldn't or cannot be opened, then
the recipient will respond with a "close" message.

The channel id must not already be in use by another channel.

An example of an open:

    {
        "command": "open",
        "channel": "a4",
        "payload": "text-stream",
        "host": "localhost"
    }

This message is sent from the cockpit-web frontend.

Command: close
--------------

The "close" command closes a channel or more channels.

The following fields are defined:

 * "channel": The id of the channel to close
 * "reason": A short reason code for closure, or empty for a normal close

If the channel is not set, then all channels (that the recipient of the message
is aware of) will be closed.

An example of a close:

    {
        "command": "close",
        "channel" : "5x",
        "reason": "not-authorized"
    }

Any protocol participant can send this message. The cockpit-bridge and cockpit-ws
backends will send this message when a channel closes whether because of an
error or a normal closure. The frontend cockpit-web will send this when it
wants to close a channel normally.

See below for a list of problem codes.

Other fields may be present in a close message.

In the case of a connection that fails wiwh the reason "unknown-hostkey" the
host key for the server will be included in a "host-key" field in the close
message.

Command: ping
-------------

The "ping" command is simply a keep alive.

No additional fields are defined.

An example of a ping:

    {
        "command": "ping",
    }

Any protocol participant can send this message, but it is not responded to or
forwarded.

Command: authorize
------------------

The "authorize" command is for communication of reauthorization challenges
and responses between cockpit-bridge and cockpit-ws.

The following fields are defined:

 * "cookie": an string sent with a challenge, that must be present in
   the corresponding response.
 * "challenge": a challenge string from the reauthorize component, present
   in messages from cockpit-bridge to cockpit-ws
 * "response": a response string from the reauthorize component, present
   in messages from cockpit-ws to cockpit-bridge

The contents of the "challenge" and "response" fields are defined and
documented in the reauthorize component.

Example authorize challenge and response messages:

    {
        "command": "authorize",
        "cookie": "555",
        "challenge": "crypt1:74657374:$6$2rcph,noe92ot..."
    }

    {
        "command": "authorize",
        "cookie": "555",
        "response": "crypt1:$6$r0oetn2039ntoen..."
    }

Command: logout
---------------

The "logout" command is sent by the shell to cockpit-ws. It discards credentials
for the logged in user. Optionally it disconnects the user.

The following fields are defined:

 * "disconnect": if set to true then disconnect the user

Example logout message:

    {
        "command": "logout",
        "disconnect": true
    }

Payload: null
-------------

A channel opened with this payload type will never send data, and will
ignore all data it receives.

Payload: resource1
------------------

These payloads contain resource data, such as javascript and html files that
make up cockpit packages. Typically, channels of this type are opened between
cockpit-ws and cockpit-bridge. See doc/packages.md

Additional "open" command options are available to open a channel of this
type:

 * "package": the package to retrieve resource from
 * "path": path of the resource within the package.
 * "accept": various options for choosing the resource file

The "package" may either be fully qualified (ie: package@host), although the
host part is not used for routing, and the usual "open" command "host"
option should be used. The package may also be a package checksum.

If "accept" includes "minified" then a minified form of the file will
be selected, if it is available.

The channel payload will be the raw (possibly binary) byte data of the
resource being retrieved.

If "package" and "path" are missing, then the channel will be immediately
closed without a "reason", and a combined manifest of all packages, including
checksums for system packages will be returned in the "close" message under
the "packages" option:

    {
        "command": "close",
        "channel": -5,
        "packages": [
            {
                "id": ["app1", "$0d599f0ec05c3bda8c3b8a68c32a1b47"],
                "manifest" : { ... }
            },
            ...
        ]
    }

Payload: dbus-json1
-------------------

DBus messages are encoded in JSON payloads by cockpit-web, and decoded in
cockpit-bridge. Contents not yet documented. See cockpitdbusjson.c or dbus.js.

Additional "open" command options are needed to open a channel of this
type:

 * "service": A service name of the DBus service to communicate with.
 * "object-manager": The object path of a o.f.DBus.ObjectManager whose
   interfaces and properties will be relayed.
 * "object-paths": An array of object paths to start monitoring in the
   case of a non o.f.DBus.ObjectManager based service.

Messages are encoded as JSON objects. Similar to control channel messages,
each message has a "command" field. There are some obvious inefficiencies
and issues with these encodings and there is ongoing work to streamline.

 * "call": Make a DBus method call, sent by web front end.

        {
            "command": "call",
            "cookie": "mycookie",
            "objpath": "/object/path",
            "iface": "org.example.Test",
            "method": "MethodName",
            "args": [ "invalue", 5 ]
        }

 * "call-reply": Reply from a "method-call", sent by cockpit-bridge.

        {
            "command": "call-reply",
            "data": {
                "cookie": "mycookie",
                "result": ["outvalue", 3 ]
            }
        }

 * "interface-signal":

        {
            "command": "interface-signal",
            "data": {
                "objpath": "/object/path",
                "iface_name": "org.example.Test",
                "signal_name": "SignalName",
                "args": [ "value", 3 ]
            }
        }

 * "seed": A message received from cockpit-bridge when the dbus channel is
   ready. It looks like this:

        {
            "command": "seed",
            "options": { "byteorder", "be" },
            "data": {
                "/object/path": {
                    "objpath": "/object/path",
                    "ifaces": {
                        "org.example.Test": {
                            "dbus_prop_Property": "value",
                            "dbus_prop_Property2": 12,
                        }
                    }
                }
            }
        }

 * "interface-properties-changed": Sent by cockpit-bridge when the Properties of an interface change.

        {
            "command": "interface-properties-changed",
            "data": {
                "objpath": "/object/path",
                "iface_name": "org.example.Test",
                "org.example.Test" : {
                    "dbus_prop_Property": "value",
                    "dbus_prop_Property2": 12,
                }
            }
        }

 * "object-added", "object-removed", "interface-added", "interface-removed": Sent by the backend
   when an object or interfaces is added or removed.

Payload: dbus-json2
-------------------

Identical to 'dbus-json1' payloads, except that variants are encoded as
a JSON.

Payload: rest-json1
-------------------

REST as application/json requests and responses.

What normally would be an HTTP request is encoded in a JSON wrapper. See
cockpitrestjson.c or rest.js.

Additional "open" command options are needed to open a channel of this
payload type:

 * "unix": Open a channel with the given unix socket.
 * "port": Open a channel with the given TCP port on localhost.

Requests are encoded as JSON objects. These objects have the following
fields:

 * "cookie": A unique integer which identifies this request. It will
   be included in the response. Defaults to zero. If a cookie is
   reused, then a previous request with that cookie will be cancelled.
   See below for more information about cancelling of requests.
 * "method": The HTTP method. If omitted, the request does nothing
   (and no responses will be sent) except maybe cancelling a previous
   request with the same cookie.
 * "path": The HTTP path or resource. Required if "method" is given.
 * "body": JSON to be sent as the body of the HTTP request. It will
   be sent with the Content-Type application/json.
 * "poll": An optional JSON object which turns this request into
   a JSON poll. Currently it has an "interval" field, which is an
   integer of how often in milliseconds to poll. It also has a "watch"
   field with contains a cookie value of another (usually streaming)
   request to watch, when the other request changes, polls again.

Responses are encoded as JSON objects. These objects have the following
fields:

 * "cookie": The cookie number of the request.
 * "status": The HTTP status number.
 * "message" HTTP status message.
 * "complete": true when this is the last response for the request.
   If not present, or set to false, then more responses will follow
   at some point.
 * "body": JSON returned as the body of the response. If this is
   missing then no JSON was returned.

If the HTTP response body contains multiple JSON results, then these will
be returned as separate response messages.

To cancel a previous request, send a new request with the same
"cookie" value but without a "method" field.  The connection to the
unix socket or port on localhost that was used for the previous
request will be closed.  A cancelled request will not receive any
further responses, not even one to indicate that it has been
cancelled.

Payload: text-stream
--------------------

Raw text is sent back and forth to a socket. See cockpittextstream.c. The
boundaries of the messages are arbitrary, and depend on how the kernel
and socket buffer things.

Non-UTF8 data is forced into UTF8 with a replacement character.

Additional "open" command options should be specified with a channel of
this payload type:

 * "unix": Open a channel with the given unix socket.
 * "spawn": Spawn a process and connect standard input and standard output
   to the channel. Should be an array of strings which is the process
   file path and arguments.
 * "environ": If "spawn" is set, then this is the environment for the new
   spawned process. If unset, then the environment is inherited from the
   cockpit-bridge.
 * "pty": If "spawn" is set, then execute the command as a terminal pty.
 * "batch": Batches data coming from the stream in blocks of at least this
   size. This is not a guarantee. After a short timeout the data will be
   sent even if the data doesn't match the batch size. Defaults to zero.

You can't specify both "unix" and "spawn" together.

Problem codes
-------------

These are problem codes for errors that cockpit-web responds to. They should
be self explanatory. It's totally not interesting to arbitrarily invent new
codes. Instead the web needs to be ready to react to these problems. When in
doubt use "internal-error".

 * "internal-error"
 * "no-cockpit"
 * "no-session"
 * "not-authorized"
 * "not-found"
 * "terminated"
 * "timeout"
 * "unknown-hostkey"
 * "no-forwarding"
