import sys

from .tcpbus.server import TCPBus
if sys.platform == 'linux':
    # The TCPClient only works on Linux
    from .tcpbus.client import TCPClient

name = 'socketsocketcan'
__version__ = '0.0.4'
