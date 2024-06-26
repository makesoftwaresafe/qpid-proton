#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

import os

from .common import Test, Skipped, free_tcp_ports, \
    MessengerReceiverC, MessengerSenderC, \
    ReactorReceiverC, ReactorSenderC, \
    isSSLPresent

#
# Tests that run the apps
#


class AppTests(Test):

    def __init__(self, *args):
        Test.__init__(self, *args)
        self.is_valgrind = False

    def default(self, name, value, **kwargs):
        if self.is_valgrind:
            default = kwargs.get("valgrind", value)
        else:
            default = value
        return Test.default(self, name, default, **kwargs)

    @property
    def iterations(self):
        return int(self.default("iterations", 2, fast=1, valgrind=2))

    @property
    def send_count(self):
        return int(self.default("send_count", 17, fast=1, valgrind=2))

    @property
    def target_count(self):
        return int(self.default("target_count", 5, fast=1, valgrind=2))

    @property
    def send_batch(self):
        return int(self.default("send_batch", 7, fast=1, valgrind=2))

    @property
    def forward_count(self):
        return int(self.default("forward_count", 5, fast=1, valgrind=2))

    @property
    def port_count(self):
        return int(self.default("port_count", 3, fast=1, valgrind=2))

    @property
    def sender_count(self):
        return int(self.default("sender_count", 3, fast=1, valgrind=2))

    def valgrind_test(self):
        self.is_valgrind = True

    def setUp(self):
        self.senders = []
        self.receivers = []

    def tearDown(self):
        pass

    def _do_test(self, iterations=1):
        verbose = self.verbose

        for R in self.receivers:
            R.start(verbose)

        for j in range(iterations):
            for S in self.senders:
                S.start(verbose)

            for S in self.senders:
                S.wait()
                # print("SENDER OUTPUT:")
                # print( S.stdout() )
                assert S.status() == 0, ("Command '%s' failed status=%d: '%s' '%s'"
                                         % (str(S.cmdline()),
                                            S.status(),
                                            S.stdout(),
                                            S.stderr()))

        for R in self.receivers:
            R.wait()
            # print("RECEIVER OUTPUT")
            # print( R.stdout() )
            assert R.status() == 0, ("Command '%s' failed status=%d: '%s' '%s'"
                                     % (str(R.cmdline()),
                                        R.status(),
                                        R.stdout(),
                                        R.stderr()))

#
# Traffic passing tests based on the Messenger apps
#


