#ifndef NODE_ENDPOINT_H_
#define NODE_ENDPOINT_H_

/* Need to define the coap-endpoint - where should we do that? */
typedef struct {
  int port;
  unsigned char ipaddr[4];
} coap_endpoint_t;




#endif /* NODE_ENDPOINT_H_ */
