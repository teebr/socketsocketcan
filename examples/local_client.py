from socketsocketcan import TCPBus, TCPClient

can_filters = [{'can_id': 0, 'can_mask': 0}]
client = TCPClient('can0', 'localhost', 5000, can_filters, use_unordered_map=False, limit_recv_rate_hz=None)
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