class MessengerTests(AppTests):

    _timeout = 60

    def _ssl_check(self):
        if not isSSLPresent():
            raise Skipped("No SSL libraries found.")
        if os.name == "nt":
            raise Skipped("Windows SChannel lacks anonymous cipher support.")

    def __init__(self, *args):
        AppTests.__init__(self, *args)

    def _do_oneway_test(self, receiver, sender, domain="amqp"):
        """ Send N messages to a receiver.
        Parameters:
        iterations - repeat the senders this many times
        target_count = # of targets to send to.
        send_count = # messages sent to each target
        """
        iterations = self.iterations
        send_count = self.send_count
        target_count = self.target_count

        send_total = send_count * target_count
        receive_total = send_total * iterations

        port = free_tcp_ports()[0]

        receiver.subscriptions = ["%s://~0.0.0.0:%s" % (domain, port)]
        receiver.receive_count = receive_total
        receiver.timeout = MessengerTests._timeout
        self.receivers.append(receiver)

        sender.targets = ["%s://0.0.0.0:%s/X%d" % (domain, port, j) for j in range(target_count)]
        sender.send_count = send_total
        sender.timeout = MessengerTests._timeout
        self.senders.append(sender)

        self._do_test(iterations)

    def _do_echo_test(self, receiver, sender, domain="amqp"):
        """ Send N messages to a receiver, which responds to each.
        Parameters:
        iterations - repeat the senders this many times
        target_count - # targets to send to
        send_count = # messages sent to each target
        send_batch - wait for replies after this many messages sent
        """
        iterations = self.iterations
        send_count = self.send_count
        target_count = self.target_count
        send_batch = self.send_batch

        send_total = send_count * target_count
        receive_total = send_total * iterations

        port = free_tcp_ports()[0]

        receiver.subscriptions = ["%s://~0.0.0.0:%s" % (domain, port)]
        receiver.receive_count = receive_total
        receiver.send_reply = True
        receiver.timeout = MessengerTests._timeout
        self.receivers.append(receiver)

        sender.targets = ["%s://0.0.0.0:%s/%dY" % (domain, port, j) for j in range(target_count)]
        sender.send_count = send_total
        sender.get_reply = True
        sender.send_batch = send_batch
        sender.timeout = MessengerTests._timeout
        self.senders.append(sender)

        self._do_test(iterations)

    # Removed messenger "relay" tests. The test start-up is faulty:
    # msgr-recv prints it's -X ready message when it starts to open a
    # connection but it does not wait for the remote open. The relay
    # tests depends on mapping a container name from an incoming
    # connection. They can fail under if the sender starts before the
    # connection is complete (esp. valgrind with SSL connections) We
    # could fix the tests but since messenger is deprecated it does
    # not seem worthwhile.

    def _do_star_topology_test(self, r_factory, s_factory, domain="amqp"):
        """
        A star-like topology, with a central receiver at the hub, and senders at
        the spokes.  Each sender will connect to each of the ports the receiver is
        listening on.  Each sender will then create N links per each connection.
        Each sender will send X messages per link, waiting for a response.
        Parameters:
        iterations - repeat the senders this many times
        port_count - # of ports the receiver will listen on.  Each sender connects
                     to all ports.
        sender_count - # of senders
        target_count - # of targets per connection
        send_count - # of messages sent to each target
        send_batch - # of messages to send before waiting for response
        """
        iterations = self.iterations
        port_count = self.port_count
        sender_count = self.sender_count
        target_count = self.target_count
        send_count = self.send_count
        send_batch = self.send_batch

        send_total = port_count * target_count * send_count
        receive_total = send_total * sender_count * iterations

        ports = free_tcp_ports(port_count)

        receiver = r_factory()
        receiver.subscriptions = ["%s://~0.0.0.0:%s" % (domain, port) for port in ports]
        receiver.receive_count = receive_total
        receiver.send_reply = True
        receiver.timeout = MessengerTests._timeout
        self.receivers.append(receiver)

        for i in range(sender_count):
            sender = s_factory()
            sender.targets = ["%s://0.0.0.0:%s/%d" % (domain, port, j) for port in ports for j in range(target_count)]
            sender.send_count = send_total
            sender.send_batch = send_batch
            sender.get_reply = True
            sender.timeout = MessengerTests._timeout
            self.senders.append(sender)

        self._do_test(iterations)

    def test_oneway_C(self):
        self._do_oneway_test(MessengerReceiverC(), MessengerSenderC())

    def test_oneway_C_SSL(self):
        self._ssl_check()
        self._do_oneway_test(MessengerReceiverC(), MessengerSenderC(), "amqps")

    def test_echo_C(self):
        self._do_echo_test(MessengerReceiverC(), MessengerSenderC())

    def test_echo_C_SSL(self):
        self._ssl_check()
        self._do_echo_test(MessengerReceiverC(), MessengerSenderC(), "amqps")

    def test_star_topology_C(self):
        self._do_star_topology_test(MessengerReceiverC, MessengerSenderC)

    def test_star_topology_C_SSL(self):
        self._ssl_check()
        self._do_star_topology_test(MessengerReceiverC, MessengerSenderC, "amqps")

    def test_oneway_reactor(self):
        self._do_oneway_test(ReactorReceiverC(), ReactorSenderC())
