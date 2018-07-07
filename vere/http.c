/* v/http.c
**
*/
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <uv.h>
#include <errno.h>
#include <openssl/ssl.h>
 #include <openssl/err.h>
#include <h2o.h>
#include "all.h"
#include "vere/vere.h"

#include <picohttpparser.h>
#include <tls.h>

typedef struct _u3_h2o_serv {
  h2o_globalconf_t fig_u;             //  h2o global config
  h2o_context_t    ctx_u;             //  h2o ctx
  h2o_accept_ctx_t cep_u;             //  h2o accept ctx
  h2o_hostconf_t*  hos_u;             //  h2o host config
  h2o_handler_t*   han_u;             //  h2o request handler
} u3_h2o_serv;

static void _proxy_serv_free(u3_prox* lis_u);
static void _proxy_serv_close(u3_prox* lis_u);
static u3_prox* _proxy_serv_new(u3_http* htp_u, c3_s por_s, c3_o sec);
static u3_prox* _proxy_serv_start(u3_prox* lis_u);

static void _http_conn_close(u3_hcon* hon_u);
static void _http_serv_close_hard(u3_http* htp_u);
static void _http_serv_start_all(void);

static const c3_i TCP_BACKLOG = 16;

/* _http_vec_to_meth(): convert h2o_iovec_t to meth
*/
static u3_weak
_http_vec_to_meth(h2o_iovec_t vec_u)
{
  return ( 0 == strncmp(vec_u.base, "GET",     vec_u.len) ) ? c3__get  :
         ( 0 == strncmp(vec_u.base, "PUT",     vec_u.len) ) ? c3__put  :
         ( 0 == strncmp(vec_u.base, "POST",    vec_u.len) ) ? c3__post :
         ( 0 == strncmp(vec_u.base, "HEAD",    vec_u.len) ) ? c3__head :
         ( 0 == strncmp(vec_u.base, "CONNECT", vec_u.len) ) ? c3__conn :
         ( 0 == strncmp(vec_u.base, "DELETE",  vec_u.len) ) ? c3__delt :
         ( 0 == strncmp(vec_u.base, "OPTIONS", vec_u.len) ) ? c3__opts :
         ( 0 == strncmp(vec_u.base, "TRACE",   vec_u.len) ) ? c3__trac :
         // TODO ??
         // ( 0 == strncmp(vec_u.base, "PATCH",   vec_u.len) ) ? c3__patc :
         u3_none;
}

/* _http_vec_to_atom(): convert h2o_iovec_t to atom (cord)
*/
static u3_noun
_http_vec_to_atom(h2o_iovec_t vec_u)
{
  return u3i_bytes(vec_u.len, (const c3_y*)vec_u.base);
}

/* _http_vec_to_octs(): convert h2o_iovec_t to (unit octs)
*/
static u3_noun
_http_vec_to_octs(h2o_iovec_t vec_u)
{
  if ( 0 == vec_u.len ) {
    return u3_nul;
  }

  // XX correct size_t -> atom?
  return u3nt(u3_nul, u3i_chubs(1, (const c3_d*)&vec_u.len),
                      _http_vec_to_atom(vec_u));
}

/* _http_vec_from_octs(): convert (unit octs) to h2o_iovec_t
*/
static h2o_iovec_t
_http_vec_from_octs(u3_noun oct)
{
  if ( u3_nul == oct ) {
    return h2o_iovec_init(0, 0);
  }

  //  2GB max
  if ( c3n == u3a_is_cat(u3h(u3t(oct))) ) {
    u3m_bail(c3__fail);
  }

  c3_w len_w  = u3h(u3t(oct));
  c3_y* buf_y = c3_malloc(1 + len_w);
  buf_y[len_w] = 0;

  u3r_bytes(0, len_w, buf_y, u3t(u3t(oct)));

  u3z(oct);
  return h2o_iovec_init(buf_y, len_w);
}

/* _http_heds_to_noun(): convert h2o_header_t to (list (pair @t @t))
*/
static u3_noun
_http_heds_to_noun(h2o_header_t* hed_u, c3_d hed_d)
{
  u3_noun hed = u3_nul;
  c3_d dex_d  = hed_d;

  h2o_header_t deh_u;

  while ( 0 < dex_d ) {
    deh_u = hed_u[--dex_d];
    hed = u3nc(u3nc(_http_vec_to_atom(*deh_u.name),
                    _http_vec_to_atom(deh_u.value)), hed);
  }

  return hed;
}

/* _http_heds_free(): free header linked list
*/
static void
_http_heds_free(u3_hhed* hed_u)
{
  while ( hed_u ) {
    u3_hhed* nex_u = hed_u->nex_u;

    free(hed_u->nam_c);
    free(hed_u->val_c);
    free(hed_u);
    hed_u = nex_u;
  }
}

/* _http_hed_new(): create u3_hhed from nam/val cords
*/
static u3_hhed*
_http_hed_new(u3_atom nam, u3_atom val)
{
  c3_w     nam_w = u3r_met(3, nam);
  c3_w     val_w = u3r_met(3, val);
  u3_hhed* hed_u = c3_malloc(sizeof(*hed_u));

  hed_u->nam_c = c3_malloc(1 + nam_w);
  hed_u->val_c = c3_malloc(1 + val_w);
  hed_u->nam_c[nam_w] = 0;
  hed_u->val_c[val_w] = 0;
  hed_u->nex_u = 0;
  hed_u->nam_w = nam_w;
  hed_u->val_w = val_w;

  u3r_bytes(0, nam_w, (c3_y*)hed_u->nam_c, nam);
  u3r_bytes(0, val_w, (c3_y*)hed_u->val_c, val);

  return hed_u;
}

/* _http_heds_from_noun(): convert (list (pair @t @t)) to u3_hhed
*/
static u3_hhed*
_http_heds_from_noun(u3_noun hed)
{
  u3_noun deh = hed;
  u3_noun i_hed;

  u3_hhed* hed_u = 0;

  while ( u3_nul != hed ) {
    i_hed = u3h(hed);
    u3_hhed* nex_u = _http_hed_new(u3h(i_hed), u3t(i_hed));
    nex_u->nex_u = hed_u;

    hed_u = nex_u;
    hed = u3t(hed);
  }

  u3z(deh);
  return hed_u;
}

/* _http_req_find(): find http request in connection by sequence.
*/
static u3_hreq*
_http_req_find(u3_hcon* hon_u, c3_w seq_l)
{
  u3_hreq* req_u = hon_u->req_u;

  //  XX glories of linear search
  //
  while ( req_u ) {
    if ( seq_l == req_u->seq_l ) {
      return req_u;
    }
    req_u = req_u->nex_u;
  }
  return 0;
}

/* _http_req_link(): link http request to connection
*/
static void
_http_req_link(u3_hcon* hon_u, u3_hreq* req_u)
{
  req_u->hon_u = hon_u;
  req_u->seq_l = hon_u->seq_l++;
  req_u->nex_u = hon_u->req_u;

  if ( 0 != req_u->nex_u ) {
    req_u->nex_u->pre_u = req_u;
  }
  hon_u->req_u = req_u;
}

/* _http_req_unlink(): remove http request from connection
*/
static void
_http_req_unlink(u3_hreq* req_u)
{
  if ( 0 != req_u->pre_u ) {
    req_u->pre_u->nex_u = req_u->nex_u;

    if ( 0 != req_u->nex_u ) {
      req_u->nex_u->pre_u = req_u->pre_u;
    }
  }
  else {
    req_u->hon_u->req_u = req_u->nex_u;

    if ( 0 != req_u->nex_u ) {
      req_u->nex_u->pre_u = 0;
    }
  }
}

/* _http_req_to_duct(): translate srv/con/req to duct
*/
static u3_noun
_http_req_to_duct(u3_hreq* req_u)
{
  return u3nt(u3_blip, c3__http,
              u3nq(u3dc("scot", c3_s2('u','v'), req_u->hon_u->htp_u->sev_l),
                   u3dc("scot", c3_s2('u','d'), req_u->hon_u->coq_l),
                   u3dc("scot", c3_s2('u','d'), req_u->seq_l),
                   u3_nul));
}

/* _http_req_kill(): kill http request in %eyre.
*/
static void
_http_req_kill(u3_hreq* req_u)
{
  u3_noun pox = _http_req_to_duct(req_u);
  u3v_plan(pox, u3nc(c3__thud, u3_nul));
}

/* _http_req_cancel(): send 503 and kill request
*/
static void
_http_req_cancel(u3_hreq* req_u)
{
  if ( u3_rsat_plan != req_u->sat_e ) {
    return;
  }

  c3_c* msg_c = "shutting down";
  h2o_send_error_generic(req_u->rec_u, 503, msg_c, msg_c,
                         H2O_SEND_ERROR_HTTP1_CLOSE_CONNECTION);
  _http_req_kill(req_u);
  req_u->sat_e = u3_rsat_ripe;
}

/* _http_req_done(): request finished, deallocation callback
*/
static void
_http_req_done(void* ptr_v)
{
  u3_hreq* req_u = (u3_hreq*)ptr_v;

  // client canceled request
  if ( u3_rsat_plan == req_u->sat_e ) {
    _http_req_kill(req_u);
  }

  _http_req_unlink(req_u);
}

/* _http_req_new(): receive http request.
*/
static u3_hreq*
_http_req_new(u3_hcon* hon_u, h2o_req_t* rec_u)
{
  u3_hreq* req_u = h2o_mem_alloc_shared(&rec_u->pool, sizeof(*req_u),
                                        _http_req_done);
  req_u->rec_u = rec_u;
  req_u->sat_e = u3_rsat_init;
  req_u->pre_u = 0;

  _http_req_link(hon_u, req_u);

  return req_u;
}

/* _http_req_dispatch(): dispatch http request to %eyre
*/
static void
_http_req_dispatch(u3_hreq* req_u, u3_noun req)
{
  c3_assert(u3_rsat_init == req_u->sat_e);
  req_u->sat_e = u3_rsat_plan;

  u3_noun pox = _http_req_to_duct(req_u);
  u3_noun typ = _(req_u->hon_u->htp_u->lop) ? c3__chis : c3__this;

  u3v_plan(pox, u3nq(typ,
                     req_u->hon_u->htp_u->sec,
                     u3nc(c3y, u3i_words(1, &req_u->hon_u->ipf_w)),
                     req));
}

