/*
 * Copyright (c) 2015, Yanzi Networks AB.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \addtogroup oma-lwm2m
 * @{
 */

/**
 * \file
 *         Implementation of the Contiki OMA LWM2M engine
 * \author
 *         Joakim Eriksson <joakime@sics.se>
 *         Niclas Finne <nfi@sics.se>
 */

#include "lwm2m-engine.h"
#include "lwm2m-object.h"
#include "lwm2m-device.h"
#include "lwm2m-plain-text.h"
#include "lwm2m-json.h"
#include "er-coap-constants.h"
#include "er-coap-engine.h"
#include "oma-tlv.h"
#include "oma-tlv-reader.h"
#include "oma-tlv-writer.h"
#include "lib/list.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "lib/mini-snprintf.h"

#if UIP_CONF_IPV6_RPL
#include "net/rpl/rpl.h"
#include "net/ipv6/uip-ds6.h"
#endif /* UIP_CONF_IPV6_RPL */

#define DEBUG 1
#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#define PRINTS(l,s,f) do { int i;					\
    for(i = 0; i < l; i++) printf(f, s[i]); \
    } while(0)
#else
#define PRINTF(...)
#define PRINTS(l,s,f)
#endif

#ifndef LWM2M_ENGINE_CLIENT_ENDPOINT_PREFIX
#ifdef LWM2M_DEVICE_MODEL_NUMBER
#define LWM2M_ENGINE_CLIENT_ENDPOINT_PREFIX LWM2M_DEVICE_MODEL_NUMBER
#else /* LWM2M_DEVICE_MODEL_NUMBER */
#define LWM2M_ENGINE_CLIENT_ENDPOINT_PREFIX "Contiki-"
#endif /* LWM2M_DEVICE_MODEL_NUMBER */
#endif /* LWM2M_ENGINE_CLIENT_ENDPOINT_PREFIX */

#ifdef LWM2M_ENGINE_CONF_MAX_OBJECTS
#define MAX_OBJECTS LWM2M_ENGINE_CONF_MAX_OBJECTS
#else /* LWM2M_ENGINE_CONF_MAX_OBJECTS */
#define MAX_OBJECTS 10
#endif /* LWM2M_ENGINE_CONF_MAX_OBJECTS */

#ifdef LWM2M_ENGINE_CONF_USE_RD_CLIENT
#define USE_RD_CLIENT LWM2M_ENGINE_CONF_USE_RD_CLIENT
#else
#define USE_RD_CLIENT 1
#endif

void lwm2m_device_init(void);
void lwm2m_security_init(void);
void lwm2m_server_init(void);

static int lwm2m_handler_callback(coap_packet_t *request,
                                  coap_packet_t *response,
                                  uint8_t *buffer, uint16_t buffer_size,
                                  int32_t *offset);
static lwm2m_object_instance_t *
lwm2m_engine_next_object_instance(const lwm2m_context_t *context, lwm2m_object_instance_t *last);


COAP_HANDLER(lwm2m_handler, lwm2m_handler_callback);
LIST(object_list);

static const lwm2m_object_t *objects[MAX_OBJECTS];
static char endpoint[32];
/*---------------------------------------------------------------------------*/
int
u16toa(uint8_t *buf, uint16_t v)
{
  int pos = 0;
  int div = 10000;
  /* Max size = 5 */
  while(div > 0) {
    buf[pos] = '0' + (v / div) % 10;
    /* if first non-zero found or we have found that before */
    if(buf[pos] > '0' || pos > 0 || div == 1) pos++;
    div = div / 10;
  }
  return pos;
}

