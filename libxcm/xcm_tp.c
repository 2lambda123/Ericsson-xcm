/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2020 Ericsson AB
 */

#include "xcm_tp.h"

#include "util.h"
#include "xcm_addr.h"
#include "xcm_attr_names.h"

#ifdef XCM_CTL
#include "ctl.h"
#endif

const char *xcm_tp_socket_type_name(enum xcm_socket_type socket_type)
{
    switch (socket_type) {
    case xcm_socket_type_conn:
	return "connection";
    case xcm_socket_type_server:
	return "server";
    default:
	ut_assert(0);
    }
}

/* socket id, unique on a per-process basis */
static pthread_mutex_t next_id_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t next_id = 0;

static int64_t get_next_sock_id(void)
{
    int64_t nid;
    ut_mutex_lock(&next_id_lock);
    nid = next_id++;
    ut_mutex_unlock(&next_id_lock);
    return nid;
}

struct xcm_socket *xcm_tp_socket_create(const struct xcm_tp_proto *proto,
					enum xcm_socket_type type,
					int epoll_fd, bool is_blocking)
{
    size_t priv_size = proto->ops->priv_size(type);
    struct xcm_socket *s = ut_calloc(sizeof(struct xcm_socket) + priv_size);
    s->proto = proto;
    s->type = type;
    s->is_blocking = is_blocking;
    s->epoll_fd = epoll_fd;
    s->sock_id = get_next_sock_id();
    s->condition = 0;
#ifdef XCM_CTL
    s->ctl = NULL;
#endif
    return s;
}

void xcm_tp_socket_destroy(struct xcm_socket *s)
{
    ut_free(s);
}

int xcm_tp_socket_init(struct xcm_socket *s, struct xcm_socket *parent)
{
    return XCM_TP_CALL(init, s, parent);
}

static void do_ctl(struct xcm_socket *s)
{
#ifdef XCM_CTL
    if (s->ctl != NULL)
	ctl_process(s->ctl);
#endif
}

#define MAX_SKIPPED_CTL_CALLS (256)
/* This is the maximum number of times an application will go into
   select() (or the equivalent) and back again before the control
   interface fd is being properly processed. A higher number here
   means higher CPU usage and higher latency when the control
   interface is used. A lower number means more poll() system calls to
   check the control fd, and higher CPU usage during normal operation
   (i.e., when the control interface fd is not active). */
#define MAX_WAKEUPS_PER_CTL_CHECK (4)

static void consider_ctl(struct xcm_socket *s, bool permanently_failed_op,
			 bool temporarly_failed_op)
{
#ifdef XCM_CTL
    if (permanently_failed_op)
	return;

    if (temporarly_failed_op)
	s->skipped_ctl_calls +=
	    (MAX_SKIPPED_CTL_CALLS / MAX_WAKEUPS_PER_CTL_CHECK);
    else
	s->skipped_ctl_calls++;

    if (s->skipped_ctl_calls > MAX_SKIPPED_CTL_CALLS) {
	do_ctl(s);
	s->skipped_ctl_calls = 0;
    }
#endif
}

int xcm_tp_socket_connect(struct xcm_socket *s, const char *remote_addr)
{
    do_ctl(s);

    int rc = XCM_TP_CALL(connect, s, remote_addr);
    if (rc == 0)
	xcm_tp_socket_update(s);
    return rc;
}

int xcm_tp_socket_server(struct xcm_socket *s, const char *local_addr)
{
    do_ctl(s);

    int rc = XCM_TP_CALL(server, s, local_addr);
    if (rc == 0)
	xcm_tp_socket_update(s);
    return rc;
}

int xcm_tp_socket_close(struct xcm_socket *s)
{
    int rc = 0;

    if (s != NULL) {
#ifdef XCM_CTL
	ctl_destroy(s->ctl, true);
#endif
	rc = XCM_TP_CALL(close, s);
    }
    return rc;
}

void xcm_tp_socket_cleanup(struct xcm_socket *s)
{
    if (s != NULL) {
#ifdef XCM_CTL
	ctl_destroy(s->ctl, false);
#endif
	XCM_TP_CALL(cleanup, s);
    }
}

int xcm_tp_socket_accept(struct xcm_socket *conn_s,
			 struct xcm_socket *server_s)
{
    int rc = XCM_TP_CALL(accept, conn_s, server_s);

    if (rc == 0)
	xcm_tp_socket_update(conn_s);

    consider_ctl(server_s, rc < 0 && errno != EAGAIN,
		 rc < 0 && errno == EAGAIN);

