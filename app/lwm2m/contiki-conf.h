/*
 * Copyright (c) 2016, SICS, Swedish ICT AB.
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
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * \file
 *         Configuration for Contiki library
 * \author
 *         Niclas Finne <nfi@sics.se>
 *         Joakim Eriksson <joakime@sics.se>
 */

#ifndef CONTIKI_CONF_H_
#define CONTIKI_CONF_H_

#include <stdint.h>
#include "node-endpoint.h"

#define CC_CONF_REGISTER_ARGS 0
#define CC_CONF_FUNCTION_POINTER_ARGS 0
#define CC_CONF_UNSIGNED_CHAR_BUGS 0
#define CC_CONF_DOUBLE_HASH 0
#define CC_CONF_NO_VA_ARGS  0

#define UIP_CONF_IPV6_RPL 0

#define NTIMER_CONF_DRIVER ntimer_nodemcu_driver

/* For device and endpoint */
extern char *nodemcu_ep;
extern char *nodemcu_man;
extern char *nodemcu_firmware;
extern char *nodemcu_serialnumber;
extern char *nodemcu_modelnumber;

#define PLATFORM_HAS_BUTTON 0

#define LWM2M_ENGINE_CLIENT_ENDPOINT_NAME nodemcu_ep
#define LWM2M_DEVICE_MANUFACTURER nodemcu_man
#define LWM2M_DEVICE_TYPE "NodeMCU-lwm2m"
#define LWM2M_DEVICE_MODEL_NUMBER nodemcu_modelnumber
#define LWM2M_DEVICE_SERIAL_NUMBER nodemcu_serialnumber
#define LWM2M_DEVICE_FIRMWARE_VERSION nodemcu_firmware

#ifdef COAP_TRANSPORT_CONF_H
#include COAP_TRANSPORT_CONF_H
#endif

#define COAP_ENDPOINT_CUSTOM 1
#define REST_MAX_CHUNK_SIZE           256

#endif /* CONTIKI_CONF_H_ */
