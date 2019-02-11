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
                try:
                    data = msg.data + bytearray(8-msg.dlc)
                except:
                    print("negative size?",msg.dlc,8-msg.dlc)
                    data = bytearray(8)
                fd.write(struct.pack("<LLL9B",sec,usec,msg.arbitration_id,msg.dlc,*data))

    
logger = BinaryLogger()
filename = logger.filename
#create a listener to print all received messages
# notifier = can.Notifier(bus,(logger,),timeout=None)

count = 0
msg = can.Message(
arbitration_id=0xBAD,
data=[0x0F,0x00,0x00,0x0D])
period = 0.05

try:
    t0 = perf_counter()
    while bus.is_connected:
        bus.recv()
        count += 1
    print("socket disconnected.")

except KeyboardInterrupt:
    print("ctrl+c, exiting...")

finally:
    t1 = perf_counter()
    if bus.is_connected:
        bus.shutdown()
    logger.write_to_file()
    dt = t1-t0
    print(count,"messages in ",dt,"seconds. Period:",1000*dt/count)
    