    xcm_tp_socket_update(server_s);

    return rc;
}

int xcm_tp_socket_send(struct xcm_socket *s, const void *buf, size_t len)
{
    int rc = XCM_TP_CALL(send, s, buf, len);

    consider_ctl(s, rc < 0 && errno != EAGAIN, rc < 0 && errno == EAGAIN);

    xcm_tp_socket_update(s);
    return rc;
}

int xcm_tp_socket_receive(struct xcm_socket *s, void *buf, size_t capacity)
{
    int rc = XCM_TP_CALL(receive, s, buf, capacity);

    consider_ctl(s, rc == 0 || (rc < 0 && errno != EAGAIN),
		 rc < 0 && errno == EAGAIN);

    xcm_tp_socket_update(s);

    return rc;
}

void xcm_tp_socket_update(struct xcm_socket *s)
{
    XCM_TP_CALL(update, s);
}
    
int xcm_tp_socket_finish(struct xcm_socket *s)
{
    int rc = XCM_TP_CALL(finish, s);

    consider_ctl(s, rc < 0 && errno != EAGAIN, rc < 0 && errno == EAGAIN);

    xcm_tp_socket_update(s);

    return rc;
}

const char *xcm_tp_socket_get_transport(struct xcm_socket *s)
{
    /* Allow transports to give an arbitrary transport name at
       run-time, which is needed to allow UTLS connections to
       'masquerade' as the underlying transport used. */
    if (XCM_TP_GETOPS(s)->get_transport)
	return XCM_TP_CALL(get_transport, s);
    else
	return s->proto->name;
}

bool xcm_tp_socket_is_bytestream(struct xcm_socket *s)
{
    return !XCM_TP_SUPPORTS_OP(s, max_msg);
}

const char *xcm_tp_socket_get_remote_addr(struct xcm_socket *conn_s,
					  bool suppress_tracing)
{
    return XCM_TP_CALL(get_remote_addr, conn_s, suppress_tracing);
}

int xcm_tp_socket_set_local_addr(struct xcm_socket *s, const char *local_addr)
{
    if (XCM_TP_SUPPORTS_OP(s, set_local_addr))
	return XCM_TP_CALL(set_local_addr, s, local_addr);
    else {
	errno = EACCES;
	return -1;
    }
}

const char *xcm_tp_socket_get_local_addr(struct xcm_socket *s,
					 bool suppress_tracing)
{
    return XCM_TP_CALL(get_local_addr, s, suppress_tracing);
}

size_t xcm_tp_socket_max_msg(struct xcm_socket *conn_s)
{
    return XCM_TP_CALL(max_msg, conn_s);
}

int64_t xcm_tp_socket_get_cnt(struct xcm_socket *conn_s, enum xcm_tp_cnt cnt)
{
    return XCM_TP_CALL(get_cnt, conn_s, cnt);
}

void xcm_tp_socket_enable_ctl(struct xcm_socket *s)
{
#ifdef XCM_CTL
    if (XCM_TP_SUPPORTS_OP(s, enable_ctl))
	XCM_TP_CALL(enable_ctl, s);
    else
	s->ctl = ctl_create(s);
#endif
}

void xcm_tp_socket_get_attrs(struct xcm_socket *s,
			     const struct xcm_tp_attr **attr_list,
			     size_t *attr_list_len)
{
    XCM_TP_CALL(get_attrs, s, attr_list, attr_list_len);
}

int xcm_tp_get_str_attr(const char *value, void *buf, size_t capacity)
{
    size_t len = strlen(value);
    if (len >= capacity) {
	errno = EOVERFLOW;
	return -1;
    }

    strcpy(buf, value);

    return len+1;
}

int xcm_tp_set_bool_attr(const void *buf, size_t len, bool *value)
{
    if (len != sizeof(bool)) {
	errno = EINVAL;
	return -1;
    }

    memcpy(value, buf, len);

    return 0;
}

int xcm_tp_get_bool_attr(bool value, void *buf, size_t capacity)
{
    if (sizeof(bool) > capacity) {
	errno = EOVERFLOW;
	return -1;
    }

    memcpy(buf, &value, sizeof(bool));

    return sizeof(bool);
}

static int get_type_attr(struct xcm_socket *s, void *context, 
			 void *value, size_t capacity)
{
    return xcm_tp_get_str_attr(xcm_tp_socket_type_name(s->type),
			       value, capacity);
}

static int get_transport_attr(struct xcm_socket *s, void *context,
			      void *value, size_t capacity)
{
    return xcm_tp_get_str_attr(xcm_tp_socket_get_transport(s),
			       value, capacity);
}

