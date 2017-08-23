#ifndef IB_PINGPONG_H
#define IB_PINGPONG_H

struct pingpong_context {
        struct ibv_context      *context;
        struct ibv_comp_channel *channel;
        struct ibv_pd           *pd;
        struct ibv_mr           *mr;
        union {
                struct ibv_cq           *cq;
                struct ibv_cq_ex        *cq_ex;
        } cq_s;
        struct ibv_qp           *qp;
        void                    *buf;
        int                      size;
        int                      send_flags;
        int                      rx_depth;
        int                      pending;
        struct ibv_port_attr     portinfo;
        uint64_t                 completion_timestamp_mask;
};


#endif /* IB_PINGPONG_H */