typedef struct _u3_hgen {
  h2o_generator_t neg_u;
  h2o_iovec_t     bod_u;
  u3_hhed*        hed_u;
} u3_hgen;

/* _http_hgen_dispose(): dispose response generator and buffers
*/
static void
_http_hgen_dispose(void* ptr_v)
{
  u3_hgen* gen_u = (u3_hgen*)ptr_v;
  _http_heds_free(gen_u->hed_u);
  free(gen_u->bod_u.base);
}

/* _http_req_respond(): write httr to h2o_req_t->res and send
*/
static void
_http_req_respond(u3_hreq* req_u, u3_noun sas, u3_noun hed, u3_noun bod)
{
  // XX ideally
  //c3_assert(u3_rsat_plan == req_u->sat_e);

  if ( u3_rsat_plan != req_u->sat_e ) {
    //uL(fprintf(uH, "duplicate response\n"));
    return;
  }

  req_u->sat_e = u3_rsat_ripe;

  h2o_req_t* rec_u = req_u->rec_u;

  rec_u->res.status = sas;
  rec_u->res.reason = (sas < 200) ? "weird" :
                      (sas < 300) ? "ok" :
                      (sas < 400) ? "moved" :
                      (sas < 500) ? "missing" :
                      "hosed";

  u3_hhed* hed_u = _http_heds_from_noun(u3k(hed));

  u3_hgen* gen_u = h2o_mem_alloc_shared(&rec_u->pool, sizeof(*gen_u),
                                        _http_hgen_dispose);
  gen_u->neg_u = (h2o_generator_t){0, 0};
  gen_u->hed_u = hed_u;

  while ( 0 != hed_u ) {
    h2o_add_header_by_str(&rec_u->pool, &rec_u->res.headers,
                          hed_u->nam_c, hed_u->nam_w, 0, 0,
                          hed_u->val_c, hed_u->val_w);
    hed_u = hed_u->nex_u;
  }

  gen_u->bod_u = _http_vec_from_octs(u3k(bod));
  rec_u->res.content_length = gen_u->bod_u.len;

  h2o_start_response(rec_u, &gen_u->neg_u);
  h2o_send(rec_u, &gen_u->bod_u, 1, H2O_SEND_STATE_FINAL);


  u3z(sas); u3z(hed); u3z(bod);
}

/* _http_rec_to_httq(): convert h2o_req_t to httq
*/
static u3_weak
_http_rec_to_httq(h2o_req_t* rec_u)
{
  u3_noun med = _http_vec_to_meth(rec_u->method);

  if ( u3_none == med ) {
    return u3_none;
  }

  u3_noun url = _http_vec_to_atom(rec_u->path);
  u3_noun hed = _http_heds_to_noun(rec_u->headers.entries,
                                   rec_u->headers.size);

  // restore host header
  hed = u3nc(u3nc(u3i_string("host"),
                  _http_vec_to_atom(rec_u->authority)),
             hed);

  u3_noun bod = _http_vec_to_octs(rec_u->entity);

  return u3nq(med, url, hed, bod);
}

typedef struct _h2o_uv_sock {         //  see private st_h2o_uv_socket_t
  h2o_socket_t     sok_u;             //  socket
  uv_stream_t*     han_u;             //  client stream handler (u3_hcon)
} h2o_uv_sock;

/* _http_rec_accept(); handle incoming http request from h2o.
*/
static c3_i
_http_rec_accept(h2o_handler_t* han_u, h2o_req_t* rec_u)
{
  u3_weak req = _http_rec_to_httq(rec_u);

  if ( u3_none == req ) {
    if ( (u3C.wag_w & u3o_verbose) ) {
      uL(fprintf(uH, "strange %.*s request\n", (int)rec_u->method.len,
                                               rec_u->method.base));
    }
    c3_c* msg_c = "bad request";
    h2o_send_error_generic(rec_u, 400, msg_c, msg_c, 0);
  }
  else {
    u3_lo_open();

    h2o_uv_sock* suv_u = (h2o_uv_sock*)rec_u->conn->
                           callbacks->get_socket(rec_u->conn);
    u3_hcon* hon_u = (u3_hcon*)suv_u->han_u;

    // sanity check
    c3_assert( hon_u->sok_u == &suv_u->sok_u );

    u3_hreq* req_u = _http_req_new(hon_u, rec_u);
    _http_req_dispatch(req_u, req);

    u3_lo_shut(c3y);
  }

  return 0;
}

/* _http_conn_find(): find http connection in server by sequence.
*/
static u3_hcon*
_http_conn_find(u3_http *htp_u, c3_w coq_l)
{
  u3_hcon* hon_u = htp_u->hon_u;

  //  XX glories of linear search
  //
  while ( hon_u ) {
    if ( coq_l == hon_u->coq_l ) {
      return hon_u;
    }
    hon_u = hon_u->nex_u;
  }
  return 0;
}

/* _http_conn_link(): link http request to connection
*/
static void
_http_conn_link(u3_http* htp_u, u3_hcon* hon_u)
{
  hon_u->htp_u = htp_u;
  hon_u->coq_l = htp_u->coq_l++;
  hon_u->nex_u = htp_u->hon_u;

  if ( 0 != hon_u->nex_u ) {
    hon_u->nex_u->pre_u = hon_u;
  }
  htp_u->hon_u = hon_u;
}

/* _http_conn_unlink(): remove http request from connection
*/
static void
_http_conn_unlink(u3_hcon* hon_u)
{
  if ( 0 != hon_u->pre_u ) {
    hon_u->pre_u->nex_u = hon_u->nex_u;

    if ( 0 != hon_u->nex_u ) {
      hon_u->nex_u->pre_u = hon_u->pre_u;
    }
  }
  else {
    hon_u->htp_u->hon_u = hon_u->nex_u;

    if ( 0 != hon_u->nex_u ) {
      hon_u->nex_u->pre_u = 0;
    }
  }
}

/* _http_conn_free(): free http connection on close.
*/
static void
_http_conn_free(uv_handle_t* han_t)
{
  u3_hcon* hon_u = (u3_hcon*)han_t;
  u3_http* htp_u = hon_u->htp_u;


  _http_conn_unlink(hon_u);

  // XX refactor graceful shutdown
  if ( (c3n == htp_u->liv) && (0 == htp_u->hon_u) ) {
    uL(fprintf(uH, "http conn free %d shutdown \n", hon_u->coq_l));
    _http_serv_close_hard(htp_u);
  }

  if ( 0 != hon_u->req_u ) {
    uL(fprintf(uH, "http conn free %d has reqs\n", hon_u->coq_l));

    u3_hreq* req_u = hon_u->req_u;

    u3_lo_open();

    while ( 0 != req_u ) {
      u3_hreq* nex_u = req_u->nex_u;

      if ( u3_rsat_plan == req_u->sat_e ) {
        _http_req_kill(req_u);
        _http_req_unlink(req_u);
      }

      req_u = nex_u;
    }

    u3_lo_shut(c3y);
  }

  free(hon_u);
}

// copied here for private close_connection

typedef void (*h2o_http1_upgrade_cb)(void *user_data, h2o_socket_t *sock, size_t reqsize);

struct st_h2o_http1_finalostream_t {
    h2o_ostream_t super;
    int sent_headers;
    struct {
        void *buf;
        h2o_ostream_pull_cb cb;
    } pull;
};

struct st_h2o_http1_conn_t {
    h2o_conn_t super;
    h2o_socket_t *sock;
    h2o_linklist_t _conns;
    h2o_timeout_t *_timeout;
    h2o_timeout_entry_t _timeout_entry;
    uint64_t _req_index;
    size_t _prevreqlen;
    size_t _reqsize;
    void* _req_entity_reader;
    struct st_h2o_http1_finalostream_t _ostr_final;
    struct {
        void *data;
        h2o_http1_upgrade_cb cb;
    } upgrade;
    h2o_req_t req;
};

/* _http_conn_close(): close http connection.
*/
static void
_http_conn_close(u3_hcon* hon_u)
{
  struct st_h2o_http1_conn_t* con_u = (void*)hon_u->con_u;

  //see private close_connection in h2o http1.c
  h2o_timeout_unlink(&con_u->_timeout_entry);
  h2o_dispose_request(&con_u->req);
  h2o_linklist_unlink(&con_u->_conns);
  free(con_u);

  h2o_socket_close(hon_u->sok_u);
}

/* _http_conn_close_soft(): gracefully close http connection.
*/
static void
_http_conn_close_soft(u3_hcon* hon_u)
{
  hon_u->liv = c3n;

  if ( 0 == hon_u->req_u ) {
    _http_conn_close(hon_u);
  }
  else {
    u3_hreq* req_u = hon_u->req_u;

    u3_lo_open();

    while ( 0 != req_u ) {
      u3_hreq* nex_u = req_u->nex_u;
      _http_req_cancel(req_u);
      req_u = nex_u;
    }

    u3_lo_shut(c3y);
  }
}

/* _http_conn_new(): create and accept http connection.
*/
static u3_hcon*
_http_conn_new(u3_http* htp_u)
{
  u3_hcon* hon_u = c3_malloc(sizeof(*hon_u));
  hon_u->seq_l = 1;
  hon_u->ipf_w = 0;
  hon_u->req_u = 0;
  hon_u->sok_u = 0;
  hon_u->con_u = 0;
  hon_u->pre_u = 0;

  _http_conn_link(htp_u, hon_u);

  return hon_u;
}

/* _http_serv_find(): find http server by sequence.
*/
static u3_http*
_http_serv_find(c3_l sev_l)
{
  u3_http* htp_u = u3_Host.htp_u;

  //  XX glories of linear search
  //
  while ( htp_u ) {
    if ( sev_l == htp_u->sev_l ) {
      return htp_u;
    }
    htp_u = htp_u->nex_u;
  }
  return 0;
}