static int set_service_attr(struct xcm_socket *s, void *context,
			    const void *value, size_t len)
{
    if (strcmp(value, XCM_SERVICE_ANY) == 0)
	return 0;

    const char *actual = xcm_tp_socket_is_bytestream(s) ?
	XCM_SERVICE_BYTESTREAM : XCM_SERVICE_MESSAGING;

    if (strcmp(actual, value) == 0)
	return 0;

    errno = EINVAL;
    return -1;
}

static int get_service_attr(struct xcm_socket *s, void *context,
			    void *value, size_t capacity)
{
    const char *service = xcm_tp_socket_is_bytestream(s) ?
	XCM_SERVICE_BYTESTREAM : XCM_SERVICE_MESSAGING;

    return xcm_tp_get_str_attr(service, value, capacity);
}

static int addr_to_attr(const char *addr, void *value, size_t capacity)
{
    if (!addr)
	return -1;
    return xcm_tp_get_str_attr(addr, value, capacity);
}

static int set_local_attr(struct xcm_socket *s, void *context,
			  const void *value, size_t len)
{
    return xcm_tp_socket_set_local_addr(s, value);
}

static int get_local_attr(struct xcm_socket *s, void *context,
			  void *value, size_t capacity)
{
    return addr_to_attr(xcm_local_addr(s), value, capacity);
}

static int get_remote_attr(struct xcm_socket *s, void *context,
			   void *value, size_t capacity)
{
    return addr_to_attr(xcm_remote_addr(s), value, capacity);
}

static int set_blocking_attr(struct xcm_socket *s, void *context,
			     const void *value, size_t len)
{
    bool is_blocking;

    if (xcm_tp_set_bool_attr(value, len, &is_blocking) < 0)
	return -1;

    if (xcm_set_blocking(s, is_blocking) < 0)
	return -1;

    return 0;
}

static int get_blocking_attr(struct xcm_socket *s, void *context,
			     void *value, size_t capacity)
{
    return xcm_tp_get_bool_attr(s->is_blocking, value, capacity);
}

static int get_max_msg_attr(struct xcm_socket *s, void *context,
			    void *value, size_t capacity)
{
    if (s->type != xcm_socket_type_conn) {
	errno = ENOENT;
	return -1;
    }

    if (capacity < sizeof(int64_t)) {
	errno = EOVERFLOW;
	return -1;
    }

    int64_t max_msg = XCM_TP_CALL(max_msg, s);

    memcpy(value, &max_msg, sizeof(int64_t));

    return sizeof(int64_t);                     \
}

#define GEN_CNT_ATTR_GETTER(cnt_name)					\
    static int get_ ## cnt_name ## _attr(struct xcm_socket *s,		\
					 void *context,			\
					 void *value,			\
					 size_t capacity)		\
    {									\
	if (capacity < sizeof(int64_t)) {				\
	    errno = EOVERFLOW;						\
	    return -1;							\
	}								\
	int64_t cnt_value =						\
	    xcm_tp_socket_get_cnt(s, xcm_tp_cnt_ ## cnt_name);		\
	memcpy(value, &cnt_value, sizeof(int64_t));			\
	return sizeof(int64_t);						\
    }

GEN_CNT_ATTR_GETTER(to_app_bytes)
GEN_CNT_ATTR_GETTER(from_app_bytes)
GEN_CNT_ATTR_GETTER(to_lower_bytes)
GEN_CNT_ATTR_GETTER(from_lower_bytes)

GEN_CNT_ATTR_GETTER(to_app_msgs)
GEN_CNT_ATTR_GETTER(from_app_msgs)
GEN_CNT_ATTR_GETTER(to_lower_msgs)
GEN_CNT_ATTR_GETTER(from_lower_msgs)

#define COMMON_ATTRS							\
    XCM_TP_DECL_RW_ATTR(XCM_ATTR_XCM_BLOCKING, xcm_attr_type_bool,	\
			set_blocking_attr, get_blocking_attr),		\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_TYPE, xcm_attr_type_str,	        \
			get_type_attr),					\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_TRANSPORT, xcm_attr_type_str,	\
			get_transport_attr),				\
    XCM_TP_DECL_RW_ATTR(XCM_ATTR_XCM_SERVICE, xcm_attr_type_str,	\
			set_service_attr, get_service_attr)

