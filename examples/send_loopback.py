from socketsocketcan import TCPBus
import can
from datetime import datetime   
from time import sleep

bus = TCPBus(5000)
print("socket connected!")

#create a listener to print all received messages
listener = can.Printer()
notifier = can.Notifier(bus,(listener,),timeout=None)

try:
    msg = can.Message(
    is_extended_id=False,
    dlc=6)
    count = 0
    while bus.is_connected:
        msg.arbitration_id = count
        msg.data = (count).to_bytes(6,"little")
        bus.send(msg)
        msg.timestamp = datetime.now().timestamp() #just needed for printing
        print(msg) #print sent message
        count+=1
        sleep(0.5)
    print("socket disconnected.")

except KeyboardInterrupt:
    print("ctrl+c, exiting...")
    notifier.stop()
    if bus.is_connected:
        bus.shutdown()
