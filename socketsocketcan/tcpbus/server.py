import socket
from queue import Queue
from threading import Thread
import can
from time import sleep

class TCPBus(can.BusABC):
    FRAME_SZ = 21
    CAN_EFF_FLAG = 0x80000000
    CAN_RTR_FLAG = 0x40000000
    CAN_ERR_FLAG = 0x20000000
    
    def __init__(self,port,hostname="",can_filters=None,**kwargs):
        super().__init__("whatever",can_filters)
        self.hostname = hostname #TODO: check validity. ping?
        self.port = port #TODO: check validity.
        self._is_connected = False
        self.recv_buffer = Queue()
        self.send_buffer = Queue()
        self._shutdown_flag = Queue()

        #open socket and wait for connection to establish.
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._socket.bind((self.hostname, self.port))
        self._socket.listen()
        self._conn, addr = self._socket.accept()
        self._is_connected = True
        self._conn.settimeout(0.5) #blocking makes exiting an infinite loop hard

        #now we're connected, kick off other threads.
        self._tcp_listener = Thread(target=self._poll_socket)
        self._tcp_listener.start()

        self._tcp_writer = Thread(target=self._poll_send)
        self._tcp_writer.start()
    
    def _recv_internal(self,timeout=None):
        #TODO: filtering shit
        return (self.recv_buffer.get(timeout=timeout), True)

    def send(self,msg):
        if msg.is_extended_id:
            msg.arbitration_id |= self.CAN_EFF_FLAG
        if msg.is_remote_frame:
            msg.arbitration_id |= self.CAN_RTR_FLAG
        if msg.is_error_frame:
            msg.arbitration_id |= self.CAN_ERR_FLAG        
        self.send_buffer.put(msg)

    def _stop_threads(self):
        self._shutdown_flag.put(True)
        self._socket.close() #shutdown might be faster but can be ugly and raise an exception
        self._is_connected = False

    def shutdown(self):
        """gracefully close TCP connection and exit threads"""
        if self.is_connected:
            self._stop_threads()
        #can't join threads because this might be called from that thread, so just wait...
        while self._tcp_listener.is_alive() or self._tcp_writer.is_alive():
            sleep(0.005)

    @property
    def is_connected(self):
        """check that a TCP connection is active"""
        return self._is_connected

    def _msg_to_bytes(self,msg):
        """convert Message object to bytes to be put on TCP socket"""
        id = msg.arbitration_id.to_bytes(4,"little") #TODO: masks
        dlc = msg.dlc.to_bytes(1,"little")
        data = msg.data + bytes(8-msg.dlc)
        return id+dlc+data

    def _bytes_to_message(self,b):
        """convert raw TCP bytes to can.Message object"""
        ts = int.from_bytes(b[:4],"little") + int.from_bytes(b[4:8],"little")/1e6
        dlc = b[12]
        can_id = int.from_bytes(b[8:12],"little")

        #decompose ID
        is_extended = bool(can_id & self.CAN_EFF_FLAG) #CAN_EFF_FLAG
        if is_extended:
            arb_id = can_id & 0x1FFFFFFF
        else:
            arb_id = can_id & 0x000007FF
        
        return can.Message(
            timestamp = ts,
            arbitration_id = arb_id,
            is_extended_id = is_extended,
            is_error_frame = bool(can_id & self.CAN_ERR_FLAG), #CAN_ERR_FLAG
            is_remote_frame = bool(can_id & self.CAN_RTR_FLAG), #CAN_RTR_FLAG
            dlc=dlc,
            data=b[13:13+dlc]
        )

    def _poll_socket(self):
        """background thread to check for new CAN messages on the TCP socket"""
        with self._conn as conn:
            while self._shutdown_flag.empty():
                try:
                    data = conn.recv(8192)
                except socket.timeout:
                    #no data, just try again.
                    continue
                except OSError:
                    # socket's been closed.
                    self._stop_threads()
                    break                
                # process the 1 or more messages we just received
                num_frames = len(data) // self.FRAME_SZ
                if len(data):
                    c = 0
                    for _ in range(num_frames):
                        self.recv_buffer.put(self._bytes_to_message(data[c:c+self.FRAME_SZ]))
                        c += self.FRAME_SZ
                else:
                    #socket's been closed at the other end.
                    self._stop_threads()
                    break

    def _poll_send(self):
        """background thread to send messages when they are put in the queue"""
        with self._conn as s:
            while self._shutdown_flag.empty():
                if self.send_buffer.empty():
                    #TODO: use timeout on queue to avoid this? easy for one message, but could be multiple messages
                    sleep(0.001) 
                else:
                    data = bytearray()
                    while not self.send_buffer.empty():
                        msg = self.send_buffer.get()
                        data += self._msg_to_bytes(msg)
                    try:
                        s.sendall(data)
                    except OSError:
                        # socket's been closed.
                        self._stop_threads()
                        break

        