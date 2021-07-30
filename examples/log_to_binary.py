from socketsocketcan import TCPBus
import can
from datetime import datetime
from time import sleep, perf_counter
from queue import Queue
import struct

bus = TCPBus(5000)
print("socket connected!")

class BinaryLogger(can.Listener):
    """write each message as a 21 byte binary file"""
    def __init__(self,filename=None):
        if filename is None:
            filename = str(datetime.now()) + ".log"
        self.filename = filename
        self.buffer = Queue()
        self._count = 0
        self.total_count = 0

    def on_message_received(self,msg):
        self.buffer.put(msg)
        self._count += 1
        self.total_count += 1
        if self._count > 100:
            self.write_to_file()
            self._count = 0

    def on_error(self):
        self.write_to_file()

    def write_to_file(self):
        with open(self.filename,"ab") as fd:
            while not self.buffer.empty():
                msg = self.buffer.get()
                sec = int(msg.timestamp)
                usec = int((msg.timestamp - sec) * 1e6)
                data = msg.data + bytearray(8-msg.dlc)
                fd.write(struct.pack("<LLL9B",sec,usec,msg.arbitration_id,msg.dlc,*data))

logger = BinaryLogger()
filename = logger.filename
#create a listener to print all received messages
notifier = can.Notifier(bus,(logger,),timeout=None)

count = 0
msg = can.Message(
arbitration_id=0xBAD,
data=[0x0F,0x00,0x00,0x0D])
period = 0.05

try:
    while bus.is_connected:
        t0 = perf_counter()
        # bus.send(msg)
        count += 1
        sleep(period - (perf_counter() - t0))
    print("socket disconnected.")

except KeyboardInterrupt:
    print("ctrl+c, exiting...")

finally:
    notifier.stop()
    if bus.is_connected:
        bus.shutdown()
    logger.write_to_file()
    print(count,"messages sent")
    print("messages logged:",logger.total_count)

    with open(filename,"rb") as fd:
        ts_sent_prev = 0
        ts_sent_first = 0
        ts_sent_last = 0
        t0 = None
        count = 0
        sent_count = 0
        while True:
            row = fd.read(21)
            if len(row) == 0:
                break
            count += 1

            sec,usec,arb_id,dlc = struct.unpack("<LLLB",row[:13])
            ts = sec + (usec/1e6)
            if count == 1:
                t0 = ts
            if arb_id == 0xBAD:
                sent_count += 1
                if sent_count == 1:
                    ts_sent_prev = ts
                    ts_sent_first = ts
                else:
                    # print(ts - ts_sent_prev)
                    ts_sent_prev = ts
                    ts_sent_last = ts

        dt = ts - t0
        sent_dt = ts_sent_last - ts_sent_first
        print("{:d} total messages logged in {:0.3f}s. period: {:0.2f}ms".format(count,dt,1000*dt/count))
        if sent_count:
            print("{:d} sent messages logged in {:0.3f}s. period: {:0.2f}ms".format(sent_count,sent_dt,1000*sent_dt/sent_count))