#define MSG_CNT_ATTRS						\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_TO_APP_MSGS, xcm_attr_type_int64,  \
			get_to_app_msgs_attr),				\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_TO_APP_BYTES, xcm_attr_type_int64, \
			get_to_app_bytes_attr),				\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_FROM_APP_MSGS, xcm_attr_type_int64, \
	                get_from_app_msgs_attr),			\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_FROM_APP_BYTES, xcm_attr_type_int64, \
			get_from_app_bytes_attr),			\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_TO_LOWER_MSGS, xcm_attr_type_int64, \
			get_to_lower_msgs_attr),			\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_TO_LOWER_BYTES, xcm_attr_type_int64, \
			get_to_lower_bytes_attr),			\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_FROM_LOWER_MSGS, xcm_attr_type_int64, \
			get_from_lower_msgs_attr),			\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_FROM_LOWER_BYTES, xcm_attr_type_int64, \
			get_from_lower_bytes_attr)

#define BYTESTREAM_CNT_ATTRS						\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_TO_APP_BYTES, xcm_attr_type_int64, \
			get_to_app_bytes_attr),				\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_FROM_APP_BYTES, xcm_attr_type_int64, \
			get_from_app_bytes_attr),			\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_TO_LOWER_BYTES, xcm_attr_type_int64, \
			get_to_lower_bytes_attr),			\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_FROM_LOWER_BYTES, xcm_attr_type_int64, \
			get_from_lower_bytes_attr)

#define COMMON_CONN_ATTRS						\
    COMMON_ATTRS,							\
    XCM_TP_DECL_RW_ATTR(XCM_ATTR_XCM_LOCAL_ADDR, xcm_attr_type_str,     \
			set_local_attr, get_local_attr),		\
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_REMOTE_ADDR, xcm_attr_type_str,    \
			get_remote_attr)

static struct xcm_tp_attr msg_conn_attrs[] = {
    COMMON_CONN_ATTRS,
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_MAX_MSG_SIZE, xcm_attr_type_int64,
			get_max_msg_attr),
    MSG_CNT_ATTRS
};

static struct xcm_tp_attr bytestream_conn_attrs[] = {
    COMMON_CONN_ATTRS,
    BYTESTREAM_CNT_ATTRS
};

static struct xcm_tp_attr server_attrs[] = {
    COMMON_ATTRS,
    XCM_TP_DECL_RO_ATTR(XCM_ATTR_XCM_LOCAL_ADDR, xcm_attr_type_str,
			get_local_attr)
};

void xcm_tp_get_attrs(enum xcm_socket_type type, bool bytestream,
		      const struct xcm_tp_attr **attr_list,
		      size_t *attr_list_len)
{
    switch (type) {
    case xcm_socket_type_conn:
	if (bytestream) {
	    *attr_list = bytestream_conn_attrs;
	    *attr_list_len = UT_ARRAY_LEN(bytestream_conn_attrs);
	} else {
	    *attr_list = msg_conn_attrs;
	    *attr_list_len = UT_ARRAY_LEN(msg_conn_attrs);
	}
	break;
    case xcm_socket_type_server:
	*attr_list = server_attrs;
	*attr_list_len = UT_ARRAY_LEN(server_attrs);
	break;
    default:
	ut_assert(0);
    }
}

#define MAX_PROTOS (8)
static struct xcm_tp_proto protos[MAX_PROTOS];
static size_t num_protos = 0;

struct xcm_tp_proto *xcm_tp_proto_by_name(const char *proto_name)
{
    int i;
    for (i = 0; i < num_protos; i++)
	if (strcmp(protos[i].name, proto_name) == 0)
	    return &(protos[i]);
    return NULL;
}

struct xcm_tp_proto *xcm_tp_proto_by_addr(const char *addr)
{
    char proto_s[XCM_ADDR_MAX_PROTO_LEN+1];
    if (xcm_addr_parse_proto(addr, proto_s, sizeof(proto_s)) < 0)
	return NULL;

    struct xcm_tp_proto *proto = xcm_tp_proto_by_name(proto_s);
    if (!proto) {
	errno = ENOPROTOOPT;
	return NULL;
    }
    return proto;
}

void xcm_tp_register(const char *proto_name, const struct xcm_tp_ops *ops)
{
    /* build configuration needs to assure we don't exceed these limits */
    ut_assert(num_protos < MAX_PROTOS);
    ut_assert(strlen(proto_name) <= XCM_ADDR_MAX_PROTO_LEN);
    ut_assert(strlen(proto_name) <= XCM_ADDR_MAX_PROTO_LEN);
    ut_assert(xcm_tp_proto_by_name(proto_name) == NULL);

    strcpy(protos[num_protos].name, proto_name);
    protos[num_protos].ops = ops;
    num_protos++;
}