/* _http_serv_link(): link http server to global state.
*/
static void
_http_serv_link(u3_http* htp_u)
{
  // XX link elsewhere initially, relink on start?

  if ( 0 != u3_Host.htp_u ) {
    htp_u->sev_l = 1 + u3_Host.htp_u->sev_l;
  }
  else {
    htp_u->sev_l = u3A->sev_l;
  }

  htp_u->nex_u = u3_Host.htp_u;
  u3_Host.htp_u = htp_u;
}

/* _http_serv_unlink(): remove http server from global state.
*/
static void
_http_serv_unlink(u3_http* htp_u)
{
  // XX link elsewhere initially, relink on start?

  if ( u3_Host.htp_u == htp_u ) {
    u3_Host.htp_u = htp_u->nex_u;
  }
  else {
    u3_http* pre_u = u3_Host.htp_u;

    //  XX glories of linear search
    //
    while ( pre_u ) {
      if ( pre_u->nex_u == htp_u ) {
        pre_u->nex_u = htp_u->nex_u;
      }
      else pre_u = pre_u->nex_u;
    }
  }
}

/* _http_serv_free(): free http server.
*/
static void
_http_serv_free(u3_http* htp_u)
{
  _http_serv_unlink(htp_u);

  // XX revisit, this is called twice when we're restarting
  while ( 0 != htp_u->hon_u ) {
    uL(fprintf(uH, "http serv free %d has conn\n", htp_u->hon_u->coq_l));
    _http_conn_close(htp_u->hon_u);
    htp_u->hon_u = htp_u->hon_u->nex_u;
  }

  if ( 0 != htp_u->h2o_u ) {
    u3_h2o_serv* h2o_u = htp_u->h2o_u;

    h2o_config_dispose(&h2o_u->fig_u);

    if ( 0 != h2o_u->cep_u.ssl_ctx ) {
      SSL_CTX_free(h2o_u->cep_u.ssl_ctx);
    }

    free(htp_u->h2o_u);
    htp_u->h2o_u = 0;
  }

  // XX refactor graceful shutdown
  if ( (c3n == htp_u->liv) && (0 == u3_Host.htp_u) ) {
    _http_serv_start_all();
  }

  free(htp_u);
}

/* _http_serv_close_hard(): close http server.
*/
static void
_http_serv_close_hard(u3_http* htp_u)
{
  uv_close((uv_handle_t*)&htp_u->wax_u,
           (uv_close_cb)_http_serv_free);
}

/* _http_serv_close_soft(): close http server gracefully.
*/
static void
_http_serv_close_soft(u3_http* htp_u)
{
  u3_h2o_serv* h2o_u = htp_u->h2o_u;
  h2o_context_request_shutdown(&h2o_u->ctx_u);

  htp_u->liv = c3n;

  if ( 0 == htp_u->hon_u ) {
    _http_serv_close_hard(htp_u);
  }
  else {
    u3_hcon* hon_u = htp_u->hon_u;

    while ( 0 != hon_u ) {
      _http_conn_close_soft(hon_u);
      hon_u = hon_u->nex_u;
    }
  }

  if ( 0 != htp_u->rox_u ) {
    // XX close soft
    _proxy_serv_close(htp_u->rox_u);
    htp_u->rox_u = 0;
  }
}

/* _http_serv_new(): create new http server.
*/
static u3_http*
_http_serv_new(c3_s por_s, c3_o sec, c3_o lop)
{
  u3_http* htp_u = c3_malloc(sizeof(*htp_u));

  htp_u->coq_l = 1;
  htp_u->por_s = por_s;
  htp_u->sec = sec;
  htp_u->lop = lop;
  htp_u->liv = c3y;
  htp_u->h2o_u = 0;
  htp_u->rox_u = 0;
  htp_u->hon_u = 0;
  htp_u->nex_u = 0;

  _http_serv_link(htp_u);

  return htp_u;
}

/* _http_serv_accept(): accept new http connection.
*/
static void
_http_serv_accept(u3_http* htp_u)
{
  if ( c3n == htp_u->liv ) {
    return;
  }

  u3_hcon* hon_u = _http_conn_new(htp_u);

  uv_tcp_init(u3L, &hon_u->wax_u);

  c3_i sas_i;

  if ( 0 != (sas_i = uv_accept((uv_stream_t*)&htp_u->wax_u,
                               (uv_stream_t*)&hon_u->wax_u)) ) {
    if ( (u3C.wag_w & u3o_verbose) ) {
      uL(fprintf(uH, "http: accept: %s\n", uv_strerror(sas_i)));
    }

    uv_close((uv_handle_t*)&hon_u->wax_u,
             (uv_close_cb)_http_conn_free);
    return;
  }

  hon_u->sok_u = h2o_uv_socket_create((uv_stream_t*)&hon_u->wax_u,
                                      (uv_close_cb)_http_conn_free);

  h2o_accept(&((u3_h2o_serv*)htp_u->h2o_u)->cep_u, hon_u->sok_u);

  // capture h2o connection (XX fragile)
  hon_u->con_u = (h2o_conn_t*)hon_u->sok_u->data;

  struct sockaddr_in adr_u;
  h2o_socket_getpeername(hon_u->sok_u, (struct sockaddr*)&adr_u);
  hon_u->ipf_w = ( adr_u.sin_family != AF_INET ) ?
                 0 : ntohl(adr_u.sin_addr.s_addr);
}

/* _http_serv_listen_cb(): uv_connection_cb for uv_listen
*/
static void
_http_serv_listen_cb(uv_stream_t* str_u, c3_i sas_i)
{
  u3_http* htp_u = (u3_http*)str_u;

  if ( 0 != sas_i ) {
    uL(fprintf(uH, "http: listen_cb: %s\n", uv_strerror(sas_i)));
  }
  else {
    _http_serv_accept(htp_u);
  }
}

/* _http_serv_init_h2o(): initialize h2o ctx and handlers for server.
*/
static u3_h2o_serv*
_http_serv_init_h2o(SSL_CTX* tls_u, c3_o log, c3_o red)
{
  u3_h2o_serv* h2o_u = c3_calloc(sizeof(*h2o_u));

  h2o_config_init(&h2o_u->fig_u);
  h2o_u->fig_u.server_name = h2o_iovec_init(
                               H2O_STRLIT("urbit/vere-" URBIT_VERSION));

  // XX default pending vhost/custom-domain design
  // XX revisit the effect of specifying the port
  h2o_u->hos_u = h2o_config_register_host(&h2o_u->fig_u,
                                          h2o_iovec_init(H2O_STRLIT("default")),
                                          65535);

  h2o_u->cep_u.ctx = (h2o_context_t*)&h2o_u->ctx_u;
  h2o_u->cep_u.hosts = h2o_u->fig_u.hosts;

  if ( 0 != tls_u ) {
    SSL_CTX_up_ref(tls_u);
  }

  h2o_u->cep_u.ssl_ctx = tls_u;

  h2o_u->han_u = h2o_create_handler(&h2o_u->hos_u->fallback_path,
                                    sizeof(*h2o_u->han_u));
  if ( c3y == red ) {
    // XX h2o_redirect_register
    h2o_u->han_u->on_req = _http_rec_accept;
  }
  else {
    h2o_u->han_u->on_req = _http_rec_accept;
  }

  if ( c3y == log ) {
    // XX enable access log
  }

  // XX h2o_compress_register

  h2o_context_init(&h2o_u->ctx_u, u3L, &h2o_u->fig_u);

  return h2o_u;
}

/* _http_serv_start(): start http server.
*/
static void
_http_serv_start(u3_http* htp_u)
{
  struct sockaddr_in adr_u;
  memset(&adr_u, 0, sizeof(adr_u));

  adr_u.sin_family = AF_INET;
  adr_u.sin_addr.s_addr = ( c3y == htp_u->lop ) ?
                          htonl(INADDR_LOOPBACK) :
                          INADDR_ANY;

  uv_tcp_init(u3L, &htp_u->wax_u);

  /*  Try ascending ports.
  */
  while ( 1 ) {
    c3_i sas_i;

    adr_u.sin_port = htons(htp_u->por_s);

    if ( 0 != (sas_i = uv_tcp_bind(&htp_u->wax_u,
                                   (const struct sockaddr*)&adr_u, 0)) ||
         0 != (sas_i = uv_listen((uv_stream_t*)&htp_u->wax_u,
                                 TCP_BACKLOG, _http_serv_listen_cb)) ) {
      if ( (UV_EADDRINUSE == sas_i) || (UV_EACCES == sas_i) ) {
        if ( (c3y == htp_u->sec) && (443 == htp_u->por_s) ) {
          htp_u->por_s = 8443;
        }
        else if ( (c3n == htp_u->sec) && (80 == htp_u->por_s) ) {
          htp_u->por_s = 8080;
        }
        else {
          htp_u->por_s++;
        }

        continue;
      }

      uL(fprintf(uH, "http: listen: %s\n", uv_strerror(sas_i)));
      _http_serv_free(htp_u);

      if ( 0 != htp_u->rox_u ) {
        _proxy_serv_free(htp_u->rox_u);
      }
      return;
    }

    // XX this is weird
    if ( 0 != htp_u->rox_u ) {
      htp_u->rox_u = _proxy_serv_start(htp_u->rox_u);
    }

    if ( 0 != htp_u->rox_u ) {
      uL(fprintf(uH, "http: live (%s, %s) on %d (proxied on %d)\n",
                   (c3y == htp_u->sec) ? "secure" : "insecure",
                   (c3y == htp_u->lop) ? "loopback" : "public",
                   htp_u->por_s,
                   htp_u->rox_u->por_s));
    }
    else {
      uL(fprintf(uH, "http: live (%s, %s) on %d\n",
                   (c3y == htp_u->sec) ? "secure" : "insecure",
                   (c3y == htp_u->lop) ? "loopback" : "public",
                   htp_u->por_s));
    }

    break;
  }
}

//XX deduplicate these with cttp

/* _cttp_mcut_char(): measure/cut character.
*/
static c3_w
_cttp_mcut_char(c3_c* buf_c, c3_w len_w, c3_c chr_c)
{
  if ( buf_c ) {
    buf_c[len_w] = chr_c;
  }
  return len_w + 1;
}

