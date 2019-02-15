#include <iostream>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <linux/can.h>

namespace py = pybind11;

int tcpclient(const char *can_port, const char *hostname, int port, const struct can_filter *filter, int numfilter, bool use_unordered_map);

int tcpclient_wrapper(const char *can_port, const char *hostname, int port, py::list filters, bool use_unordered_map) {
    int numfilter = filters.size();
    int retval = -1;
    if (numfilter == 0)
    {
        retval = tcpclient(can_port, hostname, port, NULL, 0, use_unordered_map);
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
        retval = tcpclient(can_port, hostname, port, rfilter, numfilter, use_unordered_map);
        free(rfilter);
    }
    return retval;
}

PYBIND11_MODULE(tcpclient, m) {
    m.def("tcpclient", &tcpclient_wrapper);
}
