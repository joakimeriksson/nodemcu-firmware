// Module for LWM2M

#include "module.h"
#include "lauxlib.h"
#include "platform.h"

#include "c_string.h"
#include "c_stdlib.h"

#include "c_types.h"
#include "mem.h"
#include "lwip/ip_addr.h"
#include "espconn.h"
#include "driver/uart.h"
#include "node-endpoint.h"

static os_timer_t tick_timer;
long ticks;

#define MAX_MESSAGE_SIZE 1200

/* get debug info here */
#undef NODE_DBG
#define NODE_DBG dbg_printf

/* Needs to be defined here as contiki code use stdint and here c_types */
int ntimer_run(void);
void coap_endpoint_copy(coap_endpoint_t *destination,
                        const coap_endpoint_t *from);
int coap_endpoint_cmp(const coap_endpoint_t *e1, const coap_endpoint_t *e2);
void coap_endpoint_print(const coap_endpoint_t *ep);
int coap_endpoint_parse(const char *text, size_t size, coap_endpoint_t *ep);
int coap_receive(const coap_endpoint_t *src,
                 uint8_t *payload, uint16_t payload_length);
void lwm2m_rd_client_register_with_server(coap_endpoint_t *server);
void lwm2m_rd_client_use_registration_server(int use);

/* Timer tick for ntimer */
static void ntimer_tick(void *arg)
{ 
  int i;
  for(i = 0; i < 5 && ntimer_run(); i++);
  ticks++;
}

/* The init function from the lwm2m app */

/* Need to move all endpoint code here... */

void lwm2m_app_init(void);

typedef struct llwm2m_userdata
{
  struct espconn *pesp_conn;
  int self_ref;
} llwm2m_userdata_t;


#define printf(...) ets_printf( __VA_ARGS__ )
/*------------------------------------------------------------------------*/
/* Functions implementing CoAP Endpoint
/*------------------------------------------------------------------------*/
#define BUFSIZE 1280

typedef union {
  uint32_t u32[(BUFSIZE + 3) / 4];
  uint8_t u8[BUFSIZE];
} coap_buf_t;

/* Currently supports only one instance of lwm2m client */
static llwm2m_userdata_t *conn; /* this has the connection also */
static coap_endpoint_t last_source;
static coap_buf_t coap_aligned_buf;
static uint16_t coap_buf_len;

static coap_endpoint_t reg_server;

void
coap_transport_init(void)
{
  /* Maybe this is for the module?? - test of printf! */
  printf("CoAP transport init!\n");
}

/*------------------------------------------------------------------------*/
static const coap_endpoint_t *
coap_src_endpoint(void)
{
  return &last_source;
}

/*------------------------------------------------------------------------*/

int
coap_endpoint_cmp(const coap_endpoint_t *e1, const coap_endpoint_t *e2)
{
  return memcmp(e1, e2, sizeof(coap_endpoint_t)) == 0;
}

/*------------------------------------------------------------------------*/

void
coap_endpoint_copy(coap_endpoint_t *destination, const coap_endpoint_t *from)
{
  memcpy(destination, from, sizeof(coap_endpoint_t));
}

/*------------------------------------------------------------------------*/

void
coap_endpoint_print(const coap_endpoint_t *ep)
{
  ip_addr_t *ipaddr;
  ipaddr = (ip_addr_t *)&ep->ipaddr;
  printf("%d.%d.%d.%d:%u",
	 ip4_addr1(ipaddr),
	 ip4_addr2(ipaddr),
	 ip4_addr3(ipaddr),
	 ip4_addr4(ipaddr),
	 ntohs(ep->port));
}

/*------------------------------------------------------------------------*/
/* Used for parsing URI:s */
int
coap_endpoint_parse(const char *text, size_t size, coap_endpoint_t *ep)
{
  return 0;
}

/*------------------------------------------------------------------------*/

