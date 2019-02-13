import multiprocessing

# noinspection PyUnresolvedReferences
from tcpclient import tcpclient


class TCPClient(object):

    def __init__(self, can_port, hostname, port):
        self.can_port = can_port
        self.hostname = hostname
        self.port = port

        # Run the client in a separate process, so it does not block the main thread
        self._tcp_client_process = multiprocessing.Process(target=self._tcp_client, args=(can_port, hostname, port))
        self._tcp_client_process.daemon = False
        self._tcp_client_process.start()

    def shutdown(self, timeout=1):
        self._tcp_client_process.join(timeout)
        if self._tcp_client_process.is_alive():
            self._tcp_client_process.terminate()

    @staticmethod
    def _tcp_client(can_port, hostname, port):
        tcpclient(can_port, hostname, port)
