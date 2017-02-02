/*
 * Copyright (c) 2016, Eistec AB.
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
 *         Implementation of the Contiki OMA LWM2M JSON writer
 * \author
 *         Joakim Nohlgård <joakim.nohlgard@eistec.se>
 *         Joakim Eriksson <joakime@sics.se> added JSON reader parts
 */

#include "lwm2m-object.h"
#include "lwm2m-json.h"
#include "lwm2m-plain-text.h"
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#define DEBUG 0
#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif
/*---------------------------------------------------------------------------*/

/* {"e":[{"n":"111/1","v":123},{"n":"111/2","v":42}]} */

/* Begin String */
#define T_NONE       0
#define T_STRING_B   1
#define T_STRING     2
#define T_NAME       4
#define T_VNUM       5
#define T_OBJ        6
#define T_VAL        7

/* Simlified JSON style reader for reading in values from a LWM2M JSON
   string */
int lwm2m_json_next_token(lwm2m_context_t *ctx, struct json_data *json) {
  int pos = ctx->inpos;
  uint8_t type = T_NONE;
  uint8_t vpos_start = 0;
  uint8_t vpos_end = 0;
  uint8_t cont;
  uint8_t wscount = 0;

  json->name_len = 0;
  json->value_len = 0;

  cont = 1;
  /* We will be either at start, or at a specific position */
  while(pos < ctx->insize && cont) {
    uint8_t c = ctx->inbuf[pos++];
    switch(c) {
    case '{': type = T_OBJ; break;
    case '}':
    case ',':
      if(type == T_VAL || type == T_STRING) {
        json->value = &ctx->inbuf[vpos_start];
        json->value_len = vpos_end - vpos_start - wscount;
        type = T_NONE;
        cont = 0;
      }
      wscount = 0;
      break;
    case '\\':
      /* stuffing */
      if(pos < ctx->insize) {
        pos++;
        vpos_end = pos;
      }
      break;
    case '"':
      if(type == T_STRING_B) {
        type = T_STRING;
        vpos_end = pos - 1;
        wscount = 0;
      } else {
        type = T_STRING_B;
        vpos_start = pos;
      }
      break;
    case ':':
      if(type == T_STRING) {
        json->name = &ctx->inbuf[vpos_start];
        json->name_len = vpos_end - vpos_start;
        vpos_start = vpos_end = pos;
        type = T_VAL;
      } else {
        /* Could be in string or at illegal pos */
        if(type != T_STRING_B) {
          PRINTF("ERROR - illegal ':'\n");
        }
      }
      break;
      /* ignore whitespace */
    case ' ':
    case '\n':
    case '\t':
      if(type != T_STRING_B) {
        if(vpos_start == pos - 1) {
          vpos_start = pos;
        } else {
          wscount++;
        }
      }
    default:
      vpos_end = pos;
    }
  }

  if(cont == 0 && pos < ctx->insize) {
    ctx->inpos = pos;
  }
  /* OK if cont == 0 othewise we failed */
  return cont == 0 && pos < ctx->insize;
}
/*---------------------------------------------------------------------------*/
static size_t
init_write(lwm2m_context_t *ctx)
{
  int len = snprintf((char *)&ctx->outbuf[ctx->outlen],
                     ctx->outsize - ctx->outlen, "{\"e\":[");
  ctx->writer_flags = 0; /* set flags to zero */
  if((len < 0) || (len >= ctx->outsize)) {
    return 0;
  }
  return len;
}
/*---------------------------------------------------------------------------*/
static size_t
end_write(lwm2m_context_t *ctx)
{
  int len = snprintf((char *)&ctx->outbuf[ctx->outlen],
                     ctx->outsize - ctx->outlen, "]}");
  if((len < 0) || (len >= ctx->outsize - ctx->outlen)) {
    return 0;
  }
  return len;
}
/*---------------------------------------------------------------------------*/
static size_t
write_boolean(lwm2m_context_t *ctx, uint8_t *outbuf, size_t outlen,
              int value)
{
  char *sep = (ctx->writer_flags & WRITER_OUTPUT_VALUE) ? "," : "";
  int len = snprintf((char *)outbuf, outlen, "%s{\"n\":\"%u\",\"bv\":%s}", sep, ctx->resource_id, value ? "true" : "false");
  if((len < 0) || (len >= outlen)) {
    return 0;
  }
  ctx->writer_flags |= WRITER_OUTPUT_VALUE;
  return len;
}
/*---------------------------------------------------------------------------*/
static size_t
write_int(lwm2m_context_t *ctx, uint8_t *outbuf, size_t outlen,
          int32_t value)
{
  char *sep = (ctx->writer_flags & WRITER_OUTPUT_VALUE) ? "," : "";
  int len = snprintf((char *)outbuf, outlen, "%s{\"n\":\"%u\",\"v\":%" PRId32 "}", sep, ctx->resource_id, value);
  if((len < 0) || (len >= outlen)) {
    return 0;
  }
  ctx->writer_flags |= WRITER_OUTPUT_VALUE;
  return len;
}
/*---------------------------------------------------------------------------*/
static size_t
write_float32fix(lwm2m_context_t *ctx, uint8_t *outbuf, size_t outlen,
                 int32_t value, int bits)
{
  char *sep = (ctx->writer_flags & WRITER_OUTPUT_VALUE) ? "," : "";
  size_t len = 0;
  int res;
  res = snprintf((char *)outbuf, outlen, "%s{\"n\":\"%u\",\"v\":", sep, ctx->resource_id);
  if(res <= 0 || res >= outlen) {
    return 0;
  }
  len += res;
  outlen -= res;
  res = lwm2m_plain_text_write_float32fix(&outbuf[len], outlen, value, bits);
  if((res <= 0) || (res >= outlen)) {
    return 0;
  }
  len += res;
  outlen -= res;
  res = snprintf((char *)&outbuf[len], outlen, "}");
  if((res <= 0) || (res >= outlen)) {
    return 0;
  }
  len += res;
  ctx->writer_flags |= WRITER_OUTPUT_VALUE;
  return len;
}
/*---------------------------------------------------------------------------*/
static size_t
write_string(lwm2m_context_t *ctx, uint8_t *outbuf, size_t outlen,
             const char *value, size_t stringlen)
{
  char *sep = (ctx->writer_flags & WRITER_OUTPUT_VALUE) ? "," : "";
  size_t i;
  size_t len = 0;
  int res;
  PRINTF("{\"n\":\"%u\",\"sv\":\"", ctx->resource_id);
  res = snprintf((char *)outbuf, outlen, "%s{\"n\":\"%u\",\"sv\":\"", sep,
                 ctx->resource_id);
  if(res < 0 || res >= outlen) {
    return 0;
  }
  len += res;
  for (i = 0; i < stringlen && len < outlen; ++i) {
    /* Escape special characters */
    /* TODO: Handle UTF-8 strings */
    if(value[i] < '\x20') {
      PRINTF("\\x%x", value[i]);
      res = snprintf((char *)&outbuf[len], outlen - len, "\\x%x", value[i]);
      if((res < 0) || (res >= (outlen - len))) {
        return 0;
      }
      len += res;
      continue;
    } else if(value[i] == '"' || value[i] == '\\') {
      PRINTF("\\");
      outbuf[len] = '\\';
      ++len;
      if(len >= outlen) {
        return 0;
      }
    }
    PRINTF("%c", value[i]);
    outbuf[len] = value[i];
    ++len;
    if(len >= outlen) {
      return 0;
    }
  }
  PRINTF("\"}\n");
  res = snprintf((char *)&outbuf[len], outlen - len, "\"}");
  if((res < 0) || (res >= (outlen - len))) {
    return 0;
  }
  len += res;
  ctx->writer_flags |= WRITER_OUTPUT_VALUE;
  return len;
}
/*---------------------------------------------------------------------------*/
const lwm2m_writer_t lwm2m_json_writer = {
  init_write,
  end_write,
  write_int,
  write_string,
  write_float32fix,
  write_boolean
};
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
/** @} */