uint8_t *
coap_databuf(void)
{
  return coap_aligned_buf.u8;
}

/*------------------------------------------------------------------------*/

uint16_t
coap_datalen()
{
  return coap_buf_len;
}

/*------------------------------------------------------------------------*/

void
coap_send_message(const coap_endpoint_t *ep, const uint8_t *data, uint16_t len)
{
  NODE_DBG("Send message: %d to %d.%d.%d.%d\n", len,
	   ep->ipaddr[0], ep->ipaddr[1], ep->ipaddr[2], ep->ipaddr[3]);
  if(conn != NULL) {
    conn->pesp_conn->proto.udp->remote_port = ep->port;
    os_memmove(conn->pesp_conn->proto.udp->remote_ip, ep->ipaddr, 4);
    espconn_send(conn->pesp_conn, (unsigned char *)data, len);
  }
}

/*------------------------------------------------------------------------*/

static void data_received(void *arg, char *pdata, unsigned short len)
{
  NODE_DBG("data_received is called. %d bytes.\n", len);
  struct espconn *pesp_conn = arg;
  llwm2m_userdata_t *cud = (llwm2m_userdata_t *)pesp_conn->reverse;

  if (len > MAX_MESSAGE_SIZE) {
    NODE_DBG("Request Entity Too Large.\n"); // NOTE: should response 4.13 to client...
    return;
  }

  size_t rsplen = 0;//coap_server_respond(pdata, len, buf, MAX_MESSAGE_SIZE+1);
  // SDK 1.4.0 changed behaviour, for UDP server need to look up remote ip/port
  remot_info *pr = 0;
  if (espconn_get_connection_info(pesp_conn, &pr, 0) != ESPCONN_OK)
    return;

  /* update last source - probably this should be a local var */
  last_source.port = pr->remote_port;
  os_memmove(last_source.ipaddr, pr->remote_ip, 4);
  coap_receive(&last_source, pdata, len);
}

static void data_sent(void *arg)
{
  NODE_DBG("data_sent is called.\n");
}

// Create the LWM2M client object (which is a CoAP server)
// Lua: s = lwm2m.create(function(conn))
static int lwm2m_create( lua_State* L, const char* mt )
{
  struct espconn *pesp_conn = NULL;
  llwm2m_userdata_t *cud;
  unsigned type;
  int stack = 1;

  // create a object
  cud = (llwm2m_userdata_t *)lua_newuserdata(L, sizeof(llwm2m_userdata_t));
  // pre-initialize it, in case of errors
  cud->self_ref = LUA_NOREF;
  cud->pesp_conn = NULL;

  // set its metatable
  luaL_getmetatable(L, mt);
  lua_setmetatable(L, -2);

  // create the espconn struct
  pesp_conn = (struct espconn *)c_zalloc(sizeof(struct espconn));
  if(!pesp_conn)
    return luaL_error(L, "not enough memory");

  cud->pesp_conn = pesp_conn;

  pesp_conn->type = ESPCONN_UDP;
  pesp_conn->proto.tcp = NULL;
  pesp_conn->proto.udp = NULL;

  pesp_conn->proto.udp = (esp_udp *)c_zalloc(sizeof(esp_udp));
  if(!pesp_conn->proto.udp){
    c_free(pesp_conn);
    cud->pesp_conn = pesp_conn = NULL;
    return luaL_error(L, "not enough memory");
  }
  pesp_conn->state = ESPCONN_NONE;
  NODE_DBG("UDP server/client is set.\n");

  pesp_conn->reverse = cud;

  /* UGLY hack to get the user data into single static (last) for send */
  conn = cud;

  NODE_DBG("lwm2m_create is called.\n");
  return 1;  
}

