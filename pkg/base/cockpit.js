/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

var cockpit = cockpit || { };
var mock = mock || { };

var phantom_checkpoint = phantom_checkpoint || function () { };

(function(cockpit, $) {
"use strict";

/*
 * User and system information
 */

cockpit.user = { };
cockpit.info = { };

/*
 * TODO: We will expose standard client side url composability
 * utilities soon, for now this is private.
 */

function get_page_param(key, page) {

    /*
     * HACK: Mozilla will unescape 'window.location.hash' before returning
     * it, which is broken.
     *
     * https://bugzilla.mozilla.org/show_bug.cgi?id=135309
     */
    var hash = (window.location.href.split('#')[1] || '');

    var trail = [ ];
    var locs = hash.split('&');
    var params, i, j, p, vals;
    for (i = 0; i < locs.length; i++) {
        params = locs[i].split('?');
        p = { page: decodeURIComponent(params[0]) };
        for (j = 1; j < params.length; j++) {
            vals = params[j].split('=');
            p[decodeURIComponent(vals[0])] = decodeURIComponent(vals[1]);
        }
        trail.push(p);
    }

    var index = trail.length - 1;
    if (page) {
        while (index >= 0 && trail[index].page != page)
            index--;
    }
    if (index >= 0)
        return trail[index][key];
    else
        return undefined;
}

/*
 * Channels
 *
 * Public: https://files.cockpit-project.org/guide/api-cockpit.html
 */

var default_transport = null;
var reload_after_disconnect = false;
var expect_disconnect = false;

$(window).on('beforeunload', function() { expect_disconnect = true; });

function transport_debug() {
    if (cockpit.debugging == "all" || cockpit.debugging == "channel")
        console.debug.apply(console, arguments);
}

function calculate_url() {
    if (window.mock && window.mock.url)
        return window.mock.url;
    var window_loc = window.location.toString();
    if (window_loc.indexOf('http:') === 0) {
        return "ws://" + window.location.host + "/socket";
    } else if (window_loc.indexOf('https:') === 0) {
        return "wss://" + window.location.host + "/socket";
    } else {
        console.error("Cockpit must be used over http or https");
        return null;
    }
}

/* Private Transport class */
function Transport() {
    var self = this;

    var last_channel = 0;
    var channel_seed = "";

    var ws_loc = calculate_url();
    if (!ws_loc)
        return;

    if (window.mock)
        window.mock.last_transport = self;

    transport_debug("Connecting to " + ws_loc);

    var ws;
    if ("WebSocket" in window) {
        ws = new WebSocket(ws_loc, "cockpit1");
    } else if ("MozWebSocket" in window) { // Firefox 6
        ws = new MozWebSocket(ws_loc);
    } else {
        console.error("WebSocket not supported, application will not work!");
        return;
    }

    var control_cbs = { };
    var message_cbs = { };
    var got_message = false;
    var waiting_for_init = true;
    self.ready = false;

    var check_health_timer = window.setInterval(function () {
        if (!got_message) {
            console.log("health check failed");
            self.close({ "reason": "timeout" });
        }
        got_message = false;
    }, 10000);

    /* Called when ready for channels to interact */
    function ready_for_channels() {
        if (!self.ready) {
            self.ready = true;
            $(self).triggerHandler("ready");
        }
    }

    ws.onopen = function() {
        if (ws) {
            ws.send("\n{ \"command\": \"init\", \"version\": 0 }");
        }
    };

    ws.onclose = function(event) {
        transport_debug("WebSocket onclose");
        ws = null;
        if (reload_after_disconnect) {
            expect_disconnect = true;
            window.location.reload(true);
        }
        if (expect_disconnect)
            return;
        self.close();
    };

    ws.onmessage = function(event) {
        got_message = true;

        /* The first line of a message is the channel */
        var data = event.data;
        var pos = data.indexOf("\n");
        var channel = data.substring(0, pos);
        var payload = data.substring(pos + 1);
        if (!channel) {
            transport_debug("recv control:", payload);
            process_control(JSON.parse(payload));
        } else {
            transport_debug("recv " + channel + ":", payload);
            process_message(channel, payload);
        }
        phantom_checkpoint();
    };

    self.close = function close(options) {
        if (self === default_transport)
            default_transport = null;
        if (!options)
            options = { "reason": "disconnected" };
        options.command = "close";
        clearInterval(check_health_timer);
        var ows = ws;
        ws = null;
        if (ows)
            ows.close();
        ready_for_channels(); /* ready to fail */
        process_control(options);
    };

    self.next_channel = function next_channel() {
        last_channel++;
        return String(last_channel) + channel_seed;
    };

    function process_init(options) {
        if (options.version !== 0) {
            console.error("received invalid version in init message");
            self.close({"reason": "protocol-error"});
            return;
        }

        if (options["channel-seed"])
            channel_seed = ":" + String(options["channel-seed"]);
        if (options.user)
            $.extend(cockpit.user, options.user);
        if (options.system)
            $.extend(cockpit.info, options.system);

        if (waiting_for_init) {
            waiting_for_init = false;
            ready_for_channels();
        }

        if (options.user)
            $(cockpit.user).trigger("changed");
        if (options.system)
            $(cockpit.info).trigger("changed");
    }

    function process_control(data) {
        var channel = data.channel;
        var func;

        /* Init message received */
        if (data.command == "init") {
            process_init(data);
            return;
        }

        if (waiting_for_init) {
            waiting_for_init = false;
            if (data.command != "close" || data.channel) {
                console.error ("received message before init");
                data = { "reason": "protocol-error" };
            }
            self.close(data);
            return;
        }

        /* 'ping' messages are ignored */
        if (data.command == "ping")
            return;

        /* Broadcast to everyone if no channel */
        if (channel === undefined) {
            for (var chan in control_cbs) {
                func = control_cbs[chan];
                func.apply(null, [data]);
            }
        } else {
            func = control_cbs[channel];
            if (func)
                func.apply(null, [data]);
        }
    }

    function process_message(channel, payload) {
        var func = message_cbs[channel];
        if (func)
            func.apply(null, [payload]);
    }

    self.send_message = function send_message(channel, payload) {
        if (!ws) {
            console.log("transport closed, dropped message: " + payload);
            return;
        }
        if (channel)
            transport_debug("send " + channel + ":", payload);
        else
            transport_debug("send control:", payload);
        var msg = channel.toString() + "\n" + payload;
        ws.send(msg);
    };

    self.send_control = function send_control(data) {
        if(!ws && data.command == "close")
            return; /* don't complain if closed and closing */
        self.send_message("", JSON.stringify(data));
    };

    self.register = function register(channel, control_cb, message_cb) {
        control_cbs[channel] = control_cb;
        message_cbs[channel] = message_cb;
    };

    self.unregister = function unregister(channel) {
        delete control_cbs[channel];
        delete message_cbs[channel];
    };
}

function ensure_transport(callback) {
    var transport;
    if (!default_transport)
        default_transport = new Transport();
    transport = default_transport;
    if (transport.ready) {
        callback(transport);
    } else {
        $(default_transport).on("ready", function() {
            callback(transport);
        });
    }
}

function Channel(options) {
    var self = this;

    var transport;
    var valid = true;
    var queue = [ ];
    var id = null;

    /* Handy for callers, but not used by us */
    self.valid = valid;
    self.options = options;
    self.id = id;

    function on_message(payload) {
        $(self).triggerHandler("message", payload);
    }

    function on_control(data) {
        if (data.command == "close") {
            self.valid = valid = false;
            transport.unregister(id);
            $(self).triggerHandler("close", data);
        } else {
            console.log("unhandled control message: '" + data.command + "'");
        }
    }

    ensure_transport(function(trans) {
        transport = trans;
        if (!valid)
            return;

        id = transport.next_channel();
        self.id = id;

        /* Register channel handlers */
        transport.register(id, on_control, on_message);

        /* Now open the channel */
        var command = {
            "command" : "open",
            "channel": id
        };
        $.extend(command, options);
        if (command.host === undefined)
            command.host = get_page_param('machine', 'server');
        transport.send_control(command);

        /* Now drain the queue */
        while(queue.length > 0)
            transport.send_message(id, queue.shift());
    });

    self.send = function send(message) {
        if (!valid)
            console.log("sending message on closed channel: " + self);
        else if (!transport)
            queue.push(message);
        else
            transport.send_message(id, message);
    };

    self.close = function close(options) {
        self.valid = valid = false;
        if (!options)
            options = { };
        else if (!$.isPlainObject(options))
            options = { "reason" : options + "" };
        $.extend(options, {
            "command" : "close",
            "channel": id
        });
        if (transport) {
            transport.send_control(options);
            transport.unregister(id);
        }
        $(self).triggerHandler("close", options);
    };

    self.toString = function toString() {
        var host = options["host"] || "localhost";
        return "[Channel " + (valid ? id : "<invalid>") + " -> " + host + "]";
    };
}

cockpit.channel = function channel(options) {
    return new Channel(options);
};

cockpit.logout = function logout(reload) {
    if (reload !== false)
        reload_after_disconnect = true;
    ensure_transport(function(transport) {
        transport.send_control({ "command": "logout", "disconnect": true });
    });
};

/* Not public API ... yet? */
cockpit.drop_privileges = function drop_privileges() {
    ensure_transport(function(transport) {
        transport.send_control({ "command": "logout", "disconnect": false });
    });
};

cockpit.transport = {
    close: function close(reason) {
        if (!default_transport)
            return;
        var options;
        if (reason)
            options = {"reason": reason };
        default_transport.close(options);
    }
};

/*
 * Spawning
 *
 * Public: https://files.cockpit-project.org/guide/api-cockpit.html
 */

function ProcessError(arg0, signal) {
    var status = parseInt(arg0, 10);
    if (arg0 !== undefined && isNaN(status)) {
        this.problem = arg0;
        this.exit_status = NaN;
        this.exit_signal = null;
        this.message = arg0;
    } else {
        this.exit_status = status;
        this.exit_signal = signal;
        this.problem = null;
        if (this.exit_signal)
            this.message = "Process killed with signal " + this.exit_signal;
        else
            this.message = "Process exited with code " + this.exit_status;
    }
    this.toString = function() {
        return this.message;
    };
}

function spawn_debug() {
    if (cockpit.debugging == "all" || cockpit.debugging == "spawn")
        console.debug.apply(console, arguments);
}

/* public */
cockpit.spawn = function(command, options) {
    var dfd = new $.Deferred();

    var args = { "payload": "text-stream", "spawn": [] };
    if (command instanceof Array) {
        for (var i = 0; i < command.length; i++)
            args["spawn"].push(String(command[i]));
    } else {
        args["spawn"].push(String(command));
    }
    if (options !== undefined)
        $.extend(args, options);

    var channel = cockpit.channel(args);

    /* Callbacks that want to stream response, see below */
    var streamers = null;

    var buffer = "";
    $(channel).
        on("message", function(event, payload) {
            spawn_debug("process output:", payload);
            buffer += payload;
            if (streamers && buffer) {
                streamers.fire(buffer);
                buffer = "";
            }
        }).
        on("close", function(event, options) {
            spawn_debug("process closed:", JSON.stringify(options));
            if (options.reason)
                dfd.reject(new ProcessError(options.reason));
            else if (options["exit-status"] || options["exit-signal"])
                dfd.reject(new ProcessError(options["exit-status"], options["exit-signal"]));
            else
                dfd.resolve(buffer);
        });

    var promise = dfd.promise();
    promise.stream = function(callback) {
        if (streamers === null)
           streamers = $.Callbacks("" /* no flags */);
        streamers.add(callback);
        return this;
    };

    promise.write = function(message) {
        spawn_debug("process input:", message);
        channel.send(message);
        return this;
    };

    promise.close = function(reason) {
        spawn_debug("process closing:", reason);
        if (channel.valid)
            channel.close(reason);
        return this;
    };

    return promise;
};

}(cockpit, jQuery));