int
append_reg_tag(uint8_t *rd_data, int oid, int iid, int rid)
{
  int pos;
  rd_data[pos++] = '<';
  pos += u16toa(&rd_data[pos], oid);
  if(iid > -1) {
    rd_data[pos++] = '/';
    pos += u16toa(&rd_data[pos], iid);
    if(rid > -1) {
      rd_data[pos++] = '/';
      pos += u16toa(&rd_data[pos], rid);
    }
  }
  rd_data[pos++] = '>';
  return pos;
}
/*---------------------------------------------------------------------------*/
static inline const char *
get_method_as_string(rest_resource_flags_t method)
{
  if(method == METHOD_GET) {
    return "GET";
  } else if(method == METHOD_POST) {
    return "POST";
  } else if(method == METHOD_PUT) {
    return "PUT";
  } else if(method == METHOD_DELETE) {
    return "DELETE";
  } else {
    return "UNKNOWN";
  }
}
/*--------------------------------------------------------------------------*/
static int
lwm2m_engine_parse_context(const char *path, int path_len,
                           coap_packet_t *request, coap_packet_t *response,
                           uint8_t *outbuf, size_t outsize,
                           lwm2m_context_t *context)
{
  int len;
  int ret;
  int pos;
  uint16_t val;
  char c;
  if(context == NULL || path == NULL) {
    return 0;
  }

  memset(context, 0, sizeof(lwm2m_context_t));

  /* Set CoAP request/response for now */
  context->request = request;
  context->response = response;

  /* Set out buffer */
  context->outbuf = outbuf;
  context->outsize = outsize;

  /* Set default reader/writer */
  context->reader = &lwm2m_plain_text_reader;
  context->writer = &oma_tlv_writer;

  /* get object id */
  PRINTF("Parse PATH:");
  PRINTS(path_len, path, "%c");
  PRINTF("\n");

  ret = 0;
  pos = 0;
  do {
    val = 0;
    /* we should get a value first - consume all numbers */
    while(pos < path_len && (c = path[pos]) >= '0' && c <= '9') {
      val = val * 10 + (c - '0');
      pos++;
    }
    /* Slash will mote thing forward - and the end will be when pos == pl */
    if(c == '/' || pos == path_len) {
      if(ret == 0) context->object_id = val;
      if(ret == 1) context->object_instance_id = val;
      if(ret == 2) context->resource_id = val;
      ret++;
      pos++;
    } else {
      PRINTF("Error: illegal char '%c' at pos:%d\n", c, pos);
      return -1;
    }
  } while(pos < path_len);
  
  //  ret += parse_next(&path[pos], path_len, &context->object_instance_id);
  //  ret += parse_next(&path[pos], path_len, &context->resource_id);

  if(ret > 0) {
    context->level = ret;
  }
  return ret;
}
/*---------------------------------------------------------------------------*/
int
lwm2m_engine_get_rd_data(uint8_t *rd_data, int size) {
  lwm2m_object_instance_t *o;
  int pos;
  int len, i, j;

  pos = 0;
  for(o = list_head(object_list); o != NULL; o = o->next) {
    if(pos > 0) {
      rd_data[pos++] = ',';
    }
    len = append_reg_tag(&rd_data[pos], o->object_id, o->instance_id, -1);
    if(len > 0 && len < size - pos) {
      pos += len;
    }
  }

  for(i = 0; i < MAX_OBJECTS; i++) {
    if(objects[i] != NULL) {
      for(j = 0; j < objects[i]->count; j++) {
        if(objects[i]->instances[j].flag & LWM2M_INSTANCE_FLAG_USED) {
	  uint16_t oid = objects[i]->id;
	  uint16_t iid = objects[i]->instances[j].id;
	  if(size - pos > 6) { /* Should be better guess */
	    if(pos > 0) {
	      rd_data[pos++] = ',';
	    }
	    pos += append_reg_tag(&rd_data[pos], oid, iid, -1);
	  }
        }
      }
    }
  }
  rd_data[pos] = 0;
  return pos;
}
/*---------------------------------------------------------------------------*/
void
lwm2m_engine_init(void)
{
  list_init(object_list);
#ifdef LWM2M_ENGINE_CLIENT_ENDPOINT_NAME

  snprintf(endpoint, sizeof(endpoint) - 1,
           "?ep=" LWM2M_ENGINE_CLIENT_ENDPOINT_NAME);

#else /* LWM2M_ENGINE_CLIENT_ENDPOINT_NAME */

  int len, i;
  uint8_t state;
  uip_ipaddr_t *ipaddr;
  char client[sizeof(endpoint)];

  len = strlen(LWM2M_ENGINE_CLIENT_ENDPOINT_PREFIX);
  /* ensure that this fits with the hex-nums */
  if(len > sizeof(client) - 13) {
    len = sizeof(client) - 13;
  }
  memcpy(client, LWM2M_ENGINE_CLIENT_ENDPOINT_PREFIX, len);

  /* pick an IP address that is PREFERRED or TENTATIVE */
  ipaddr = NULL;
  for(i = 0; i < UIP_DS6_ADDR_NB; i++) {
    state = uip_ds6_if.addr_list[i].state;
    if(uip_ds6_if.addr_list[i].isused &&
       (state == ADDR_TENTATIVE || state == ADDR_PREFERRED)) {
      ipaddr = &(uip_ds6_if.addr_list[i]).ipaddr;
      break;
    }
  }

  if(ipaddr != NULL) {
    for(i = 0; i < 6; i++) {
      /* assume IPv6 for now */
      uint8_t b = ipaddr->u8[10 + i];
      client[len++] = (b >> 4) > 9 ? 'A' - 10 + (b >> 4) : '0' + (b >> 4);
      client[len++] = (b & 0xf) > 9 ? 'A' - 10 + (b & 0xf) : '0' + (b & 0xf);
    }
  }

  /* a zero at end of string */
  client[len] = 0;
  /* create endpoint */
  snprintf(endpoint, sizeof(endpoint) - 1, "?ep=%s", client);

#endif /* LWM2M_ENGINE_CLIENT_ENDPOINT_NAME */

  rest_init_engine();

  /* Register the CoAP handler for lightweight object handling */
  coap_add_handler(&lwm2m_handler);

#if USE_RD_CLIENT
  lwm2m_rd_client_init(endpoint);
#endif
}
/*---------------------------------------------------------------------------*/
void
lwm2m_engine_register_default_objects(void)
{
  lwm2m_security_init();
  lwm2m_server_init();
  lwm2m_device_init();
}
/*---------------------------------------------------------------------------*/
const lwm2m_object_t *
lwm2m_engine_get_object(uint16_t id)
{
  int i;
  for(i = 0; i < MAX_OBJECTS; i++) {
    if(objects[i] != NULL && objects[i]->id == id) {
      return objects[i];
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
int
lwm2m_engine_register_object(const lwm2m_object_t *object)
{
  int i;
  int found = 0;
  for(i = 0; i < MAX_OBJECTS; i++) {
    if(objects[i] == NULL) {
      objects[i] = object;
      found = 1;
      break;
    }
  }
  rest_activate_resource(lwm2m_object_get_coap_resource(object),
                         (char *)object->path);
  return found;
}
/*---------------------------------------------------------------------------*/
const lwm2m_instance_t *
lwm2m_engine_get_first_instance_of_object(uint16_t id, lwm2m_context_t *context)
{
  const lwm2m_object_t *object;
  int i;

  object = lwm2m_engine_get_object(id);
  if(object == NULL) {
    /* No object with the specified id found */
    return NULL;
  }

  /* Initialize the context */
  memset(context, 0, sizeof(lwm2m_context_t));
  context->object_id = id;

  for(i = 0; i < object->count; i++) {
    if(object->instances[i].flag & LWM2M_INSTANCE_FLAG_USED) {
      context->object_instance_id = object->instances[i].id;
      context->object_instance_index = i;
      return &object->instances[i];
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
const lwm2m_instance_t *
lwm2m_engine_get_instance(const lwm2m_object_t *object, lwm2m_context_t *context, int depth)
{
  int i;
  if(depth > 1) {
    PRINTF("lwm2m: searching for instance %u\n", context->object_instance_id);
    for(i = 0; i < object->count; i++) {
      PRINTF("  Instance %d -> %u (used: %d)\n", i, object->instances[i].id,
             (object->instances[i].flag & LWM2M_INSTANCE_FLAG_USED) != 0);
      if(object->instances[i].id == context->object_instance_id &&
         object->instances[i].flag & LWM2M_INSTANCE_FLAG_USED) {
        context->object_instance_index = i;
        return &object->instances[i];
      }
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
const lwm2m_resource_t *
lwm2m_get_resource(const lwm2m_instance_t *instance, lwm2m_context_t *context)
{
  int i;
  if(instance != NULL) {
    PRINTF("lwm2m: searching for resource %u\n", context->resource_id);
    for(i = 0; i < instance->count; i++) {
      PRINTF("  Resource %d -> %u\n", i, instance->resources[i].id);
      if(instance->resources[i].id == context->resource_id) {
        context->resource_index = i;
        return &instance->resources[i];
      }
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
/**
 * @brief Write a list of object instances as a CoRE Link-format list
 */
static int
write_object_instances_link(const lwm2m_object_t *object,
                            char *buffer, size_t size)
{
  const lwm2m_instance_t *instance;
  int len, rdlen, i;

  PRINTF("</%d>", object->id);
  rdlen = snprintf(buffer, size, "</%d>",
                   object->id);
  if(rdlen < 0 || rdlen >= size) {
    return -1;
  }

  for(i = 0; i < object->count; i++) {
    if((object->instances[i].flag & LWM2M_INSTANCE_FLAG_USED) == 0) {
      continue;
    }
    instance = &object->instances[i];
    PRINTF(",</%d/%d>", object->id, instance->id);

    len = snprintf(&buffer[rdlen], size - rdlen,
                   ",<%d/%d>", object->id, instance->id);
    rdlen += len;
    if(len < 0 || rdlen >= size) {
      return -1;
    }
  }
  return rdlen;
}
/*---------------------------------------------------------------------------*/
static int
write_link_format_data(const lwm2m_object_t *object,
                       const lwm2m_instance_t *instance,
                       char *buffer, size_t size)
{
  const lwm2m_resource_t *resource;
  int len, rdlen, i;

  PRINTF("<%d/%d>", object->id, instance->id);
  rdlen = snprintf(buffer, size, "<%d/%d>",
                   object->id, instance->id);
  if(rdlen < 0 || rdlen >= size) {
    return -1;
  }

  for(i = 0; i < instance->count; i++) {
    resource = &instance->resources[i];
    PRINTF(",<%d/%d/%d>", object->id, instance->id, resource->id);

    len = snprintf(&buffer[rdlen], size - rdlen,
                   ",<%d/%d/%d>", object->id, instance->id, resource->id);
    rdlen += len;
    if(len < 0 || rdlen >= size) {
      return -1;
    }
  }
  return rdlen;
}
/*---------------------------------------------------------------------------*/
static int
write_json_data(const lwm2m_context_t *context,
                const lwm2m_object_t *object,
                const lwm2m_instance_t *instance,
                char *buffer, size_t size)
{
  const lwm2m_resource_t *resource;
  const char *s = "";
  int len, rdlen, i;

  PRINTF("{\"e\":[");
  rdlen = snprintf(buffer, size, "{\"e\":[");
  if(rdlen < 0 || rdlen >= size) {
    PRINTF("#<truncated>\n");
    return -1;
  }

  for(i = 0, len = 0; i < instance->count; i++) {
    resource = &instance->resources[i];
    len = 0;
    if(lwm2m_object_is_resource_string(resource)) {
      const uint8_t *value;
      uint16_t slen;
      value = lwm2m_object_get_resource_string(resource, context);
      slen = lwm2m_object_get_resource_strlen(resource, context);
      if(value != NULL) {
        PRINTF("%s{\"n\":\"%u\",\"sv\":\"", s, resource->id);
	PRINTS(slen, value, "%c");
	PRINTF("\"}", size, rdlen);
        len = snprintf(&buffer[rdlen], size - rdlen,
                       "%s{\"n\":\"%u\",\"sv\":\"", s, resource->id);
	memcpy(&buffer[rdlen + len], value, slen);
	len = len + slen;
	memcpy(&buffer[rdlen + len], "\"}", 2);
	len = len + 2;
      }
    } else if(lwm2m_object_is_resource_int(resource)) {
      int32_t value;
      if(lwm2m_object_get_resource_int(resource, context, &value)) {
        PRINTF("%s{\"n\":\"%u\",\"v\":%" PRId32 "}", s,
               resource->id, value);
        len = snprintf(&buffer[rdlen], size - rdlen,
                       "%s{\"n\":\"%u\",\"v\":%" PRId32 "}", s,
                       resource->id, value);
      }
    } else if(lwm2m_object_is_resource_floatfix(resource)) {
      int32_t value;
      if(lwm2m_object_get_resource_floatfix(resource, context, &value)) {
        PRINTF("%s{\"n\":\"%u\",\"v\":%" PRId32 "}", s, resource->id,
               value / (int32_t)LWM2M_FLOAT32_FRAC);
        len = snprintf(&buffer[rdlen], size - rdlen,
                       "%s{\"n\":\"%u\",\"v\":", s, resource->id);
        rdlen += len;
        if(len < 0 || rdlen >= size) {
          PRINTF("#<truncated>\n");
          return -1;
        }

        len = lwm2m_plain_text_write_float32fix((uint8_t *)&buffer[rdlen],
                                                size - rdlen,
                                                value, LWM2M_FLOAT32_BITS);
        if(len == 0) {
          PRINTF("#<truncated>\n");
          return -1;
        }
        rdlen += len;

        if(rdlen < size) {
          buffer[rdlen] = '}';
        }
        len = 1;
      }
    } else if(lwm2m_object_is_resource_boolean(resource)) {
      int value;
      if(lwm2m_object_get_resource_boolean(resource, context, &value)) {
        PRINTF("%s{\"n\":\"%u\",\"bv\":%s}", s, resource->id,
               value ? "true" : "false");
        len = snprintf(&buffer[rdlen], size - rdlen,
                       "%s{\"n\":\"%u\",\"bv\":%s}", s, resource->id,
                       value ? "true" : "false");
      }
    }
    rdlen += len;
    if(len < 0 || rdlen >= size) {
      PRINTF("#<truncated>\n");
      return -1;
    }
    if(rdlen > 0) {
      s = ",";
    }
  }
  PRINTF("]}\n");
  len = snprintf(&buffer[rdlen], size - rdlen, "]}");
  rdlen += len;
  if(len < 0 || rdlen >= size) {
    PRINTF("#<truncated>\n");
    return -1;
  }

  return rdlen;
}
/*---------------------------------------------------------------------------*/
/**
 * @brief  Set the writer pointer to the proper writer based on the Accept: header
 *
 * @param[in] context  LWM2M context to operate on
 * @param[in] accept   Accept type number from CoAP headers
 *
 * @return The content type of the response if the selected writer is used
 */
static unsigned int
lwm2m_engine_select_writer(lwm2m_context_t *context, unsigned int accept)
{
  switch(accept) {
    case LWM2M_TLV:
      context->writer = &oma_tlv_writer;
      break;
    case LWM2M_TEXT_PLAIN:
    case TEXT_PLAIN:
      context->writer = &lwm2m_plain_text_writer;
      break;
    case LWM2M_JSON:
    case APPLICATION_JSON:
      context->writer = &lwm2m_json_writer;
      break;
    default:
      PRINTF("Unknown Accept type %u, using LWM2M plain text\n", accept);
      context->writer = &lwm2m_plain_text_writer;
      /* Set the response type to plain text */
      accept = LWM2M_TEXT_PLAIN;
      break;
  }
  context->content_type = accept;
  return accept;
}
/*---------------------------------------------------------------------------*/
/**
 * @brief  Set the reader pointer to the proper reader based on the Content-format: header
 *
 * @param[in] context        LWM2M context to operate on
 * @param[in] content_format Content-type type number from CoAP headers
 */
static void
lwm2m_engine_select_reader(lwm2m_context_t *context, unsigned int content_format)
{
  switch(content_format) {
    case LWM2M_TLV:
      context->reader = &oma_tlv_reader;
      break;
    case LWM2M_TEXT_PLAIN:
    case TEXT_PLAIN:
      context->reader = &lwm2m_plain_text_reader;
      break;
    default:
      PRINTF("Unknown content type %u, using LWM2M plain text\n",
             content_format);
      context->reader = &lwm2m_plain_text_reader;
      break;
  }
}
/*---------------------------------------------------------------------------*/
void
lwm2m_engine_handler(const lwm2m_object_t *object,
                     void *request, void *response,
                     uint8_t *buffer, uint16_t preferred_size,
                     int32_t *offset)
{
  int len;
  const char *url;
  unsigned int format;
  unsigned int accept;
  int depth;
  lwm2m_context_t context;
  rest_resource_flags_t method;
  const lwm2m_instance_t *instance;

  method = REST.get_method_type(request);

  len = REST.get_url(request, &url);
  if(!REST.get_header_content_type(request, &format)) {
    PRINTF("No format given. Assume text plain...\n");
    format = LWM2M_TEXT_PLAIN;
  } else if(format == TEXT_PLAIN) {
    /* CoAP content format text plain - assume LWM2M text plain */
    format = LWM2M_TEXT_PLAIN;
  }
  if(!REST.get_header_accept(request, &accept)) {
    PRINTF("No Accept header, using same as Content-format...\n");
    accept = format;
  }

  depth = lwm2m_engine_parse_context(url, len, request, response,
                                     buffer, preferred_size,
                                     &context);

  PRINTF("Context: %u/%u/%u  found: %d\n", context.object_id,
         context.object_instance_id, context.resource_id, depth);

  /* Select reader and writer based on provided Content type and Accept headers */
  lwm2m_engine_select_reader(&context, format);
  lwm2m_engine_select_writer(&context, accept);

#if DEBUG
  /* for debugging */
  PRINTF("%s Called Path:%u/%u/%u Format:%d ID:%d bsize:%u\n",
         get_method_as_string(method), context.object_id,
         context.object_instance_id, context.resource_id,
	 format, object->id, preferred_size);
  if(format == LWM2M_TEXT_PLAIN) {
    /* a string */
    const uint8_t *data;
    int plen = REST.get_request_payload(request, &data);
    if(plen > 0) {
      PRINTF("Data: '");
      PRINTS(plen, data, "%c");
      PRINTF("'\n");
    }
  }
#endif /* DEBUG */

  instance = lwm2m_engine_get_instance(object, &context, depth);

  /* from POST */
  if(depth > 1 && instance == NULL) {
    if(method != METHOD_PUT && method != METHOD_POST) {
      PRINTF("Error - do not have instance %d\n", context.object_instance_id);
      REST.set_response_status(response, NOT_FOUND_4_04);
      return;
    } else {
      const uint8_t *data;
      int i, len, plen, pos;
      oma_tlv_t tlv;
      PRINTF(">>> CREATE ? %d/%d\n", context.object_id,
             context.object_instance_id);

      for(i = 0; i < object->count; i++) {
        if((object->instances[i].flag & LWM2M_INSTANCE_FLAG_USED) == 0) {
          /* allocate this instance */
          object->instances[i].flag |= LWM2M_INSTANCE_FLAG_USED;
          object->instances[i].id = context.object_instance_id;
          context.object_instance_index = i;
          PRINTF("Created instance: %d\n", context.object_instance_id);
          REST.set_response_status(response, CREATED_2_01);
          instance = &object->instances[i];
          break;
        }
      }

      if(instance == NULL) {
        /* could for some reason not create the instance */
        REST.set_response_status(response, NOT_ACCEPTABLE_4_06);
        return;
      }

      plen = REST.get_request_payload(request, &data);
      if(plen == 0) {
        /* do nothing more */
        return;
      }
      PRINTF("Payload: ");
      for(i = 0; i < plen; i++) {
        PRINTF("%02x", data[i]);
      }
      PRINTF("\n");

      pos = 0;
      do {
        len = oma_tlv_read(&tlv, (uint8_t *)&data[pos], plen - pos);
        PRINTF("Found TLV type=%u id=%u len=%lu\n",
               tlv.type, tlv.id, (unsigned long)tlv.length);
        /* here we need to do callbacks or write value */
        if(tlv.type == OMA_TLV_TYPE_RESOURCE) {
          context.resource_id = tlv.id;
          const lwm2m_resource_t *rsc = lwm2m_get_resource(instance, &context);
          if(rsc != NULL) {
            /* write the value to the resource */
            if(lwm2m_object_is_resource_string(rsc)) {
              PRINTF("  new string value for /%d/%d/%d = ",
                     context.object_id, context.object_instance_id,
                     context.resource_id);
	      PRINTS((int)tlv.length, tlv.value, "%c");
	      PRINTF("\n");
              lwm2m_object_set_resource_string(rsc, &context,
                                               tlv.length, tlv.value);
            } else if(lwm2m_object_is_resource_int(rsc)) {
              PRINTF("  new int value for /%d/%d/%d = %" PRId32 "\n",
                     context.object_id, context.object_instance_id,
                     context.resource_id, oma_tlv_get_int32(&tlv));
              lwm2m_object_set_resource_int(rsc, &context,
                                            oma_tlv_get_int32(&tlv));
            } else if(lwm2m_object_is_resource_floatfix(rsc)) {
              int32_t value;
              if(oma_tlv_float32_to_fix(&tlv, &value, LWM2M_FLOAT32_BITS)) {
                PRINTF("  new float value for /%d/%d/%d = %" PRId32 "\n",
                     context.object_id, context.object_instance_id,
                     context.resource_id, value >> LWM2M_FLOAT32_BITS);
                lwm2m_object_set_resource_floatfix(rsc, &context, value);
              } else {
                PRINTF("  new float value for /%d/%d/%d: FAILED\n",
                     context.object_id, context.object_instance_id,
                     context.resource_id);
              }
            } else if(lwm2m_object_is_resource_boolean(rsc)) {
              PRINTF("  new boolean value for /%d/%d/%d = %" PRId32 "\n",
                     context.object_id, context.object_instance_id,
                     context.resource_id, oma_tlv_get_int32(&tlv));
              lwm2m_object_set_resource_boolean(rsc, &context,
                                                oma_tlv_get_int32(&tlv) != 0);
            }
          }
        }
        pos = pos + len;
      } while(len > 0 && pos < plen);
    }
    return;
  }

  if(depth == 3) {
    const lwm2m_resource_t *resource = lwm2m_get_resource(instance, &context);
    size_t content_len = 0;
    if(resource == NULL) {
      PRINTF("Error - do not have resource %d\n", context.resource_id);
      REST.set_response_status(response, NOT_FOUND_4_04);
      return;
    }
    /* HANDLE PUT */
    if(method == METHOD_PUT) {
      if(lwm2m_object_is_resource_callback(resource)) {
        if(resource->value.callback.write != NULL) {
          /* pick a reader ??? */
          if(format == LWM2M_TEXT_PLAIN) {
            /* a string */
            const uint8_t *data;
            int plen = REST.get_request_payload(request, &data);
            context.reader = &lwm2m_plain_text_reader;
            PRINTF("PUT Callback with data: '");
	    PRINTS(plen, data, "%c");
	    PRINTF("'\n");
            /* no specific reader for plain text */
            content_len = resource->value.callback.write(&context, data, plen,
                                                    buffer, preferred_size);
            PRINTF("content_len:%u\n", (unsigned int)content_len);
            REST.set_response_status(response, CHANGED_2_04);
          } else {
            PRINTF("PUT callback with format %d\n", format);
            REST.set_response_status(response, NOT_ACCEPTABLE_4_06);
          }
        } else {
          PRINTF("PUT - no write callback\n");
          REST.set_response_status(response, METHOD_NOT_ALLOWED_4_05);
        }
      } else {
        PRINTF("PUT on non-callback resource!\n");
        REST.set_response_status(response, METHOD_NOT_ALLOWED_4_05);
      }
      /* HANDLE GET */
    } else if(method == METHOD_GET) {
      if(lwm2m_object_is_resource_string(resource)) {
        const uint8_t *value;
        value = lwm2m_object_get_resource_string(resource, &context);
        if(value != NULL) {
          uint16_t len = lwm2m_object_get_resource_strlen(resource, &context);
          PRINTF("Get string value: ");
	  PRINTS(len, value, "%c");
	  PRINTF("\n");
          content_len = context.writer->write_string(&context, buffer,
            preferred_size, (const char *)value, len);
        }
      } else if(lwm2m_object_is_resource_int(resource)) {
        int32_t value;
        if(lwm2m_object_get_resource_int(resource, &context, &value)) {
          content_len = context.writer->write_int(&context, buffer, preferred_size, value);
        }
      } else if(lwm2m_object_is_resource_floatfix(resource)) {
        int32_t value;
        if(lwm2m_object_get_resource_floatfix(resource, &context, &value)) {
          /* export FLOATFIX */
          PRINTF("Exporting %d-bit fix as float: %" PRId32 "\n",
                 LWM2M_FLOAT32_BITS, value);
          content_len = context.writer->write_float32fix(&context, buffer,
            preferred_size, value, LWM2M_FLOAT32_BITS);
        }
      } else if(lwm2m_object_is_resource_callback(resource)) {
        if(resource->value.callback.read != NULL) {
          content_len = resource->value.callback.read(&context,
                                                 buffer, preferred_size);
        } else {
          REST.set_response_status(response, METHOD_NOT_ALLOWED_4_05);
          return;
        }
      }
      if(content_len > 0) {
        REST.set_response_payload(response, buffer, content_len);
        REST.set_header_content_type(response, context.content_type);
      } else {
        /* failed to produce output - it is an internal error */
        REST.set_response_status(response, INTERNAL_SERVER_ERROR_5_00);
      }
      /* Handle POST */
    } else if(method == METHOD_POST) {
      if(lwm2m_object_is_resource_callback(resource)) {
        if(resource->value.callback.exec != NULL) {
          const uint8_t *data;
          int plen = REST.get_request_payload(request, &data);
          PRINTF("Execute Callback with data: '");
	  PRINTS(plen, data, "%c");
	  PRINTF("'\n");
          content_len = resource->value.callback.exec(&context,
                                                 data, plen,
                                                 buffer, preferred_size);
          REST.set_response_status(response, CHANGED_2_04);
        } else {
          PRINTF("Execute callback - no exec callback\n");
          REST.set_response_status(response, METHOD_NOT_ALLOWED_4_05);
        }
      } else {
        PRINTF("Resource post but no callback resource\n");
        REST.set_response_status(response, METHOD_NOT_ALLOWED_4_05);
      }
    }
  } else if(depth == 2) {
    /* produce an instance response */
    if(method != METHOD_GET) {
      REST.set_response_status(response, METHOD_NOT_ALLOWED_4_05);
    } else if(instance == NULL) {
      REST.set_response_status(response, NOT_FOUND_4_04);
    } else {
      int rdlen;
      if(accept == APPLICATION_LINK_FORMAT) {
        rdlen = write_link_format_data(object, instance,
                                       (char *)buffer, preferred_size);
      } else {
        rdlen = write_json_data(&context, object, instance,
                                (char *)buffer, preferred_size);
      }
      if(rdlen < 0) {
        PRINTF("Failed to generate instance response\n");
        REST.set_response_status(response, SERVICE_UNAVAILABLE_5_03);
        return;
      }
      REST.set_response_payload(response, buffer, rdlen);
      if(accept == APPLICATION_LINK_FORMAT) {
        REST.set_header_content_type(response, REST.type.APPLICATION_LINK_FORMAT);
      } else {
        REST.set_header_content_type(response, LWM2M_JSON);
      }
    }
  } else if(depth == 1) {
    /* produce a list of instances */
    if(method != METHOD_GET) {
      REST.set_response_status(response, METHOD_NOT_ALLOWED_4_05);
    } else {
      int rdlen;
      PRINTF("Sending instance list for object %u\n", object->id);
      /* TODO: if(accept == APPLICATION_LINK_FORMAT) { */
      rdlen = write_object_instances_link(object, (char *)buffer, preferred_size);
      if(rdlen < 0) {
        PRINTF("Failed to generate object response\n");
        REST.set_response_status(response, SERVICE_UNAVAILABLE_5_03);
        return;
      }
      REST.set_header_content_type(response, REST.type.APPLICATION_LINK_FORMAT);
      REST.set_response_payload(response, buffer, rdlen);
    }
  }
}
/*---------------------------------------------------------------------------*/
void
lwm2m_engine_delete_handler(const lwm2m_object_t *object, void *request,
                            void *response, uint8_t *buffer,
                            uint16_t preferred_size, int32_t *offset)
{
  int len;
  const char *url;
  lwm2m_context_t context;

  len = REST.get_url(request, &url);
  PRINTF("*** DELETE URI:'");
  PRINTS(len, url, "%c");
  PRINTF("' called... - responding with DELETED.\n");
  len = lwm2m_engine_parse_context(url, len, request, response,
                                   buffer, preferred_size,
                                   &context);
  PRINTF("Context: %u/%u/%u  found: %d\n", context.object_id,
         context.object_instance_id, context.resource_id, len);

  REST.set_response_status(response, DELETED_2_02);
}
/*---------------------------------------------------------------------------*/
/* Lightweight object instances */
/*---------------------------------------------------------------------------*/
static lwm2m_object_instance_t *last_ins;
static int last_rsc_pos;

static int
perform_discovery(lwm2m_object_instance_t *instance,
                  lwm2m_context_t *ctx)
{
  int pos = 0;
  int size = ctx->outsize;
  int len = 0;
  PRINTF("DISCO - o:%d s:%d lsr:%d lv:%d\n", ctx->offset, size, last_rsc_pos, ctx->level);

  if(ctx->offset == 0) {
    last_ins = instance;
    last_rsc_pos = 0;
    /* Here we should print top node */
  } else {
    /* offset > 0 - assume that we are already in a disco */
    instance = last_ins;
    PRINTF("Old disco: %x\n", last_ins);
    if(last_ins == NULL) {
      ctx->offset = -1;
      ctx->outbuf[0] = ' ';
      pos = 1;
    }
  }

  while(instance != NULL) {
    /* Do the discovery */
    /* Just object this time... */
    if(instance->resource_ids != NULL && instance->resource_count > 0) {
      while(last_rsc_pos < instance->resource_count) {
        if(ctx->level < 3 || ctx->resource_id == instance->resource_ids[last_rsc_pos]) {
          len = snprintf((char *) &ctx->outbuf[pos], size - pos,
                         pos == 0 && ctx->offset == 0 ? "</%d/%d/%d>":",</%d/%d/%d>",
                         instance->object_id, instance->instance_id, instance->resource_ids[last_rsc_pos]);
          if(len < 0 || len + pos >= size) {
            /* ok we trunkated here... */
            ctx->offset += pos;
            ctx->outlen = pos;
            return 1;
          }
          pos += len;
        }
        last_rsc_pos++;
      }
    }
    instance = lwm2m_engine_next_object_instance(ctx, instance);
    last_ins = instance;
    last_rsc_pos = 0;
  }
  /* seems like we are done! */
  ctx->offset=-1;
  ctx->outlen = pos;
  return 1;
}
/*---------------------------------------------------------------------------*/
uint16_t
lwm2m_engine_recommend_instance_id(uint16_t object_id)
{
  lwm2m_object_instance_t *i;
  uint16_t min_id = 0xffff;
  uint16_t max_id = 0;
  int found = 0;
  for(i = list_head(object_list); i != NULL ; i = i->next) {
    if(i->object_id == object_id
       && i->instance_id != LWM2M_OBJECT_INSTANCE_NONE) {
      found++;
      if(i->instance_id > max_id) {
        max_id = i->instance_id;
      }
      if(i->instance_id < min_id) {
        min_id = i->instance_id;
      }
    }
  }
  if(found == 0) {
    /* No existing instances found */
    return 0;
  }
  if(min_id > 0) {
    return min_id - 1;
  }
  return max_id + 1;
}
/*---------------------------------------------------------------------------*/
void
lwm2m_engine_add_object(lwm2m_object_instance_t *object)
{
  list_add(object_list, object);
}
/*---------------------------------------------------------------------------*/
void
lwm2m_engine_remove_object(lwm2m_object_instance_t *object)
{
  list_remove(object_list, object);
}
/*---------------------------------------------------------------------------*/
static lwm2m_object_instance_t *
lwm2m_engine_get_object_instance(const lwm2m_context_t *context)
{
  lwm2m_object_instance_t *i;
  for(i = list_head(object_list); i != NULL ; i = i->next) {
    if(i->object_id == context->object_id &&
       ((context->level < 2) || i->instance_id == context->object_instance_id)) {
      return i;
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
static lwm2m_object_instance_t *
lwm2m_engine_next_object_instance(const lwm2m_context_t *context, lwm2m_object_instance_t *last)
{
  while(last != NULL) {
    last = last->next;
    if(last != NULL && last->object_id == context->object_id &&
       ((context->level < 2) || last->instance_id == context->object_instance_id)) {
      return last;
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
static int
lwm2m_handler_callback(coap_packet_t *request, coap_packet_t *response,
                       uint8_t *buffer, uint16_t buffer_size, int32_t *offset)
{
  const char *url;
  int url_len;
  unsigned int format;
  unsigned int accept;
  int depth;
  lwm2m_context_t context;
  lwm2m_object_instance_t *instance;
  uint32_t bnum;
  uint8_t bmore;
  uint16_t bsize;
  uint32_t boffset;
  uint8_t success = 1; /* the success boolean */

  url_len = REST.get_url(request, &url);
  depth = lwm2m_engine_parse_context(url, url_len, request, response,
                                     buffer, buffer_size, &context);

  PRINTF("URL:");
  PRINTS(url_len, url, "%c");
  PRINTF(" CTX:%u/%u/%u\n", context.object_id, context.object_instance_id,
	 context.resource_id);
  /* Get format and accept */
  if(!REST.get_header_content_type(request, &format)) {
    PRINTF("lwm2m: No format given. Assume text plain...\n");
    format = LWM2M_TEXT_PLAIN;
  } else if(format == TEXT_PLAIN) {
    /* CoAP content format text plain - assume LWM2M text plain */
    format = LWM2M_TEXT_PLAIN;
  }
  if(!REST.get_header_accept(request, &accept)) {
    PRINTF("lwm2m: No Accept header, using same as Content-format...\n");
    accept = format;
  }

  /**
   * 1 => Object only
   * 2 => Object and Instance
   * 3 => Object and Instance and Resource
   */
  if(depth < 1) {
    /* No possible object id found in URL - ignore request */
    return 0;
  }

  instance = lwm2m_engine_get_object_instance(&context);
  if(instance == NULL || instance->callback == NULL) {
    /* No matching object/instance found - ignore request */
    return 0;
  }

  PRINTF("lwm2m Context: %u/%u/%u  found: %d\n",
         context.object_id,
         context.object_instance_id, context.resource_id, depth);
  /*
   * Select reader and writer based on provided Content type and
   * Accept headers.
   */
  lwm2m_engine_select_reader(&context, format);
  lwm2m_engine_select_writer(&context, accept);

  switch(REST.get_method_type(request)) {
  case METHOD_PUT:
    /* can also be write atts */
    context.operation = LWM2M_OP_WRITE;
    REST.set_response_status(response, CHANGED_2_04);
    break;
  case METHOD_POST:
    if(context.level == 2) {
      /* write to a instance */
      context.operation = LWM2M_OP_WRITE;
      REST.set_response_status(response, CHANGED_2_04);
    } else if(context.level == 3) {
      context.operation = LWM2M_OP_EXECUTE;
      REST.set_response_status(response, CHANGED_2_04);
    }
    break;
  case METHOD_GET:
    /* Assuming that we haev already taken care of discovery... it will be read q*/
    if(accept == APPLICATION_LINK_FORMAT) {
      context.operation = LWM2M_OP_DISCOVER;
    } else {
      context.operation = LWM2M_OP_READ;
    }
    REST.set_response_status(response, CONTENT_2_05);
    break;
  default:
    break;
  }

#if DEBUG
  /* for debugging */
  PRINTF("lwm2m %s Format:%d ID:%d bsize:%u\n",
         get_method_as_string(REST.get_method_type(request)),
         format, context.object_id, buffer_size);
  if(format == LWM2M_TEXT_PLAIN) {
    /* a string */
    const uint8_t *data;
    int plen = REST.get_request_payload(request, &data);
    if(plen > 0) {
      PRINTF("Data: '");
      PRINTS(plen, data, "%c");
      PRINTF("'\n");
    }
  }
#endif /* DEBUG */

  context.offset = *offset;
  context.insize = coap_get_payload(request, (const uint8_t **) &context.inbuf);
  context.inpos = 0;

  /* PUT/POST - e.g. write will not send in offset here - Maybe in the future? */
  if(*offset == 0 && IS_OPTION(request, COAP_OPTION_BLOCK1)) {
    coap_get_header_block1(request, &bnum, &bmore, &bsize, &boffset);
    context.offset = boffset;
  }

    /* This is a discovery operation */
  if(context.operation == LWM2M_OP_DISCOVER) {
    /* Assume only one disco at a time... */
    success = perform_discovery(instance, &context);
  } else {
    PRINTF("LWM2M: Doing callback...\n");
    /* If not discovery or create - this is a regular OP - do the callback */
    success = instance->callback(instance, &context);
  }

  if(success) {
    /* Handle blockwise 1 */
    if(IS_OPTION(request, COAP_OPTION_BLOCK1)) {
      PRINTF("Setting BLOCK 1 num:%d o2:%d o:%d\n", bnum, boffset, *offset);
      coap_set_header_block1(response, bnum, 0, bsize);
    }

    if(context.outlen > 0) {
      PRINTF("lwm2m: replying with %u bytes\n", context.outlen);
      REST.set_response_payload(response, context.outbuf, context.outlen);
      REST.set_header_content_type(response, context.content_type);

      *offset = context.offset;
    } else {
      PRINTF("lwm2m: no data in reply\n", url_len, url);
    }
  } else {
    /* Failed to handle the request */
    REST.set_response_status(response, INTERNAL_SERVER_ERROR_5_00);
    PRINTF("lwm2m: resource failed\n", url_len, url);
  }
  return 1;
}
/*---------------------------------------------------------------------------*/
/** @} */
