# SocketCAN over TCP

#### Original author Thomas Bruen
#### Modified by Kristian Lauszus, 2019-2020
_________

[![](https://github.com/Lauszus/socketsocketcan/workflows/socketsocketcan%20CI/badge.svg?branch=pybind11)](https://github.com/Lauszus/socketsocketcan/actions?query=branch%3Apybind11)

For when you want to use the [Python CAN package](https://github.com/hardbyte/python-can) but are running on a resource-constrained device, such as a Raspberry Pi.

I've found that for a busy bus (> 1000 messages/ second), my Raspberry Pi was dropping messages: we'd reached the limit of what Python could achieve. However, it's still a really powerful library for processing messages and integrating other services.

This package lets you run `python-can` on a more powerful machine, by setting up a TCP connection to the Linux device. A small C client runs on the Linux device to send/ receive CAN messages (with timestamp). This is perfect for listening to a bus and sending occasional messages. I haven't tested this\* with anything that demands high-frequency periodic sending of messages, but using the CAN broadcast manager will probably be better than using this directly.

\* *I haven't tested this much at all. Sorry. I did have 3 terminals running `cangen` at a 1ms interval for 10 minutes, and didn't have any issues (it was identical to `candump` except where `candump` locked up and dropped a few hundred messages).*

# CAN Set-up
Enable `vcan` or `can0` (or whichever) on the Linux device:

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up
```

or

```bash
sudo ip link set can0 up type can bitrate 1000000
```

# Installation
## Client
Copy the `client` folder to your Linux device and run `make`

By default the client is set to receive the message you've sent (personally I've found this really useful for logging because the timestamps are aligned), but you can disable it in `client.c`: `#define RECV_OWN_MSGS 0`

## Server
Using a virtual environment is really recommended here.

```bash
pip install -e path/to/socketsocketcan-repo
```

# Usage
On creating a `TCPBus` object, it will block until a connection with the client is made (i.e. start the client now). Then, you can use the bus to `send()` and `recv()` messages as usual. By default, the bus accepts connections from any host, you can limit this using the `hostname="A_PARTICULAR_HOSTNAME"`keyword arguent.

The `TCPBus` runs a couple of threads in the background: one to write 'sent' messages to the socket, so that the client can read them in and actually put them on the CAN bus, and another to receive and messages from the client and put them in a queue so the `recv` method can retrieve them.

As the quotes above impied, calling `send()` doesn't actually send the message. It puts the message on a queue, which a thread will write to the socket as soon as possible. (I haven't looked at how long it actually takes from putting it on the queue and it actually being sent: it will vary depending on resources and you'd have to make sure the clocks on the two computers are synchronised to the sub millisecond level.)

The client has a similar structure: there is one thread to poll the CAN bus for new messages and put them in a buffer, a second thread to copy from that buffer to the TCP socket, and a third to read from the TCP socket and send the messages via the CAN Bus.

## Client
`./client CANCHANNEL HOSTNAME PORT`
e.g. `./client vcan0 my-can-server 5000`

## Server

```python
from socketsocketcan import TCPBus
from can import Message
bus = TCPBus(5000) #start the client now.
bus.send(Message(arbitration_id=0x100,data=list(range(5))))
print(bus.recv()) #this will be the message you just send (unless)
```