// Lua: client:delete()
static int lwm2m_delete_c( lua_State* L, const char* mt )
{
  struct espconn *pesp_conn = NULL;
  llwm2m_userdata_t *cud;

  cud = (llwm2m_userdata_t *)luaL_checkudata(L, 1, mt);
  luaL_argcheck(L, cud, 1, "Server/Client expected");
  if(cud==NULL){
    NODE_DBG("userdata is nil.\n");
    return 0;
  }

  // free (unref) callback ref
  if(LUA_NOREF!=cud->self_ref){
    luaL_unref(L, LUA_REGISTRYINDEX, cud->self_ref);
    cud->self_ref = LUA_NOREF;
  }

  if(cud->pesp_conn)
  {
    if(cud->pesp_conn->proto.udp->remote_port || cud->pesp_conn->proto.udp->local_port)
      espconn_delete(cud->pesp_conn);
    c_free(cud->pesp_conn->proto.udp);
    cud->pesp_conn->proto.udp = NULL;
    c_free(cud->pesp_conn);
    cud->pesp_conn = NULL;
  }

  NODE_DBG("lwm2m_delete is called.\n");
  return 0;  
}

// Lua: server:listen( port, ip )
static int lwm2m_listen_c( lua_State* L, const char* mt )
{
  struct espconn *pesp_conn = NULL;
  llwm2m_userdata_t *cud;
  unsigned port;
  size_t il;
  ip_addr_t ipaddr;

  cud = (llwm2m_userdata_t *)luaL_checkudata(L, 1, mt);
  luaL_argcheck(L, cud, 1, "Server/Client expected");
  if(cud==NULL){
    NODE_DBG("userdata is nil.\n");
    return 0;
  }

  pesp_conn = cud->pesp_conn;
  port = luaL_checkinteger( L, 2 );
  pesp_conn->proto.udp->local_port = port;
  NODE_DBG("UDP port is set: %d.\n", port);

  if( lua_isstring(L,3) )   // deal with the ip string
  {
    const char *ip = luaL_checklstring( L, 3, &il );
    if (ip == NULL)
    {
      ip = "0.0.0.0";
    }
    ipaddr.addr = ipaddr_addr(ip);
    c_memcpy(pesp_conn->proto.udp->local_ip, &ipaddr.addr, 4);
    NODE_DBG("UDP ip is set: ");
    NODE_DBG(IPSTR, IP2STR(&ipaddr.addr));
    NODE_DBG("\n");
  }

  if(LUA_NOREF==cud->self_ref){
    lua_pushvalue(L, 1);  // copy to the top of stack
    cud->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  espconn_regist_recvcb(pesp_conn, data_received);
  espconn_regist_sentcb(pesp_conn, data_sent);
  espconn_create(pesp_conn);

  NODE_DBG("LWM2M UDP Server started on port: %d\n", port);
  NODE_DBG("lwm2m_start is called.\n");
  return 0;  
}

// Lua: regiester( port, ip )
static int lwm2m_register_c( lua_State* L, const char* mt )
{
  struct espconn *pesp_conn = NULL;
  llwm2m_userdata_t *cud;
  unsigned port;
  size_t il;
  ip_addr_t ipaddr;

  cud = (llwm2m_userdata_t *)luaL_checkudata(L, 1, mt);
  luaL_argcheck(L, cud, 1, "Server/Client expected");
  if(cud==NULL){
    NODE_DBG("userdata is nil.\n");
    return 0;
  }

  port = luaL_checkinteger( L, 2 );
  reg_server.port = port;
  NODE_DBG("LWM2M Server UDP port is set: %d.\n", port);

  if( lua_isstring(L,3) )   // deal with the ip string
  {
    const char *ip = luaL_checklstring( L, 3, &il );
    if (ip == NULL)
    {
      ip = "0.0.0.0";
    }
    ipaddr.addr = ipaddr_addr(ip);
    c_memcpy(reg_server.ipaddr, &ipaddr.addr, 4);
    NODE_DBG("LWM2M Server UDP ip is set: ");
    NODE_DBG(IPSTR, IP2STR(&ipaddr.addr));
    NODE_DBG("\n");
  }

  if(LUA_NOREF==cud->self_ref){
    lua_pushvalue(L, 1);  // copy to the top of stack
    cud->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  lwm2m_rd_client_use_registration_server(1);
  lwm2m_rd_client_register_with_server(&reg_server);

  return 0;  
}


// Lua: client:close()
static int lwm2m_close_c( lua_State* L, const char* mt )
{
  struct espconn *pesp_conn = NULL;
  llwm2m_userdata_t *cud;

  cud = (llwm2m_userdata_t *)luaL_checkudata(L, 1, mt);
  luaL_argcheck(L, cud, 1, "Server/Client expected");
  if(cud==NULL){
    NODE_DBG("userdata is nil.\n");
    return 0;
  }

  if(cud->pesp_conn)
  {
    if(cud->pesp_conn->proto.udp->remote_port || cud->pesp_conn->proto.udp->local_port)
      espconn_delete(cud->pesp_conn);
  }

  if(LUA_NOREF!=cud->self_ref){
    luaL_unref(L, LUA_REGISTRYINDEX, cud->self_ref);
    cud->self_ref = LUA_NOREF;
  }

  NODE_DBG("lwm2m_close is called.\n");
  return 0;  
}


//extern coap_luser_entry *variable_entry;
//extern coap_luser_entry *function_entry;

// Lua: client:listen( port, ip, function(err) )

static int lwm2m_listen( lua_State* L )
{
  const char *mt = "lwm2m_client";
  return lwm2m_listen_c(L, mt);
}

static int lwm2m_register( lua_State* L )
{
  const char *mt = "lwm2m_client";
  return lwm2m_register_c(L, mt);
}

static int lwm2m_close( lua_State* L )
{
  const char *mt = "lwm2m_client";
  return lwm2m_close_c(L, mt);
}

// Lua: s = coap.createClient(function(conn))
static int lwm2m_createClient( lua_State* L )
{
  const char *mt = "lwm2m_client";
  return lwm2m_create(L, mt);
}

static int lwm2m_delete( lua_State* L )
{
  const char *mt = "lwm2m_client";
  return lwm2m_delete_c(L, mt);
}


// Module function map
static const LUA_REG_TYPE lwm2m_obj_map[] = {
  { LSTRKEY( "listen" ),  LFUNCVAL( lwm2m_listen ) },
  { LSTRKEY( "register" ),  LFUNCVAL( lwm2m_register ) },
  { LSTRKEY( "close" ),   LFUNCVAL( lwm2m_close ) },
  //  { LSTRKEY( "var" ),     LFUNCVAL( coap_server_var ) },
  // { LSTRKEY( "func" ),    LFUNCVAL( coap_server_func ) },
  { LSTRKEY( "__gc" ),    LFUNCVAL( lwm2m_delete ) },
  { LSTRKEY( "__index" ), LROVAL( lwm2m_obj_map ) },
  { LNILKEY, LNILVAL }
};

static const LUA_REG_TYPE lwm2m_map[] = 
{
  { LSTRKEY( "Client" ),      LFUNCVAL( lwm2m_createClient ) },
  { LSTRKEY( "__metatable" ), LROVAL( lwm2m_map ) },
  { LNILKEY, LNILVAL }
};

static int luaopen_lwm2m( lua_State *L )
{
  //  endpoint_setup();
  luaL_rometatable(L, "lwm2m_client", (void *)lwm2m_obj_map);  // create metatable for lwm2m_client
  lwm2m_app_init();

  /* set up a ntimer tick timer that tick each 100 ms */
  os_timer_setfn(&tick_timer, (os_timer_func_t *)ntimer_tick, NULL);
  os_timer_arm(&tick_timer, 100, 1);
  
  return 0;
}

NODEMCU_MODULE(LWM2M, "lwm2m", lwm2m_map, luaopen_lwm2m);
