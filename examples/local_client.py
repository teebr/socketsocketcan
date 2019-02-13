from socketsocketcan import TCPBus, TCPClient

client = TCPClient('can0', 'localhost', 5000)
print("client started")

bus = TCPBus(5000)
print("socket connected!")

try:
    for msg in bus:
        print(msg)
except KeyboardInterrupt:
    print("ctrl+c, exiting...")
    client.shutdown()
    if bus.is_connected:
        bus.shutdown()
