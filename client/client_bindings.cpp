#include <pybind11/pybind11.h>

#include "client.h"

namespace py = pybind11;

class TCPClient {
public:
    TCPClient(const char *can_port, const char *hostname, int port) {
        run(can_port, hostname, port);
    }
};

PYBIND11_MODULE(tcpclient, m) {
    py::class_<TCPClient>(m, "tcpclient")
        .def(py::init<const char*, const char*, int>());
}
