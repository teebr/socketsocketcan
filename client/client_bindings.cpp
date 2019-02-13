#include <pybind11/pybind11.h>

namespace py = pybind11;

int tcpclient(const char *can_port, const char *hostname, int port);

PYBIND11_MODULE(tcpclient, m) {
    m.def("tcpclient", &tcpclient);
}
