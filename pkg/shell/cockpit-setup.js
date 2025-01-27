/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
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

(function(cockpit, $) {

PageSetupServer.prototype = {
    _init: function() {
        this.id = "dashboard_setup_server_dialog";
    },

    getTitle: function() {
        return C_("page-title", "Setup Host");
    },

    show: function() {
        $("#dashboard_setup_address").focus();
    },

    leave: function() {
        $(this.local_client).off('.setup');
        this.local_client.release();
        this.local_client = null;
    },

    setup: function() {
        $('#dashboard_setup_cancel').on('click', $.proxy(this, 'cancel'));
        $('#dashboard_setup_prev').on('click', $.proxy(this, 'prev'));
        $('#dashboard_setup_next').on('click', $.proxy(this, 'next'));
    },

    highlight_error: function(container) {
        $(container).addClass("has-error");
    },

    hide_error: function(container) {
        $(container).removeClass("has-error");
    },

    highlight_error_message: function(id, message) {
        $(id).text(message);
        $(id).css("visibility", "visible");
    },

    hide_error_message: function(id) {
        $(id).css("visibility", "hidden");
    },

    check_empty_address: function() {
        var addr = $('#dashboard_setup_address').val();

        if (addr === "") {
            $('#dashboard_setup_next').prop('disabled', true);
            this.hide_error('#dashboard_setup_address_tab');
            this.hide_error_message('#dashboard_setup_address_error');
        } else if (addr.search(/\s+/) === -1) {
            $('#dashboard_setup_next').prop('disabled', false);
            this.hide_error('#dashboard_setup_address_tab');
            this.hide_error_message('#dashboard_setup_address_error');
        } else {
            $('#dashboard_setup_next').prop('disabled', true);
            this.highlight_error('#dashboard_setup_address_tab');
            this.highlight_error_message('#dashboard_setup_address_error',
                                         _("IP address or host name cannot contain whitespace."));
        }

        $('#dashboard_setup_next').text(_("Next"));
    },

    check_empty_name: function() {
        var name = $('#dashboard_setup_login_user').val();

        if (name === "") {
            this.name_is_done = false;
            $('#dashboard_setup_next').prop('disabled', true);
            this.hide_error('#login_user_cell');
            this.hide_error_message('#dashboard_setup_login_error');
        } else if (name.search(/\s+/) === -1) {
            this.name_is_done = true;
            $('#dashboard_setup_next').prop('disabled', false);
            this.hide_error('#login_user_cell');
            this.hide_error_message('#dashboard_setup_login_error');
        } else {
            this.name_is_done = false;
            $('#dashboard_setup_next').prop('disabled', true);
            this.highlight_error('#login_user_cell');
            this.highlight_error_message('#dashboard_setup_login_error',
                                         _("User name cannot contain whitespace."));
        }

        $('#dashboard_setup_next').text(_("Next"));
    },

    enter: function() {
        var self = this;

        self.client = null;
        self.address = null;
        self.options = { "host-key": "", "payload": "dbus-json1" };
        self.name_is_done = false;

        $("#dashboard_setup_address")[0].placeholder = _("Enter IP address or host name");
        $('#dashboard_setup_address').on('keyup change', $.proxy(this, 'update_discovered'));
        $('#dashboard_setup_address').on('input change focus', $.proxy(this, 'check_empty_address'));
        $('#dashboard_setup_login_user').on('input change focus', $.proxy(this, 'check_empty_name'));
        $('#dashboard_setup_login_password').on('input focus', function() {
            if (self.name_is_done)
                self.hide_error_message('#dashboard_setup_login_error');
        });
        $('#dashboard_setup_address').on('keyup', function(event) {
            if (event.which === 13) {
                var disable = $('#dashboard_setup_next').prop('disabled');

                if (!disable)
                    self.next();
            }
        });
        $('#dashboard_setup_login_user').on('keyup', function(event) {
            if (event.which === 13)
                $("#dashboard_setup_login_password").focus();
        });
        $('#dashboard_setup_login_password').on('keyup', function(event) {
            if (event.which === 13) {
                var disable = $('#dashboard_setup_next').prop('disabled');

                if (!disable)
                    self.next();
            }
        });

        /* TODO: This code needs to be migrated away from dbus-json1 */
        self.local_client = cockpit.dbus("localhost", { payload: 'dbus-json1' });
        $(self.local_client).on('objectAdded.setup objectRemoved.setup', function(event, object) {
            if (object.lookup('com.redhat.Cockpit.Machine'))
                self.update_discovered();
        });
        $(self.local_client).on('propertiesChanged.setup', function(event, object, iface) {
            if (iface._iface_name == "com.redhat.Cockpit.Machine")
                self.update_discovered();
        });

        $('#dashboard_setup_address').val("");
        $('#dashboard_setup_login_user').val("");
        $('#dashboard_setup_login_password').val("");

        $('#dashboard_setup_address_reuse_creds').prop('checked', true);

        self.show_tab('address');
        self.update_discovered();
        $('#dashboard_setup_next').prop('disabled', true);
    },

    update_discovered: function() {
        var filter = $('#dashboard_setup_address').val();
        var discovered = $('#dashboard_setup_address_discovered');
        var machines = this.local_client.getInterfacesFrom("/com/redhat/Cockpit/Machines",
                                                           "com.redhat.Cockpit.Machine");

        function render_address(address) {
            if (!filter)
                return $('<span/>').text(address);
            var index = address.indexOf(filter);
            if (index == -1)
                return null;
            return $('<span/>').append(
                $('<span/>').text(address.substring(0,index)),
                $('<b/>').text(address.substring(index,index+filter.length)),
                $('<span/>').text(address.substring(index+filter.length)));
        }

        discovered.empty();
        for (var i = 0; i < machines.length; i++) {
            if (!cockpit.find_in_array(machines[i].Tags, "dashboard")) {
                var rendered_address = render_address(machines[i].Address);
                if (rendered_address) {
                    if (machines[i].Address.trim() !== "") {
                        var item =
                            $('<li>', { 'class': 'list-group-item',
                                        'on': { 'click': $.proxy(this, 'discovered_clicked', machines[i])
                                              }
                                      }).html(rendered_address);
                        discovered.append(item);
                    }
                }
            }
        }
    },

    discovered_clicked: function(iface) {
        $("#dashboard_setup_address").val(iface.Address);
        this.update_discovered();
        $("#dashboard_setup_address").focus();
    },

    show_tab: function(tab) {
        $('.cockpit-setup-tab').hide();
        $('#dashboard_setup_next').text(_("Next"));
        if (tab == 'address') {
            $('#dashboard_setup_address_tab').show();
            $("#dashboard_setup_address").focus();
            this.hide_error_message('#dashboard_setup_address_error');
            this.next_action = this.next_select;
            this.prev_tab = null;
        } else if (tab == 'login') {
            $('#dashboard_setup_login_tab').show();
            $('#dashboard_setup_login_user').focus();
            this.hide_error_message('#dashboard_setup_login_error');
            this.next_action = this.next_login;
            this.prev_tab = 'address';
        } else if (tab == 'action') {
            $('#dashboard_setup_action_tab').show();
            $('#dashboard_setup_next').text(_("Add host"));
            this.next_action = this.next_setup;
            var reuse = $('#dashboard_setup_address_reuse_creds').prop('checked');
            if (reuse)
                this.prev_tab = 'address';
            else
                this.prev_tab = 'login';
        } else if (tab == 'close') {
            $('#dashboard_setup_action_tab').show();
            $('#dashboard_setup_next').text(_("Close"));
            this.next_action = this.next_close;
            this.prev_tab = null;
        }

        if (this.next_action === this.next_login)
            this.check_empty_name();
        else
            $('#dashboard_setup_next').prop('disabled', false);
        $('#dashboard_setup_prev').prop('disabled', !this.prev_tab);
    },

    close: function() {
        if (this.client)
            this.client.close("cancelled");
        $("#dashboard_setup_server_dialog").modal('hide');
    },

    cancel: function() {
        this.close();
    },

    prev: function() {
        if (this.prev_tab)
            this.show_tab(this.prev_tab);
    },

    next: function() {
        $('#dashboard_setup_next').html('<div class="waiting"/>');
        $('#dashboard_setup_next').prop('disabled', true);
        this.next_action();
    },

    connect_server: function() {
        /* This function tries to connect to the server in
         * 'this.address' with 'this.options' and does the right thing
         * depending on the result.
         */

        var self = this;

        /* TODO: This is using the old dbus-json1 protocol */
        var client = cockpit.dbus(self.address, self.options);
        $(client).on('state-change', function() {
            if (client.state == "closed") {
                if (!self.options["host_key"] && client.error == "unknown-hostkey") {
                    /* The host key is unknown.  Remember it and try
                     * again while allowing that one host key.  When
                     * the user confirms the host key eventually, we
                     * store it permanently.
                     */
                    self.options["host-key"] = client.error_details["host-key"];
                    $('#dashboard_setup_action_fingerprint').text(client.error_details["host-fingerprint"]);
                    self.connect_server();
                    return;
                } else if (client.error == "not-authorized") {
                    /* The given credentials didn't work.  Ask the
                     * user to try again.
                     */
                    self.show_tab('login');
                    self.highlight_error_message('#dashboard_setup_login_error',
                                                 cockpit.client_error_description(client.error));
                    return;
                }

                /* The connection has failed.  Show the error on every
                 * tab but stay on the current tab.
                 */
                self.highlight_error_message('#dashboard_setup_address_error',
                                             cockpit.client_error_description(client.error));
                self.highlight_error_message('#dashboard_setup_login_error',
                                             cockpit.client_error_description(client.error));

                $('#dashboard_setup_next').prop('disabled', false);
                $('#dashboard_setup_next').text(_("Next"));

                return;

            } else if (client.state == "ready") {
                /* We are in.  Start the setup.
                 */
                self.client = client;
                self.prepare_setup();
            }
        });
    },

    next_select: function() {
        var me = this;
        var reuse_creds;

        me.hide_error_message('#dashboard_setup_address_error');

        me.address = $('#dashboard_setup_address').val();

        if (me.address.trim() !== "") {
            $('#dashboard_setup_login_address').text(me.address);

            reuse_creds = $('#dashboard_setup_address_reuse_creds').prop('checked');

            if (!reuse_creds)
                me.show_tab('login');
            else {
                me.options.user = null;
                me.options.password = null;
                me.options.host_key = null;
                me.connect_server();
            }
        } else {
            $('#dashboard_setup_next').text(_("Next"));
            me.highlight_error_message('#dashboard_setup_address_error',
                                       _("IP address or host name cannot be empty."));
        }
    },

    next_login: function() {
        var me = this;

        var user = $('#dashboard_setup_login_user').val();
        var pass = $('#dashboard_setup_login_password').val();

        me.hide_error_message('#dashboard_setup_login_error');

        me.options.user = user;
        me.options.password = pass;

        if (user.trim() !== "") {
            me.connect_server();
        } else {
            $('#dashboard_setup_next').text(_("Next"));
            me.highlight_error_message('#dashboard_setup_login_error',
                                       _("User name cannot be empty."));
        }
    },

    reset_tasks: function() {
        var $tasks = $('#dashboard_setup_action_tasks');

        this.tasks = [];
        $tasks.empty();
    },

    add_task: function(desc, func) {
        var $tasks = $('#dashboard_setup_action_tasks');

        var $entry = $('<li/>', { 'class': 'list-group-item' }).append(
            $('<table/>', { 'class': "cockpit-setup-task-table",
                            'style': "width:100%" }).append(
                $('<tr/>').append(
                    $('<td/>').text(
                        desc),
                    $('<td style="width:16px"/>').append(
                        $('<div>',  { 'class': "cockpit-setup-task-spinner waiting",
                                      'style': "display:none"
                                    }),
                        $('<img/>', { 'class': "cockpit-setup-task-error",
                                      'src': "/cockpit/@@shell@@/images/dialog-error.png",
                                      'style': "display:none"
                                    }),
                        $('<img/>', { 'class': "cockpit-setup-task-done",
                                      'src': "/cockpit/@@shell@@/images/face-smile.png",
                                      'style': "display:none"
                                    })))));

        var task = { entry: $entry,
                     func: func,
                     error: function(msg) {
                         this.had_error = true;
                         this.entry.find(".cockpit-setup-task-table").append(
                             $('<tr/>').append(
                                 $('<td/>', { 'style': "color:red" }).text(msg)));
                     }
                   };

        this.tasks.push(task);
        $tasks.append($entry);
    },

    run_tasks: function(done) {
        var me = this;

        function run(i) {
            var t;

            if (i < me.tasks.length) {
                t = me.tasks[i];
                t.entry.find(".cockpit-setup-task-spinner").show();
                t.func(t, function() {
                    t.entry.find(".cockpit-setup-task-spinner").hide();
                    if (t.had_error)
                        t.entry.find(".cockpit-setup-task-error").show();
                    else
                        t.entry.find(".cockpit-setup-task-done").show();
                    run(i+1);
                });
            } else
                done();
        }

        run(0);
    },

    prepare_setup: function() {
        var me = this;

        function get_role_accounts(client, roles) {
            var i;
            var accounts = client.getInterfacesFrom("/com/redhat/Cockpit/Accounts/",
                                                    "com.redhat.Cockpit.Account");
            var map = { };

            function groups_contain_roles(groups) {
                for (var i = 0; i < roles.length; i++) {
                    if (cockpit.find_in_array(groups, roles[i][0]))
                        return true;
                }
                return false;
            }

            for (i = 0; i < accounts.length; i++) {
                if (roles === null || groups_contain_roles(accounts[i].Groups))
                    map[accounts[i].UserName] = accounts[i];
            }
            return map;
        }

        var manager = this.local_client.lookup("/com/redhat/Cockpit/Accounts",
                                               "com.redhat.Cockpit.Accounts");
        var local = get_role_accounts(this.local_client, manager.Roles);
        var remote = get_role_accounts(this.client, null);

        function needs_update(l) {
            // XXX
            return true;
        }

        function update_account(templ, task, done) {

            var acc;

            function std_cont(func) {
                return function(error, result) {
                    if (error) {
                        task.error(error.message);
                        done();
                    } else {
                        func(result);
                    }
                };
            }

            function create() {
                var accounts = me.client.getInterfacesFrom("/com/redhat/Cockpit/Accounts",
                                                           "com.redhat.Cockpit.Account");

                for (var i = 0; i < accounts.length; i++) {
                    if (accounts[i].UserName == templ.UserName) {
                        acc = accounts[i];
                        set_groups();
                        return;
                    }
                }

                var manager = me.client.lookup("/com/redhat/Cockpit/Accounts",
                                               "com.redhat.Cockpit.Accounts");
                manager.call("CreateAccount",
                             templ.UserName,
                             templ.RealName,
                             "",
                             false,
                             std_cont(set_groups_path));
            }

            function set_groups_path(path) {
                acc = me.client.lookup(path, "com.redhat.Cockpit.Account");
                if (!acc) {
                    task.error("Account object not found");
                    done();
                } else
                    set_groups();
            }

            function set_groups() {
                // XXX - filter out non-role groups and groups that
                //       don't exist on the target
                acc.call('ChangeGroups', templ.Groups, [ ],
                         std_cont(get_avatar));
            }

            function get_avatar() {
                templ.call('GetIconDataURL',
                           std_cont(set_avatar));
            }

            function set_avatar(dataurl) {
                if (dataurl) {
                    acc.call('SetIconDataURL', dataurl,
                             std_cont(get_password_hash));
                } else
                    get_password_hash();
            }

            function get_password_hash() {
                templ.call('GetPasswordHash',
                           std_cont(set_password_hash));
            }

            function set_password_hash(hash) {
                acc.call('SetPasswordHash', hash,
                         std_cont(done));
            }

            create();
        }

        this.reset_tasks();

        function create_update_task(templ) {
            return function(task, done) {
                update_account(templ, task, done);
            };
        }

        for (var l in local) {
            if (needs_update(l))
                this.add_task("Synchronize administrator " + local[l].UserName,
                              create_update_task(local[l]));
        }

        $('#dashboard_setup_action_address').text(this.address);

        this.show_tab('action');
    },

    next_setup: function() {
        var me = this;

        /* We can only add the machine to the list of known machines
         * here since doing so also stores its key as 'known good',
         * and we need the users permission for this.
         *
         * TODO: Add a method to set only the key and use it here.
         */

        var machines = this.local_client.lookup("/com/redhat/Cockpit/Machines",
                                                "com.redhat.Cockpit.Machines");
        machines.call('Add', me.address, me.options["host-key"], function(error, path) {
            if (error) {
                me.highlight_error_message('#dashboard_setup_address_error', error.message);
                me.show_tab('address');
                return;
            }

            me.machine = me.local_client.lookup(path, "com.redhat.Cockpit.Machine");
            if (!me.machine) {
                me.highlight_error_message('#dashboard_setup_address_error',
                                           _("New machine not found in list after adding."));
                me.show_tab('address');
                return;
            }

            me.run_tasks(function() {
                me.machine.call('AddTag', "dashboard", function(error) {
                    if (error)
                        cockpit.show_unexpected_error(error);
                    me.show_tab('close');
                });
            });
        });
    },

    next_close: function() {
        this.close();
    }

};

function PageSetupServer() {
    this._init();
}

cockpit.pages.push(new PageSetupServer());

})(cockpit, $);