/* _cttp_mcut_cord(): measure/cut cord.
*/
static c3_w
_cttp_mcut_cord(c3_c* buf_c, c3_w len_w, u3_noun san)
{
  c3_w ten_w = u3r_met(3, san);

  if ( buf_c ) {
    u3r_bytes(0, ten_w, (c3_y *)(buf_c + len_w), san);
  }
  u3z(san);
  return (len_w + ten_w);
}

/* _cttp_mcut_path(): measure/cut cord list.
*/
static c3_w
_cttp_mcut_path(c3_c* buf_c, c3_w len_w, c3_c sep_c, u3_noun pax)
{
  u3_noun axp = pax;

  while ( u3_nul != axp ) {
    u3_noun h_axp = u3h(axp);

    len_w = _cttp_mcut_cord(buf_c, len_w, u3k(h_axp));
    axp = u3t(axp);

    if ( u3_nul != axp ) {
      len_w = _cttp_mcut_char(buf_c, len_w, sep_c);
    }
  }
  u3z(pax);
  return len_w;
}

static uv_buf_t
_http_wain_to_buf(u3_noun wan)
{
  c3_w len_w = _cttp_mcut_path(0, 0, (c3_c)10, u3k(wan));
  c3_c* buf_c = c3_malloc(1 + len_w);

  _cttp_mcut_path(buf_c, 0, (c3_c)10, wan);
  buf_c[len_w] = 0;

  return uv_buf_init(buf_c, len_w);
}

