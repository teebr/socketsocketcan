import socket
from queue import Empty as QueueEmpty
from queue import Queue
from threading import Thread, Event
from typing import Dict, Union, List

import can
from can.interfaces.socketcan.constants import CAN_ERR_FLAG, CAN_RTR_FLAG, CAN_EFF_FLAG


class TCPBus(can.BusABC):
    RECV_FRAME_SZ = 21

    def __init__(self, port: int, hostname='', can_filters: List[Dict[str, Union[int, bool]]] = None, **kwargs):
        self.channel_info = "tcpbus port: {} hostname: {}".format(port, hostname)
        super(TCPBus, self).__init__('tcpbus', can_filters=can_filters, **kwargs)

        self.hostname = hostname #TODO: check validity. ping?
        self.port = port #TODO: check validity.
        self._recv_buffer = Queue()
        self._send_buffer = Queue()
        self._shutdown_flag = Event()
        self._is_connected = Event()

        #open socket and wait for connection to establish.
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._socket.bind((self.hostname, self.port))
        self._socket.listen(1)
        self._conn, addr = self._socket.accept()
        self._is_connected.set()
        self._conn.settimeout(0.5) #blocking makes exiting an infinite loop hard

        #now we're connected, kick off other threads.
        self._tcp_listener = Thread(target=self._poll_socket, name="tcpbus_poll_socket",
                                    args=(self._conn, self._shutdown_flag, self._is_connected, self._recv_buffer))
        self._tcp_listener.daemon = False  # Make sure the application waits for this to finish
        self._tcp_listener.start()

        self._tcp_writer = Thread(target=self._poll_send, name="tcpbus_poll_send",
                                  args=(self._conn, self._shutdown_flag, self._is_connected, self._send_buffer))
        self._tcp_writer.daemon = False  # Make sure the application waits for this to finish
        self._tcp_writer.start()

    def _recv_internal(self, timeout=None):
        if self._shutdown_flag.is_set():
            return None, False

        # All filtering is done in the client
        try:
            return self._recv_buffer.get(timeout=timeout), True
        except QueueEmpty:
            return None, False

    def clear_recv_buffer(self):
        with self._recv_buffer.mutex:
            self._recv_buffer.queue.clear()

    def send(self, msg, timeout=None):
        if msg.is_extended_id:
            msg.arbitration_id |= CAN_EFF_FLAG
        if msg.is_remote_frame:
            msg.arbitration_id |= CAN_RTR_FLAG
        if msg.is_error_frame:
            msg.arbitration_id |= CAN_ERR_FLAG
        self._send_buffer.put(msg, timeout=timeout)

    def _stop_threads(self, timeout):
        self._shutdown_flag.set()

        # Make sure the socket is always closed
        # See: https://stackoverflow.com/questions/409783/socket-shutdown-vs-socket-close
        try:
            self._socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._socket.close()

        # Wait for the two threads to exit
        self._tcp_listener.join(timeout)
        self._tcp_writer.join(timeout)

        self._is_connected.clear()

    def shutdown(self, timeout=None):
        """gracefully close TCP connection and exit threads"""
        self._stop_threads(timeout)

    @property
    def is_connected(self):
        """check that a TCP connection is active"""
        return self._is_connected.is_set()

    @staticmethod
    def _msg_to_bytes(msg):
        """convert Message object to bytes to be put on TCP socket"""
        arb_id = msg.arbitration_id.to_bytes(4,"little") #TODO: masks
        dlc = msg.dlc.to_bytes(1,"little")
        data = msg.data + bytes(8-msg.dlc)
        return arb_id+dlc+data

    @staticmethod
    def _bytes_to_message(b):
        """convert raw TCP bytes to can.Message object"""
        ts = int.from_bytes(b[:4],"little") + int.from_bytes(b[4:8],"little")/1e6
        can_id = int.from_bytes(b[8:12],"little")
        dlc = b[12] #TODO: sanity check on these values in case of corrupted messages.

        # Decompose ID
        is_extended = bool(can_id & CAN_EFF_FLAG)
        if is_extended:
            arb_id = can_id & 0x1FFFFFFF
        else:
            arb_id = can_id & 0x000007FF

        return can.Message(
            timestamp = ts,
            arbitration_id = arb_id,
            is_extended_id = is_extended,
            is_error_frame = bool(can_id & CAN_ERR_FLAG),
            is_remote_frame = bool(can_id & CAN_RTR_FLAG),
            dlc=dlc,
            data=b[13:13+dlc]
        )

    @staticmethod
    def _poll_socket(connection, shutdown_flag, connected_flag, recv_buffer):
        """background thread to check for new CAN messages on the TCP socket"""
        part_formed_message = bytearray() # TCP transfer might off part way through sending a message
        while not shutdown_flag.is_set() and connected_flag.is_set():
            try:
                data = connection.recv(TCPBus.RECV_FRAME_SZ * 20)
            except socket.timeout:
                # no data, just try again.
                continue
            except OSError:
                # socket's been closed.
                connection.close()
                break

            if len(data):
                # process the 1 or more messages we just received

                if len(part_formed_message):
                    data = part_formed_message + data #add on the previous remainder

                #check how many whole and incomplete messages we got through.
                num_incomplete_bytes = len(data) % TCPBus.RECV_FRAME_SZ
                num_frames = len(data) // TCPBus.RECV_FRAME_SZ

                #to pre-pend next time:
                if num_incomplete_bytes:
                    part_formed_message = data[-num_incomplete_bytes:]
                else:
                    part_formed_message = bytearray()

                c = 0
                for _ in range(num_frames):
                    recv_buffer.put(TCPBus._bytes_to_message(data[c:c+TCPBus.RECV_FRAME_SZ]))
                    c += TCPBus.RECV_FRAME_SZ
            else:
                # socket's been closed at the other end.
                connection.close()
                break

        # Set the flags to signal that the connection has been closed
        shutdown_flag.set()
        connected_flag.clear()

    @staticmethod
    def _poll_send(connection, shutdown_flag, connected_flag, send_buffer):
        """background thread to send messages when they are put in the queue"""
        while not shutdown_flag.is_set() and connected_flag.is_set():
            try:
                msg = send_buffer.get(timeout=0.02)
                data = TCPBus._msg_to_bytes(msg)
                while not send_buffer.empty(): #we know there's one message, might be more.
                    data += TCPBus._msg_to_bytes(send_buffer.get())
                try:
                    connection.sendall(data)
                except OSError:
                    connection.close()
                    break
            except QueueEmpty:
                pass  # NBD, just means nothing to send.

        # Set the flags to signal that the connection has been closed
        shutdown_flag.set()
        connected_flag.clear()
