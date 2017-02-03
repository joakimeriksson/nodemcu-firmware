/*
 * Copyright (c) 2017, SICS, Swedish ICT AB.
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
 *         Setup for the lwm2m app
 * \author
 *         Joakim Eriksson <joakime@sics.se>
 */


#include "contiki-conf.h"
#include "sys/ntimer.h"
#include <string.h>
#include <stdio.h>
#include "c_stdlib.h" /* for the c_zalloc */
#include "oma-lwm2m/lwm2m-engine.h"
#include "oma-lwm2m/lwm2m-device.h"
#include "ipso-objects/ipso-sensor-template.h"

void ipso_sensor_temp_init(void);
void lwm2m_call_lua_function(const char *lua_fn_name, int32_t *value);

static char ep[40];
static char man[80];
static char fv[16];
static char mn[16];
static char sn[16];


/* Variables used in other files */
char *nodemcu_ep = ep;
char *nodemcu_man = man;
char *nodemcu_firmware = fv;
char *nodemcu_serialnumber = sn;
char *nodemcu_modelnumber = mn;

/* Need to keep the sensor struct first in order to figure out where
   the rest is as the sensor will be the thing at the first address when
   getting the callback */

struct sensor_entry {
  ipso_sensor_t sensor;
  ipso_sensor_value_t value; /* has the reg object */
  const char *lua_fn_name;
  struct sensor_entry *next;
};

struct sensor_entry *sensors = NULL;

lwm2m_status_t
get_lua_value(const ipso_sensor_t *s, int32_t *value)
{
  struct sensor_entry *se = (struct sensor_entry *) s;
  printf("*** Should call: %s (%d/%d)\n", se->lua_fn_name,
	 s->object_id, s->instance_id);

  /* callu lua and set the value */
  lwm2m_call_lua_function(se->lua_fn_name, value);
  
  return LWM2M_STATUS_OK;
}

/*------------------------------------------------------------------------*/
int lwm2m_add_sensor(int obj_id, int instance_id, const char *unit,
		     const char *fn_name)
{
  struct sensor_entry *head = sensors;
  struct sensor_entry *new_sensor;
  while(head != NULL) {
    if(head->sensor.object_id == obj_id &&
       head->sensor.instance_id == instance_id) {
      printf("LWM2M: sensor already registered\n");
      return 0;
    }
  }
  head = sensors;
  new_sensor = (struct sensor_entry *)c_zalloc(sizeof(struct sensor_entry));
  if(new_sensor == NULL) {    
    return 0;
  }
  
  new_sensor->sensor.object_id = obj_id;
  new_sensor->sensor.instance_id = instance_id;
  new_sensor->sensor.sensor_value = &new_sensor->value;
  new_sensor->sensor.get_value_in_millis = get_lua_value;
  new_sensor->sensor.unit = (char *) unit;
  new_sensor->lua_fn_name = fn_name;
  new_sensor->next = NULL;

  printf("LWM2M: adding sensor:%d/%d %s\n", obj_id, instance_id, fn_name);
  
  if(head == NULL) {
    sensors = new_sensor;
  } else {
    new_sensor->next = sensors;
    sensors = new_sensor->next;
  }
  
  ipso_sensor_add(&new_sensor->sensor);

}


void lwm2m_app_init(void)
{
  /* Initializing drivers, etc */

  /* default name of endpoint */
  memset(ep, 0, sizeof(ep));
  memset(man, 0, sizeof(man));
  memset(fv, 0, sizeof(fv));
  memset(mn, 0, sizeof(mn));

  memcpy(nodemcu_ep, "nodemcu-ep", strlen("nodemcu-ep"));
  memcpy(nodemcu_man, "SICS", strlen("SICS"));
  memcpy(nodemcu_firmware, "0.0.1", strlen("0.0.1"));
  memcpy(nodemcu_serialnumber, "1324", strlen("1324"));
  memcpy(nodemcu_modelnumber, "the-model", strlen("the-model"));

  printf("LWM2M APP INIT. EP:%s\n", nodemcu_ep);
  
  NTIMER_DRIVER.init();
  lwm2m_engine_init();
  lwm2m_device_init();
  
  ipso_sensor_temp_init();
}