/* _http_init_tls: initialize OpenSSL context
*/
static SSL_CTX*
_http_init_tls(uv_buf_t key_u, uv_buf_t cer_u)
{
  // XX require 1.1.0 and use TLS_server_method()
  SSL_CTX* tls_u = SSL_CTX_new(SSLv23_server_method());
  // XX use SSL_CTX_set_max_proto_version() and SSL_CTX_set_min_proto_version()
  SSL_CTX_set_options(tls_u, SSL_OP_NO_SSLv2 |
                             SSL_OP_NO_SSLv3 |
                             // SSL_OP_NO_TLSv1 | // XX test
                             SSL_OP_NO_COMPRESSION);

  SSL_CTX_set_default_verify_paths(tls_u);
  SSL_CTX_set_session_cache_mode(tls_u, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_cipher_list(tls_u,
                          "ECDH+AESGCM:DH+AESGCM:ECDH+AES256:DH+AES256:"
                          "ECDH+AES128:DH+AES:ECDH+3DES:DH+3DES:RSA+AESGCM:"
                          "RSA+AES:RSA+3DES:!aNULL:!MD5:!DSS");

  {
    BIO* bio_u = BIO_new_mem_buf(key_u.base, key_u.len);
    // XX PKCS8 PEM_read_bio_PrivateKey
    RSA* rsa_u = PEM_read_bio_RSAPrivateKey(bio_u, 0, 0, 0);

    BIO_free(bio_u);

    if( (0 == rsa_u) ||
        (0 == SSL_CTX_use_RSAPrivateKey(tls_u, rsa_u)) ) {
      uL(fprintf(uH, "http: load private key failed:\n"));
      ERR_print_errors_fp(uH);
      uL(1);

      if ( 0 != rsa_u ) {
        RSA_free(rsa_u);
      }

      SSL_CTX_free(tls_u);
      return 0;
    }
  }

  {
    BIO* bio_u = BIO_new_mem_buf(cer_u.base, cer_u.len);
    X509* xer_u = PEM_read_bio_X509_AUX(bio_u, 0, 0, 0);

    if ( (0 == xer_u) ||
         (0 == SSL_CTX_use_certificate(tls_u, xer_u)) ) {
      uL(fprintf(uH, "http: load certificate failed:\n"));
      ERR_print_errors_fp(uH);
      uL(1);

      BIO_free(bio_u);

      if ( 0 != xer_u ) {
        X509_free(xer_u);
      }

      SSL_CTX_free(tls_u);

      return 0;
    }

    // freed on success too
    X509_free(xer_u);

    // XX require 1.02 or newer
    // SSL_CTX_clear_chain_certs(tls_u);

    // get any additional CA certs, ignoring errors
    while ( 0 != (xer_u = PEM_read_bio_X509(bio_u, 0, 0, 0)) ) {
      // XX require 1.0.2 or newer and use SSL_CTX_add0_chain_cert
      SSL_CTX_add_extra_chain_cert(tls_u, xer_u);
    }

    BIO_free(bio_u);
  }

  return tls_u;
}

/* _http_write_ports_file(): update .http.ports
*/
static void
_http_write_ports_file(c3_c *pax_c)
{
  c3_i    pal_i;
  c3_c    *paf_c;
  c3_i    por_i;
  u3_http *htp_u;

  pal_i = strlen(pax_c) + 13; /* includes NUL */
  paf_c = u3a_malloc(pal_i);
  snprintf(paf_c, pal_i, "%s/%s", pax_c, ".http.ports");

  por_i = open(paf_c, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  u3a_free(paf_c);

  for ( htp_u = u3_Host.htp_u; htp_u; htp_u = htp_u->nex_u ) {
    if ( 0 < htp_u->por_s ) {
      dprintf(por_i, "%u %s %s\n", htp_u->por_s,
                     (c3y == htp_u->sec) ? "secure" : "insecure",
                     (c3y == htp_u->lop) ? "loopback" : "public");
    }
  }

  c3_sync(por_i);
  close(por_i);
}

/* _http_release_ports_file(): remove .http.ports
*/
static void
_http_release_ports_file(c3_c *pax_c)
{
  c3_i pal_i;
  c3_c *paf_c;

  pal_i = strlen(pax_c) + 13; /* includes NUL */
  paf_c = u3a_malloc(pal_i);
  snprintf(paf_c, pal_i, "%s/%s", pax_c, ".http.ports");

  unlink(paf_c);
  u3a_free(paf_c);
}

/* _http_czar_host(): galaxy hostname as (unit host:eyre)
*/
static u3_noun
_http_czar_host(void)
{
  u3_noun dom = u3_nul;

  if ( (0 == u3_Host.ops_u.imp_c) || (c3n == u3_Host.ops_u.net) ) {
    return dom;
  }

  {
    c3_c* dns_c = u3_Host.ops_u.dns_c;
    c3_w  len_w = strlen(dns_c);
    c3_w  dif_w;
    c3_c* dom_c;
    c3_c* dot_c;

    while ( 0 != len_w ) {
      if ( 0 == (dot_c = strchr(dns_c, '.'))) {
        len_w = 0;
        dom = u3nc(u3i_string(dns_c), dom);
        break;
      }
      else {
        dif_w = dot_c - dns_c;
        dom_c = c3_malloc(1 + dif_w);
        strncpy(dom_c, dns_c, dif_w);
        dom_c[dif_w] = 0;

        dom = u3nc(u3i_string(dom_c), dom);

        // increment to skip leading '.'
        dns_c = dot_c + 1;
        free(dom_c);

        // XX confirm that underflow is impossible here
        len_w -= c3_min(dif_w, len_w);
      }
    }
  }

  if ( u3_nul == dom ) {
    return dom;
  }

  // increment to skip '~'
  dom = u3nc(u3i_string(u3_Host.ops_u.imp_c + 1), u3kb_flop(u3k(dom)));

  return u3nt(u3_nul, c3y, u3kb_flop(u3k(dom)));
}

/* u3_http_ef_bake(): notify %eyre that we're live
*/
void
u3_http_ef_bake(void)
{
  u3_noun ipf = u3_nul;

  {
    struct ifaddrs* iad_u;
    getifaddrs(&iad_u);

    struct ifaddrs* dia_u = iad_u;

    while ( iad_u ) {
      struct sockaddr_in* adr_u = (struct sockaddr_in *)iad_u->ifa_addr;

      if ( (0 != adr_u) && (AF_INET == adr_u->sin_family) ) {
        c3_w ipf_w = ntohl(adr_u->sin_addr.s_addr);

        if ( INADDR_LOOPBACK != ipf_w ) {
          ipf = u3nc(u3nc(c3n, u3i_words(1, &ipf_w)), ipf);
        }
      }

      iad_u = iad_u->ifa_next;
    }

    freeifaddrs(dia_u);
  }

  u3_noun hot = _http_czar_host();

  if ( u3_nul != hot ) {
    ipf = u3nc(u3k(u3t(hot)), ipf);
    u3z(hot);
  }

  u3_noun pax = u3nq(u3_blip, c3__http, u3k(u3A->sen), u3_nul);

  u3v_plan(pax, u3nc(c3__born, ipf));
}

/* u3_http_ef_thou(): send %thou from %eyre as http response.
*/
void
u3_http_ef_thou(c3_l     sev_l,
                c3_l     coq_l,
                c3_l     seq_l,
                u3_noun  rep)
{
  u3_http* htp_u;
  u3_hcon* hon_u;
  u3_hreq* req_u;
  c3_w bug_w = u3C.wag_w & u3o_verbose;

  if ( !(htp_u = _http_serv_find(sev_l)) ) {
    if ( bug_w ) {
      uL(fprintf(uH, "http: server not found: %x\r\n", sev_l));
    }
  }
  else if ( !(hon_u = _http_conn_find(htp_u, coq_l)) ) {
    if ( bug_w ) {
      uL(fprintf(uH, "http: connection not found: %x/%d\r\n", sev_l, coq_l));
    }
  }
  else if ( !(req_u = _http_req_find(hon_u, seq_l)) ) {
    if ( bug_w ) {
      uL(fprintf(uH, "http: request not found: %x/%d/%d\r\n",
                 			sev_l, coq_l, seq_l));
    }
  }
  else {
    u3_noun p_rep, q_rep, r_rep;

    if ( c3n == u3r_trel(rep, &p_rep, &q_rep, &r_rep) ) {
      uL(fprintf(uH, "http: strange response\n"));
    }
    else {
      _http_req_respond(req_u, u3k(p_rep), u3k(q_rep), u3k(r_rep));
    }
  }

  u3z(rep);
}

static void
_http_serv_start_all(void)
{
  // serv_close has already been called, but they
  // might not have been freed or unlinked yet
  u3_Host.htp_u = 0;

  // this may be shared across servers, so
  // there's no good place for it to go
  u3_Host.tls_u = 0;

  // if ( 0 != u3_Host.fig_u.tim_u ) {
  //   uv_timer_stop(u3_Host.fig_u.tim_u);
  //   free(u3_Host.fig_u.tim_u);
  //   u3_Host.fig_u.tim_u = 0;
  // }

  u3_http* htp_u;
  c3_s por_s;

  u3_form* for_u = u3_Host.fig_u.for_u;

  //  HTTPS server.
  if ( (0 != for_u->key_u.base) && (0 != for_u->cer_u.base) ) {
    u3_Host.tls_u = _http_init_tls(for_u->key_u, for_u->cer_u);

    if ( 0 != u3_Host.tls_u ) {
      por_s = ( c3y == for_u->pro ) ? 8443 : 443;
      htp_u = _http_serv_new(por_s, c3y, c3n);
      htp_u->h2o_u = _http_serv_init_h2o(u3_Host.tls_u, for_u->log, for_u->red);

      if ( (c3y == for_u->pro) || (c3y == u3_Host.ops_u.fak) ) {
        htp_u->rox_u = _proxy_serv_new(htp_u, 443, c3y);
      }
    }
  }

  //  HTTP server.
  {
    por_s = ( c3y == for_u->pro ) ? 8080 : 80;
    htp_u = _http_serv_new(por_s, c3n, c3n);
    htp_u->h2o_u = _http_serv_init_h2o(0, for_u->log, for_u->red);

    if ( (c3y == for_u->pro) || (c3y == u3_Host.ops_u.fak) ) {
      htp_u->rox_u = _proxy_serv_new(htp_u, 80, c3n);
    }
  }

  //  Loopback server.
  {
    por_s = 12321;
    htp_u = _http_serv_new(por_s, c3n, c3y);
    htp_u->h2o_u = _http_serv_init_h2o(0, for_u->log, for_u->red);
    // never proxied
  }

  for ( htp_u = u3_Host.htp_u; htp_u; htp_u = htp_u->nex_u ) {
    _http_serv_start(htp_u);
  }

  //  send listening ports to %eyre
  {
    u3_noun pax = u3nq(u3_blip, c3__http, u3k(u3A->sen), u3_nul);
    u3_noun sec = u3_nul;
    u3_noun non = u3_none;

    for ( htp_u = u3_Host.htp_u; htp_u; htp_u = htp_u->nex_u ) {
      if ( c3n == htp_u->lop ) {
        if ( c3y == htp_u->sec ) {
          sec = u3nc(u3_nul, htp_u->por_s);
        }
        else {
          non = htp_u->por_s;
        }
      }
    }

    c3_assert( u3_none != non );

    // XX u3_lo_open();

    u3v_plan(pax, u3nt(c3__live, non, sec));

    // XX u3_lo_shut(c3y);
  }

  _http_write_ports_file(u3_Host.dir_c);
}

/* _http_serv_restart_cb(): force shutdown, then start servers.
*/
static void
_http_serv_restart_cb(uv_timer_t* tim_u)
{
  u3_http* htp_u;

  free(tim_u);
  u3_Host.fig_u.tim_u = 0;

  uL(fprintf(uH, "http: force shutdown\n"));

  for ( htp_u = u3_Host.htp_u; htp_u; htp_u = htp_u->nex_u ) {
    _http_serv_close_hard(htp_u);
  }

  _http_serv_start_all();
}

/* _http_serv_restart(): gracefully shutdown, then start servers.
*/
static void
_http_serv_restart(void)
{
  u3_http* htp_u;

  if ( 0 == u3_Host.htp_u ) {
    _http_serv_start_all();
    return;
  }

  uL(fprintf(uH, "http: restarting servers to apply configuration\n"));

  for ( htp_u = u3_Host.htp_u; htp_u; htp_u = htp_u->nex_u ) {
    // flag that servers are coming down
    // stop accepting connections
    _http_serv_close_soft(htp_u);
  }

  // u3_Host.fig_u.tim_u = c3_malloc(sizeof(uv_timer_t));

  // uv_timer_init(u3L, u3_Host.fig_u.tim_u);
  // // XX how long?
  // uv_timer_start(u3_Host.fig_u.tim_u, _http_serv_restart_cb, 0, 0);

  _http_release_ports_file(u3_Host.dir_c);
}

static void
_http_form_free(u3_form* for_u)
{
  if ( 0 != for_u->key_u.base ) {
    free(for_u->key_u.base);
  }

  if ( 0 != for_u->cer_u.base ) {
    free(for_u->cer_u.base);
  }

  free(for_u);
}

/* u3_http_ef_form(): apply configuration, restart servers.
*/
void
u3_http_ef_form(u3_noun fig)
{
  // XX validate / test axes?
  u3_noun sec = u3h(fig);
  u3_noun lob = u3t(fig);

  u3_form* for_u = c3_malloc(sizeof(*for_u));

  for_u->pro = (c3_o)u3h(lob);
  for_u->log = (c3_o)u3h(u3t(lob));
  for_u->red = (c3_o)u3t(u3t(lob));

  if ( u3_nul != sec ) {
    u3_noun key = u3h(u3t(sec));
    u3_noun cer = u3t(u3t(sec));

    for_u->key_u = _http_wain_to_buf(u3k(key));
    for_u->cer_u = _http_wain_to_buf(u3k(cer));
  }
  else {
    for_u->key_u = uv_buf_init(0, 0);
    for_u->cer_u = uv_buf_init(0, 0);
  }

  u3z(fig);

  if ( 0 != u3_Host.fig_u.for_u ) {
    _http_form_free(u3_Host.fig_u.for_u);
  }

  u3_Host.fig_u.for_u = for_u;

  _http_serv_restart();
}

/* u3_http_io_init(): initialize http I/O.
*/
void
u3_http_io_init(void)
{
  u3_Host.fig_u.for_u = 0;
  u3_Host.fig_u.cli_u = 0;
  u3_Host.fig_u.con_u = 0;
}

/* u3_http_io_talk(): start http I/O.
*/
void
u3_http_io_talk(void)
{
}

/* u3_http_io_poll(): poll kernel for http I/O.
*/
void
u3_http_io_poll(void)
{
}

/* u3_http_io_exit(): shut down http.
*/
void
u3_http_io_exit(void)
{
  // Note: nothing in this codepath can print to uH!
  // it will seriously mess up your terminal

  // u3_http* htp_u;

  // for ( htp_u = u3_Host.htp_u; htp_u; htp_u = htp_u->nex_u ) {
  //   _http_serv_close_hard(htp_u);
  // }

  // XX close u3_Host.fig_u.cli_u and con_u

  _http_release_ports_file(u3_Host.dir_c);
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

typedef enum {
  u3_pars_good = 0,                   //  success
  u3_pars_fail = 1,                   //  failure
  u3_pars_moar = 2                    //  incomplete
} u3_proxy_pars;

/* _proxy_alloc(): libuv buffer allocator
*/
static void
_proxy_alloc(uv_handle_t* had_u,
             size_t len_i,
             uv_buf_t* buf)
{
  // len_i is always 64k, so we're ignoring it
  // using fixed size 4K buffer for
  // XX consider h2o_buffer_t, a pool, or something XX
  void* ptr_v = c3_malloc(4096);
  *buf = uv_buf_init(ptr_v, 4096);
}

/* _proxy_warc_link(): link warc to global state.
*/
static void
_proxy_warc_link(u3_warc* cli_u)
{
  cli_u->nex_u = u3_Host.fig_u.cli_u;

  if ( 0 != cli_u->nex_u ) {
    cli_u->nex_u->pre_u = cli_u;
  }
  u3_Host.fig_u.cli_u = cli_u;
}

/* _proxy_warc_unlink(): unlink warc from global state.
*/
static void
_proxy_warc_unlink(u3_warc* cli_u)
{
  if ( 0 != cli_u->pre_u ) {
    cli_u->pre_u->nex_u = cli_u->nex_u;

    if ( 0 != cli_u->nex_u ) {
      cli_u->nex_u->pre_u = cli_u->pre_u;
    }
  }
  else {
    u3_Host.fig_u.cli_u = cli_u->nex_u;

    if ( 0 != cli_u->nex_u ) {
      cli_u->nex_u->pre_u = 0;
    }
  }
}

/* _proxy_warc_free(): free ward client
*/
static void
_proxy_warc_free(u3_warc* cli_u)
{
  _proxy_warc_unlink(cli_u);
  free(cli_u);
}

/* _proxy_warc_new(): allocate ship-specific proxy client
*/
static u3_warc*
_proxy_warc_new(u3_http* htp_u, u3_atom sip, c3_s por_s, c3_o sec)
{
  u3_warc* cli_u = c3_malloc(sizeof(*cli_u));
  cli_u->sip = sip;
  cli_u->sec = sec;
  cli_u->htp_u = htp_u;
  cli_u->por_s = por_s; // set after opened
  cli_u->nex_u = 0;
  cli_u->pre_u = 0;

  _proxy_warc_link(cli_u);

  return cli_u;
}

/* _proxy_conn_link(): link con to listener or global state.
*/
static void
_proxy_conn_link(u3_pcon* con_u)
{
  switch ( con_u->typ_e ) {
    default: c3_assert(0);

    case u3_ptyp_ward: {
      con_u->nex_u = u3_Host.fig_u.con_u;

      if ( 0 != con_u->nex_u ) {
        con_u->nex_u->pre_u = con_u;
      }
      u3_Host.fig_u.con_u = con_u;
      break;
    }

    case u3_ptyp_prox: {
      u3_prox* lis_u = con_u->src_u.lis_u;
      con_u->nex_u = lis_u->con_u;

      if ( 0 != con_u->nex_u ) {
        con_u->nex_u->pre_u = con_u;
      }
      lis_u->con_u = con_u;
      break;
    }
  }
}

/* _proxy_conn_unlink(): unlink con from listener or global state.
*/
static void
_proxy_conn_unlink(u3_pcon* con_u)
{
  if ( 0 != con_u->pre_u ) {
    con_u->pre_u->nex_u = con_u->nex_u;

    if ( 0 != con_u->nex_u ) {
      con_u->nex_u->pre_u = con_u->pre_u;
    }
  }
  else {
    switch ( con_u->typ_e ) {
      default: c3_assert(0);

      case u3_ptyp_ward: {
        u3_Host.fig_u.con_u = con_u->nex_u;

        if ( 0 != con_u->nex_u ) {
          con_u->nex_u->pre_u = 0;
        }
        break;
      }

      case u3_ptyp_prox: {
        u3_prox* lis_u = con_u->src_u.lis_u;
        lis_u->con_u = con_u->nex_u;

        if ( 0 != con_u->nex_u ) {
          con_u->nex_u->pre_u = 0;
        }
        break;
      }
    }
  }
}

/* _proxy_conn_free(): free proxy connection
*/
static void
_proxy_conn_free(u3_pcon* con_u)
{
  if ( 0 != con_u->buf_u.base ) {
    free(con_u->buf_u.base);
  }

  if ( u3_ptyp_ward == con_u->typ_e ) {
    _proxy_warc_free(con_u->src_u.cli_u);
  }

  _proxy_conn_unlink(con_u);

  free(con_u);
}

/* _proxy_conn_close(): close both sides of proxy connection
*/
static void
_proxy_conn_close(u3_pcon* con_u)
{
  // XX revisit, this is called twice when con_u
  // is a loopback connection and we're restarting
  if ( uv_is_closing((uv_handle_t*)&con_u->don_u) ){
    return;
  }

  if ( 0 != con_u->upt_u ) {
    uv_close((uv_handle_t*)con_u->upt_u, (uv_close_cb)free);
  }

  uv_close((uv_handle_t*)&con_u->don_u, (uv_close_cb)_proxy_conn_free);
}

/* _proxy_conn_new(): allocate proxy connection
*/
static u3_pcon*
_proxy_conn_new(u3_proxy_type typ_e, void* src_u)
{
  u3_pcon* con_u = c3_malloc(sizeof(*con_u));
  con_u->upt_u = 0;
  con_u->buf_u = uv_buf_init(0, 0);
  con_u->nex_u = 0;
  con_u->pre_u = 0;

  switch ( typ_e ) {
    default: c3_assert(0);

    case u3_ptyp_prox: {
      u3_prox* lis_u = (u3_prox*)src_u;
      con_u->typ_e = typ_e;
      con_u->src_u.lis_u = lis_u;
      con_u->sec = lis_u->sec;
      break;
    }

    case u3_ptyp_ward: {
      u3_warc* cli_u = (u3_warc*)src_u;
      con_u->typ_e = typ_e;
      con_u->src_u.cli_u = cli_u;
      con_u->sec = cli_u->sec;
      break;
    }
  }

  con_u->don_u.data = con_u;

  _proxy_conn_link(con_u);

  return con_u;
}

/* _proxy_write_cb(): free uv_write_t and linked buffer.
*/
static void
_proxy_write_cb(uv_write_t* wri_u, c3_i sas_i)
{
  if ( 0 != sas_i ) {
    uL(fprintf(uH, "proxy: write: %s\n", uv_strerror(sas_i)));
  }

  if ( 0 != wri_u->data ) {
    free(wri_u->data);
  }

  free(wri_u);
}

/* _proxy_write(): write buffer to proxy stream
*/
static c3_i
_proxy_write(u3_pcon* con_u, uv_stream_t* str_u, uv_buf_t buf_u)
{
  uv_write_t* wri_u = c3_malloc(sizeof(*wri_u));
  wri_u->data = buf_u.base;

  c3_i sas_i;
  if ( 0 != (sas_i = uv_write(wri_u, str_u, &buf_u, 1, _proxy_write_cb)) ) {
    _proxy_conn_close(con_u);
    _proxy_write_cb(wri_u, sas_i);
  }

  return sas_i;
}

/* _proxy_read_downstream_cb(): read from downstream, write upstream.
*/
static void
_proxy_read_downstream_cb(uv_stream_t* don_u,
                          ssize_t      siz_w,
                          const uv_buf_t* buf_u)
{
  u3_pcon* con_u = don_u->data;

  if ( 0 > siz_w ) {
    if ( UV_EOF != siz_w ) {
      uL(fprintf(uH, "proxy: read downstream: %s\n", uv_strerror(siz_w)));
    }
    _proxy_conn_close(con_u);
  }
  else {
    _proxy_write(con_u, (uv_stream_t*)con_u->upt_u,
                 uv_buf_init(buf_u->base, siz_w));
  }
}

/* _proxy_read_upstream_cb(): read from upstream, write downstream.
*/
static void
_proxy_read_upstream_cb(uv_stream_t* upt_u,
                        ssize_t      siz_w,
                        const uv_buf_t* buf_u)
{
  u3_pcon* con_u = upt_u->data;

  if ( 0 > siz_w ) {
    if ( UV_EOF != siz_w ) {
      uL(fprintf(uH, "proxy: read upstream: %s\n", uv_strerror(siz_w)));
    }
    _proxy_conn_close(con_u);
  }
  else {
    _proxy_write(con_u, (uv_stream_t*)&(con_u->don_u),
                 uv_buf_init(buf_u->base, siz_w));
  }
}

/* _proxy_fire(): send pending buffer upstream, setup full duplex.
*/
static void
_proxy_fire(u3_pcon* con_u)
{
  if ( 0 != con_u->buf_u.base ) {
    uv_buf_t fub_u = con_u->buf_u;
    con_u->buf_u = uv_buf_init(0, 0);

    if ( 0 != _proxy_write(con_u, (uv_stream_t*)con_u->upt_u, fub_u) ) {
      return;
    }
  }

  // XX set cooldown timers to close these?

  uv_read_start((uv_stream_t*)&con_u->don_u,
                _proxy_alloc, _proxy_read_downstream_cb);

  uv_read_start((uv_stream_t*)con_u->upt_u,
                _proxy_alloc, _proxy_read_upstream_cb);
}

/* _proxy_loop_connect_cb(): callback for loopback proxy connect.
*/
static void
_proxy_loop_connect_cb(uv_connect_t * upc_u, c3_i sas_i)
{
  u3_pcon* con_u = upc_u->data;

  if ( 0 != sas_i ) {
    uL(fprintf(uH, "proxy: connect: %s\n", uv_strerror(sas_i)));
    _proxy_conn_close(con_u);
  }
  else {
    _proxy_fire(con_u);
  }

  free(upc_u);
}

/* _proxy_loop_connect(): connect to loopback.
*/
static void
_proxy_loop_connect(u3_pcon* con_u)
{
  uv_tcp_t* upt_u = c3_malloc(sizeof(*upt_u));

  con_u->upt_u = upt_u;
  upt_u->data = con_u;

  uv_tcp_init(u3L, upt_u);

  struct sockaddr_in lop_u;

  memset(&lop_u, 0, sizeof(lop_u));
  lop_u.sin_family = AF_INET;
  lop_u.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  // get the loopback port from the linked server
  {
    u3_http* htp_u;

    switch ( con_u->typ_e ) {
      default: c3_assert(0);

      case u3_ptyp_ward: {
        htp_u = con_u->src_u.cli_u->htp_u;
        break;
      }

      case u3_ptyp_prox: {
        htp_u = con_u->src_u.lis_u->htp_u;
        break;
      }
    }

    // XX make unpossible?
    c3_assert( (0 != htp_u) && (0 != htp_u->por_s) );

    lop_u.sin_port = htons(htp_u->por_s);
  }

  uv_connect_t* upc_u = c3_malloc(sizeof(*upc_u));
  upc_u->data = con_u;

  c3_i sas_i;

  if ( 0 != (sas_i = uv_tcp_connect(upc_u, upt_u,
                                    (const struct sockaddr*)&lop_u,
                                    _proxy_loop_connect_cb)) ) {
    uL(fprintf(uH, "proxy: connect: %s\n", uv_strerror(sas_i)));
    free(upc_u);
    _proxy_conn_close(con_u);
  }
}

/* _proxy_ward_link(): link ward to listener.
*/
static void
_proxy_ward_link(u3_pcon* con_u, u3_ward* rev_u)
{
  // XX link also to con_u as upstream?
  c3_assert( u3_ptyp_prox == con_u->typ_e );

  u3_prox* lis_u = con_u->src_u.lis_u;

  rev_u->nex_u = lis_u->rev_u;

  if ( 0 != rev_u->nex_u ) {
    rev_u->nex_u->pre_u = rev_u;
  }
  lis_u->rev_u = rev_u;
}

/* _proxy_ward_unlink(): unlink ward from listener.
*/
static void
_proxy_ward_unlink(u3_ward* rev_u)
{
  if ( 0 != rev_u->pre_u ) {
    rev_u->pre_u->nex_u = rev_u->nex_u;

    if ( 0 != rev_u->nex_u ) {
      rev_u->nex_u->pre_u = rev_u->pre_u;
    }
  }
  else {
    c3_assert( u3_ptyp_prox == rev_u->con_u->typ_e );

    u3_prox* lis_u = rev_u->con_u->src_u.lis_u;
    lis_u->rev_u = rev_u->nex_u;

    if ( 0 != rev_u->nex_u ) {
      rev_u->nex_u->pre_u = 0;
    }
  }
}

/* _proxy_ward_free(): free reverse proxy listener
*/
static void
_proxy_ward_free(u3_ward* rev_u)
{
  u3z(rev_u->sip);
  _proxy_ward_unlink(rev_u);
  free(rev_u);
}

/* _proxy_ward_close(): close ward (ship-specific listener)
*/
static void
_proxy_ward_close(u3_ward* rev_u)
{
  uv_timer_stop((uv_timer_t*)&rev_u->tim_u);
  uv_close((uv_handle_t*)&rev_u->tcp_u, (uv_close_cb)_proxy_ward_free);
}

/* _proxy_ward_new(): allocate reverse proxy listener
*/
static u3_ward*
_proxy_ward_new(u3_pcon* con_u, u3_atom sip)
{
  u3_ward* rev_u = c3_malloc(sizeof(*rev_u));
  rev_u->tcp_u.data = rev_u;
  rev_u->tim_u.data = rev_u;
  rev_u->con_u = con_u;
  rev_u->sip = sip;
  rev_u->por_s = 0; // set after opened
  rev_u->nex_u = 0;
  rev_u->pre_u = 0;

  _proxy_ward_link(con_u, rev_u);

  return rev_u;
}

/* _proxy_ward_accept(): accept new connection on ward
*/
static void
_proxy_ward_accept(u3_ward* rev_u)
{
  uv_tcp_t* upt_u = c3_malloc(sizeof(*upt_u));

  upt_u->data = rev_u->con_u;
  rev_u->con_u->upt_u = upt_u;

  uv_tcp_init(u3L, upt_u);

  c3_i sas_i;

  if ( 0 != (sas_i = uv_accept((uv_stream_t*)&rev_u->tcp_u,
                               (uv_stream_t*)upt_u)) ) {
    uL(fprintf(uH, "proxy: accept: %s\n", uv_strerror(sas_i)));
    _proxy_conn_close(rev_u->con_u);
  }
  else {
    _proxy_fire(rev_u->con_u);
  }
}

/* _proxy_ward_listen_cb(): listen callback for ward
*/
static void
_proxy_ward_listen_cb(uv_stream_t* tcp_u, c3_i sas_i)
{
  u3_ward* rev_u = (u3_ward*)tcp_u;

  if ( 0 != sas_i ) {
    uL(fprintf(uH, "proxy: listen_cb: %s\n", uv_strerror(sas_i)));
    _proxy_conn_close(rev_u->con_u);
  }
  else {
    _proxy_ward_accept(rev_u);
  }

  // ward listener is always closed
  _proxy_ward_close(rev_u);
}

/* _proxy_ward_timer_cb(): expiration timer for ward
*/
static void
_proxy_ward_timer_cb(uv_timer_t* tim_u)
{
  u3_ward* rev_u = tim_u->data;

  if ( 0 != rev_u ) {
    uL(fprintf(uH, "proxy: ward expired: %d\n", rev_u->por_s));
    _proxy_ward_close(rev_u);
  }
}

/* _proxy_ward_plan(): notify ship of new ward
*/
static void
_proxy_ward_plan(u3_ward* rev_u)
{
  // XX confirm duct
  u3_noun pax = u3nq(u3_blip, c3__http, c3__prox,
                     u3nc(u3k(u3A->sen), u3_nul));
  u3_noun wis = u3nq(c3__wise, u3k(rev_u->sip),
                               rev_u->por_s,
                               u3k(rev_u->con_u->sec));
  u3v_plan(pax, wis);
}

/* _proxy_ward_start(): start ward (ship-specific listener).
*/
static void
_proxy_ward_start(u3_pcon* con_u, u3_noun sip)
{
  u3_ward* rev_u = _proxy_ward_new(con_u, sip);

  uv_tcp_init(u3L, &rev_u->tcp_u);

  struct sockaddr_in add_u;
  c3_i add_i = sizeof(add_u);
  memset(&add_u, 0, add_i);
  add_u.sin_family = AF_INET;
  add_u.sin_addr.s_addr = INADDR_ANY;
  add_u.sin_port = 0;  // first available

  c3_i sas_i;

  if ( 0 != (sas_i = uv_tcp_bind(&rev_u->tcp_u,
                                 (const struct sockaddr*)&add_u, 0)) ||
       0 != (sas_i = uv_listen((uv_stream_t*)&rev_u->tcp_u,
                               TCP_BACKLOG, _proxy_ward_listen_cb)) ||
       0 != (sas_i = uv_tcp_getsockname(&rev_u->tcp_u,
                                        (struct sockaddr*)&add_u, &add_i))) {
    uL(fprintf(uH, "proxy: ward: %s\n", uv_strerror(sas_i)));
    _proxy_ward_close(rev_u);
    _proxy_conn_close(con_u);
  }
  else {
    // XX u3_lo_open();

    rev_u->por_s = ntohs(add_u.sin_port);
    _proxy_ward_plan(rev_u);

    uv_timer_init(u3L, &rev_u->tim_u);

    // XX how long?
    uv_timer_start(&rev_u->tim_u, _proxy_ward_timer_cb, 30 * 1000, 0);

    // XX u3_lo_shut(c3y);
  }
}

/* _proxy_ward_connect_cb(): ward connection callback
*/
static void
_proxy_ward_connect_cb(uv_connect_t * upc_u, c3_i sas_i)
{
  u3_pcon* con_u = upc_u->data;

  if ( 0 != sas_i ) {
    uL(fprintf(uH, "proxy: ward connect: %s\n", uv_strerror(sas_i)));
    _proxy_conn_close(con_u);
  }
  else {
    _proxy_loop_connect(con_u);
  }

  free(upc_u);
}

/* _proxy_ward_connect(): connect to remote ward
*/
static void
_proxy_ward_connect(u3_warc* cli_u)
{
  u3_pcon* con_u = _proxy_conn_new(u3_ptyp_ward, cli_u);

  uv_tcp_init(u3L, &con_u->don_u);

  if ( 0 == cli_u->hot_c ) {
    c3_c* sip_c = u3r_string(u3dc("scot", 'p', u3k(cli_u->sip)));
    c3_w len_w = 1 + strlen(sip_c) + strlen(u3_Host.ops_u.dns_c);
    cli_u->hot_c = c3_malloc(len_w);
    // incremented to skip '~'
    snprintf(cli_u->hot_c, len_w, "%s.%s", sip_c + 1, u3_Host.ops_u.dns_c);

    free(sip_c);
  }

  struct sockaddr_in add_u;

  memset(&add_u, 0, sizeof(add_u));
  add_u.sin_family = AF_INET;
  add_u.sin_addr.s_addr = htonl(cli_u->ipf_w);
  add_u.sin_port = htons(cli_u->por_s);

  uv_connect_t* upc_u = c3_malloc(sizeof(*upc_u));
  upc_u->data = con_u;

  c3_i sas_i;

  if ( 0 != (sas_i = uv_tcp_connect(upc_u, &con_u->don_u,
                                    (const struct sockaddr*)&add_u,
                                    _proxy_ward_connect_cb)) ) {
      uL(fprintf(uH, "proxy: ward connect: %s\n", uv_strerror(sas_i)));
      free(upc_u);
      _proxy_conn_close(con_u);
  }
}

/* _proxy_ward_resolve_cb(): ward IP address resolution callback
*/
static void
_proxy_ward_resolve_cb(uv_getaddrinfo_t* adr_u,
                       c3_i              sas_i,
                       struct addrinfo*  aif_u)
{
  u3_warc* cli_u = adr_u->data;

  if ( 0 != sas_i ) {
    uL(fprintf(uH, "proxy: ward: resolve: %s\n", uv_strerror(sas_i)));
    _proxy_warc_free(cli_u);
  }
  else {
    // XX traverse struct a la _ames_czar_cb
    cli_u->ipf_w = ntohl(((struct sockaddr_in *)aif_u->ai_addr)->sin_addr.s_addr);
    _proxy_ward_connect(cli_u);
  }

  free(adr_u);
  uv_freeaddrinfo(aif_u);
}

/* _proxy_reverse_resolve(): resolve IP address of remote ward
*/
static void
_proxy_ward_resolve(u3_warc* cli_u)
{
  uv_getaddrinfo_t* adr_u = c3_malloc(sizeof(*adr_u));
  adr_u->data = cli_u;

  struct addrinfo hin_u;
  memset(&hin_u, 0, sizeof(struct addrinfo));

  hin_u.ai_family = PF_INET;
  hin_u.ai_socktype = SOCK_STREAM;
  hin_u.ai_protocol = IPPROTO_TCP;

  // XX set port?

  c3_i sas_i;

  if ( 0 != (sas_i = uv_getaddrinfo(u3L, adr_u, _proxy_ward_resolve_cb,
                                         cli_u->hot_c, 0, &hin_u)) ) {
    uL(fprintf(uH, "proxy: ward: resolve: %s\n", uv_strerror(sas_i)));
    _proxy_warc_free(cli_u);
  }
}

/* _proxy_parse_host(): parse plaintext buffer for Host header
*/
static u3_proxy_pars
_proxy_parse_host(const uv_buf_t* buf_u, c3_c** hot_c)
{
  struct phr_header hed_u[H2O_MAX_HEADERS];
  size_t hed_t = H2O_MAX_HEADERS;

  {
    // unused
    c3_i        ver_i;
    const c3_c* met_c;
    size_t      met_t;
    const c3_c* pat_c;
    size_t      pat_t;

    size_t len_t = buf_u->len < H2O_MAX_REQLEN ? buf_u->len : H2O_MAX_REQLEN;
    // XX slowloris?
    c3_i las_i = 0;
    c3_i sas_i;

    sas_i = phr_parse_request(buf_u->base, len_t, &met_c, &met_t,
                              &pat_c, &pat_t, &ver_i, hed_u, &hed_t, las_i);

    switch ( sas_i ) {
      case -1: return u3_pars_fail;
      case -2: return u3_pars_moar;
    }
  }

  const h2o_token_t* tok_t;
  size_t i;

  for ( i = 0; i < hed_t; i++ ) {
    // XX in-place, copy first
    h2o_strtolower((c3_c*)hed_u[i].name, hed_u[i].name_len);

    if ( 0 != (tok_t = h2o_lookup_token(hed_u[i].name, hed_u[i].name_len)) ) {
      if ( tok_t->is_init_header_special && H2O_TOKEN_HOST == tok_t ) {
        c3_c* val_c;
        c3_c* por_c;

        val_c = c3_malloc(1 + hed_u[i].value_len);
        val_c[hed_u[i].value_len] = 0;
        memcpy(val_c, hed_u[i].value, hed_u[i].value_len);

        // 'truncate' by replacing port separator ':' with 0
        if ( 0 != (por_c = strchr(val_c, ':')) ) {
          por_c[0] = 0;
        }

        *hot_c = val_c;
        break;
      }
    }
  }

  return u3_pars_good;
}

/* _proxy_parse_sni(): parse clienthello buffer for SNI
*/
static u3_proxy_pars
_proxy_parse_sni(const uv_buf_t* buf_u, c3_c** hot_c)
{
  c3_i sas_i = parse_tls_header((const uint8_t*)buf_u->base,
                                buf_u->len, hot_c);

  if ( 0 > sas_i ) {
    switch ( sas_i ) {
      case -1: return u3_pars_moar;
      case -2: return u3_pars_good;  // SNI not present
      default: return u3_pars_fail;
    }
  }

  return u3_pars_good;
}

/* _proxy_parse_ship(): determine destination for proxied request
*/
static u3_noun
_proxy_parse_ship(c3_c* hot_c)
{
  u3_noun sip = u3_nul;
  c3_c* dom_c;

  if ( 0 == hot_c ) {
    return sip;
  }

  dom_c = strchr(hot_c, '.');

  if ( 0 == dom_c ) {
    return sip;
  }

  c3_w dif_w = dom_c - hot_c;
  c3_w dns_w = strlen(u3_Host.ops_u.dns_c);

  if ( (dns_w != strlen(hot_c) - (dif_w + 1)) ||
       (0 != strncmp(dom_c + 1, u3_Host.ops_u.dns_c, dns_w)) ) {
    return sip;
  }

  {
    c3_c* sip_c = c3_malloc(2 + dif_w);
    strncpy(sip_c + 1, hot_c, dif_w);
    sip_c[0] = '~';
    sip_c[1 + dif_w] = 0;

    sip = u3dc("slaw", 'p', u3i_string(sip_c));
    free(sip_c);

    return sip;
  }
}

/* _proxy_dest(): proxy to destination
*/
static void
_proxy_dest(u3_pcon* con_u, u3_noun sip)
{
  if ( u3_nul == sip ) {
    _proxy_loop_connect(con_u);
  }
  else {
    u3_noun hip = u3k(u3t(sip));
    u3_noun own = u3A->own;
    c3_o our = c3n;

    while ( u3_nul != own ) {
      if ( c3y == u3r_sing(hip, u3h(own)) ) {
        our = c3y;
        break;
      }
      own = u3t(own);
    }

    if ( c3y == our ) {
      _proxy_loop_connect(con_u);
    }
    else {
      // XX check if (sein:title sip) == our
      // XX check will
      _proxy_ward_start(con_u, hip);
    }
  }

  u3z(sip);
}

static void _proxy_peek_read(u3_pcon* con_u);

/* _proxy_peek(): peek at proxied request for destination
*/
static void
_proxy_peek(u3_pcon* con_u)
{
  c3_c* hot_c = 0;

  u3_proxy_pars sat_e = ( c3y == con_u->sec ) ?
                        _proxy_parse_sni(&con_u->buf_u, &hot_c) :
                        _proxy_parse_host(&con_u->buf_u, &hot_c);

  switch ( sat_e ) {
    default: c3_assert(0);

    case u3_pars_fail: {
      uL(fprintf(uH, "proxy: peek fail\n"));
      _proxy_conn_close(con_u);
      break;
    }

    case u3_pars_moar: {
      uL(fprintf(uH, "proxy: peek moar\n"));
      // XX count retries, fail after some n
      _proxy_peek_read(con_u);
      break;
    }

    case u3_pars_good: {
      u3_noun sip = _proxy_parse_ship(hot_c);
      _proxy_dest(con_u, sip);
      break;
    }
  }

  if ( 0 !=  hot_c ) {
    free(hot_c);
  }
}

/* _proxy_peek_read_cb(): read callback for peeking at proxied request
*/
static void
_proxy_peek_read_cb(uv_stream_t* don_u,
                    ssize_t      siz_w,
                    const uv_buf_t* buf_u)
{
  u3_pcon* con_u = don_u->data;

  if ( 0 > siz_w ) {
    if ( UV_EOF != siz_w ) {
      uL(fprintf(uH, "proxy: peek: %s\n", uv_strerror(siz_w)));
    }
    _proxy_conn_close(con_u);
  }
  else {
    uv_read_stop(don_u);

    u3_lo_open();

    if ( 0 == con_u->buf_u.base ) {
      con_u->buf_u = uv_buf_init(buf_u->base, siz_w);
    }
    else {
      c3_w len_w = siz_w + con_u->buf_u.len;
      // XX c3_realloc
      void* ptr_v = realloc(con_u->buf_u.base, len_w);
      c3_assert( 0 != ptr_v );

      memcpy(ptr_v + con_u->buf_u.len, buf_u->base, siz_w);
      con_u->buf_u = uv_buf_init(ptr_v, len_w);

      free(buf_u->base);
    }

    _proxy_peek(con_u);

    u3_lo_shut(c3y);
  }
}

/* _proxy_peek_read(): start read  to peek at proxied request
*/
static void
_proxy_peek_read(u3_pcon* con_u)
{
  uv_read_start((uv_stream_t*)&con_u->don_u,
                _proxy_alloc, _proxy_peek_read_cb);
}

/* _proxy_serv_free(): free proxy listener
*/
static void
_proxy_serv_free(u3_prox* lis_u)
{
  u3_pcon* con_u = lis_u->con_u;

  while ( con_u ) {
    _proxy_conn_close(con_u);
    con_u = con_u->nex_u;
  }

  u3_ward* rev_u = lis_u->rev_u;

  while ( rev_u ) {
    _proxy_ward_close(rev_u);
    rev_u = rev_u->nex_u;
  }

  // not unlinked here, owned directly by htp_u

  free(lis_u);
}

/* _proxy_serv_close(): close proxy listener
*/
static void
_proxy_serv_close(u3_prox* lis_u)
{
  uv_close((uv_handle_t*)&lis_u->sev_u, (uv_close_cb)_proxy_serv_free);
}

/* _proxy_serv_new(): allocate proxy listener
*/
static u3_prox*
_proxy_serv_new(u3_http* htp_u, c3_s por_s, c3_o sec)
{
  u3_prox* lis_u = c3_malloc(sizeof(*lis_u));
  lis_u->sev_u.data = lis_u;
  lis_u->por_s = por_s;
  lis_u->sec = sec;
  lis_u->htp_u = htp_u;
  lis_u->con_u = 0;
  lis_u->rev_u = 0;

  // not linked here, owned directly by htp_u

  return lis_u;
}

/* _proxy_serv_accept(): accept new connection.
*/
static void
_proxy_serv_accept(u3_prox* lis_u)
{
  u3_pcon* con_u = _proxy_conn_new(u3_ptyp_prox, lis_u);

  uv_tcp_init(u3L, &con_u->don_u);

  c3_i sas_i;
  if ( 0 != (sas_i = uv_accept((uv_stream_t*)&lis_u->sev_u,
                               (uv_stream_t*)&con_u->don_u)) ) {
    uL(fprintf(uH, "proxy: accept: %s\n", uv_strerror(sas_i)));
    _proxy_conn_close(con_u);
  }
  else {
    _proxy_peek_read(con_u);
  }
}

/* _proxy_serv_listen_cb(): listen callback for proxy server.
*/
static void
_proxy_serv_listen_cb(uv_stream_t* sev_u, c3_i sas_i)
{
  u3_prox* lis_u = (u3_prox*)sev_u;

  if ( 0 != sas_i ) {
    uL(fprintf(uH, "proxy: listen_cb: %s\n", uv_strerror(sas_i)));
  }
  else {
    _proxy_serv_accept(lis_u);
  }
}

/* _proxy_serv_start(): start reverse TCP proxy server.
*/
static u3_prox*
_proxy_serv_start(u3_prox* lis_u)
{
  uv_tcp_init(u3L, &lis_u->sev_u);

  struct sockaddr_in add_u;

  memset(&add_u, 0, sizeof(add_u));
  add_u.sin_family = AF_INET;
  add_u.sin_addr.s_addr = INADDR_ANY;

  /*  Try ascending ports.
  */
  while ( 1 ) {
    c3_i sas_i;

    add_u.sin_port = htons(lis_u->por_s);

    if ( 0 != (sas_i = uv_tcp_bind(&lis_u->sev_u,
                                   (const struct sockaddr*)&add_u, 0)) ||
         0 != (sas_i = uv_listen((uv_stream_t*)&lis_u->sev_u,
                                 TCP_BACKLOG, _proxy_serv_listen_cb)) ) {
      if ( (UV_EADDRINUSE == sas_i) || (UV_EACCES == sas_i) ) {
        if ( (c3y == lis_u->sec) && (443 == lis_u->por_s) ) {
          lis_u->por_s = 9443;
        }
        else if ( (c3n == lis_u->sec) && (80 == lis_u->por_s) ) {
          lis_u->por_s = 9080;
        }
        else {
          lis_u->por_s++;
        }

        continue;
      }

      uL(fprintf(uH, "proxy: listen: %s\n", uv_strerror(sas_i)));
      _proxy_serv_free(lis_u);
      return 0;
    }

    return lis_u;
  }
}

/* u3_http_ef_that(): reverse proxy requested connection notification.
*/
void
u3_http_ef_that(u3_noun sip, u3_noun por, u3_noun sec)
{
  if( c3n == u3ud(sip) ||
      c3n == u3a_is_cat(por) ||
      !( c3y == sec || c3n == sec ) ) {
    uL(fprintf(uH, "http: that: invalid card\n"));
    u3z(sip); u3z(por); u3z(sec);
    return;
  }

  u3_http* htp_u;
  u3_warc* cli_u;

  for ( htp_u = u3_Host.htp_u; (0 != htp_u); htp_u = htp_u->nex_u ) {
    if ( c3n == htp_u->lop && sec == htp_u->sec ) {
      break;
    }
  }

  if ( 0 == htp_u ) {
    uL(fprintf(uH, "http: that: no %s server\n", (c3y == sec) ?
                                                 "secure" : "insecure"));
    u3z(sip); u3z(por); u3z(sec);
    return;
  }

  cli_u = _proxy_warc_new(htp_u, (u3_atom)sip, (c3_s)por, (c3_o)sec);

  if ( c3n == u3_Host.ops_u.net ) {
    cli_u->ipf_w = INADDR_LOOPBACK;
    _proxy_ward_connect(cli_u);
    return;
  }

  _proxy_ward_resolve(cli_u);
}
