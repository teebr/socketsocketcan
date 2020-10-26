#include <iostream>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <linux/can.h>

namespace py = pybind11;

int tcpclient(const char *can_port, const char *hostname, int port, const struct can_filter *filter, int numfilter, bool use_unordered_map, float limit_recv_rate_hz);

int tcpclient_wrapper(const char *can_port, const char *hostname, int port, py::list filters, bool use_unordered_map, py::object limit_recv_rate_hz_obj) {
    int numfilter = filters.size();
    float limit_recv_rate_hz = limit_recv_rate_hz_obj.is_none() ? -1 : limit_recv_rate_hz_obj.cast<float>();
    //py::print(limit_recv_rate_hz);

    int retval = -1;
    if (numfilter == 0)
    {
        retval = tcpclient(can_port, hostname, port, NULL, 0, use_unordered_map, limit_recv_rate_hz);
    }
    else
    {
        struct can_filter *rfilter = (struct can_filter*)malloc(sizeof(struct can_filter) * numfilter);
        int i = 0;
        for (auto filter : filters) {
            //py::print(filter);
            rfilter[i].can_id = filter["can_id"].cast<canid_t>();
            rfilter[i].can_mask = filter["can_mask"].cast<canid_t>();
            i++;
        }
        retval = tcpclient(can_port, hostname, port, rfilter, numfilter, use_unordered_map, limit_recv_rate_hz);
        free(rfilter);
    }
    return retval;
}

PYBIND11_MODULE(tcpclient, m) {
    m.def("tcpclient", &tcpclient_wrapper);
}
