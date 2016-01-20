/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 * -*- */

/* Publish-Subscribe Broker for the Constrained Application Protocol (CoAP)
 * draft-koster-core-coap-pubsub-04
 *
 * Copyright (C) 2015-2016 Mikko Majanen <mikko.majanen@vtt.fi>,
 * VTT Technical Research Centre of Finland Ltd
 *
 * All rights reserved.

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:

  o Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  o Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


 * Bases on the example server code provided by CoAP library libcoap.

 */


/* coap -- simple implementation of the Constrained Application Protocol (CoAP)
 *         as defined in RFC 7252
 *
 * Copyright (C) 2010--2015 Olaf Bergmann <bergmann@tzi.org>
 *
 * This file is part of the CoAP library libcoap. Please see README for terms
 * of use.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>

#include "coap_config.h"
#include "resource.h"
#include "coap.h"

#define COAP_RESOURCE_CHECK_TIME 2

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

/* temporary storage for dynamic resource representations */
static int quit = 0;

/* changeable clock base (see handle_put_time()) */
static time_t clock_offset;
static time_t my_clock_base = 0;

struct coap_resource_t *time_resource = NULL;

struct coap_resource_t *pubsub_resource = NULL; //MiM

//#define COAP_STATS //whether to include stats or not; not part of the ietf draft (MiM 19.11.2015)
#ifdef COAP_STATS
struct coap_resource_t *pubsub_stats_resource = NULL; //MiM 19.11.2015
int n_sent_messages = 0; //number of all sent messages (including statistics)
int n_sent_stats = 0; //number of sent statistics messages
int n_sent_notifications = 0; //number of sent notifications (including stats)
int n_topics = 0; //number of pubsub topics under /ps (including statistics)
int n_subscribers = 0; //number of subscribers
int n_publishments = 0; //number of correctly received pub messages (PUT)
int n_error_responses = 0; //number of sent error messages
#endif

//hash table item (MiM):
struct topic_value {
  coap_key_t urikeyhash;    /* key, unsigned char[4] */
  size_t vlen;      /* topic value length */
  void *tvalue;  /* topic value */
  UT_hash_handle hh; /* makes this structure hashable */
};

struct topic_value *topic_values = NULL; //MiM: hash table for topic values



#ifndef WITHOUT_ASYNC
/* This variable is used to mimic long-running tasks that require
 * asynchronous responses. */
static coap_async_state_t *async = NULL;
#endif /* WITHOUT_ASYNC */

#ifdef __GNUC__
#define UNUSED_PARAM __attribute__ ((unused))
#else /* not a GCC */
#define UNUSED_PARAM
#endif /* GCC */

/* SIGINT handler: set quit to 1 for graceful termination */
static void
handle_sigint(int signum UNUSED_PARAM) {
  quit = 1;
}

#define INDEX "This is a CoAP publish-subscribe broker made with libcoap\n" \
              "Copyright (C) 2015--2016 Mikko Majanen <mikko.majanen@vtt.fi>\n"

static void
hnd_get_index(coap_context_t *ctx UNUSED_PARAM,
              struct coap_resource_t *resource UNUSED_PARAM,
              const coap_endpoint_t *local_interface UNUSED_PARAM,
              coap_address_t *peer UNUSED_PARAM,
              coap_pdu_t *request UNUSED_PARAM,
              str *token UNUSED_PARAM,
              coap_pdu_t *response) {
  unsigned char buf[3];

  response->hdr->code = COAP_RESPONSE_CODE(205);

  coap_add_option(response,
                  COAP_OPTION_CONTENT_TYPE,
                  coap_encode_var_bytes(buf, COAP_MEDIATYPE_TEXT_PLAIN), buf);

  coap_add_option(response,
                  COAP_OPTION_MAXAGE,
                  coap_encode_var_bytes(buf, 0x2ffff), buf);

  coap_add_data(response, strlen(INDEX), (unsigned char *)INDEX);
}

static void
hnd_get_time(coap_context_t  *ctx,
             struct coap_resource_t *resource,
             const coap_endpoint_t *local_interface UNUSED_PARAM,
             coap_address_t *peer,
             coap_pdu_t *request,
             str *token,
             coap_pdu_t *response) {
  coap_opt_iterator_t opt_iter;
  coap_opt_t *option;
  unsigned char buf[40];
  size_t len;
  time_t now;
  coap_tick_t t;

  /* FIXME: return time, e.g. in human-readable by default and ticks
   * when query ?ticks is given. */

  /* if my_clock_base was deleted, we pretend to have no such resource */
  response->hdr->code =
    my_clock_base ? COAP_RESPONSE_CODE(205) : COAP_RESPONSE_CODE(404);

  if (coap_find_observer(resource, peer, token)) {
    /* FIXME: need to check for resource->dirty? */
    coap_add_option(response,
                    COAP_OPTION_OBSERVE,
                    coap_encode_var_bytes(buf, ctx->observe), buf);
  }

  if (my_clock_base)
    coap_add_option(response,
                    COAP_OPTION_CONTENT_FORMAT,
                    coap_encode_var_bytes(buf, COAP_MEDIATYPE_TEXT_PLAIN), buf);

  coap_add_option(response,
                  COAP_OPTION_MAXAGE,
                  coap_encode_var_bytes(buf, 0x01), buf);

  if (my_clock_base) {

    /* calculate current time */
    coap_ticks(&t);
    now = my_clock_base + (t / COAP_TICKS_PER_SECOND);

    if (request != NULL
        && (option = coap_check_option(request, COAP_OPTION_URI_QUERY, &opt_iter))
        && memcmp(COAP_OPT_VALUE(option), "ticks",
        min(5, COAP_OPT_LENGTH(option))) == 0) {
          /* output ticks */
          len = snprintf((char *)buf,
                         min(sizeof(buf),
                             response->max_size - response->length),
                             "%u", (unsigned int)now);
          coap_add_data(response, len, buf);

    } else {      /* output human-readable time */
      struct tm *tmp;
      tmp = gmtime(&now);
      len = strftime((char *)buf,
                     min(sizeof(buf),
                     response->max_size - response->length),
                     "%b %d %H:%M:%S", tmp);
      coap_add_data(response, len, buf);
    }
  }
}

static void
hnd_put_time(coap_context_t *ctx UNUSED_PARAM,
             struct coap_resource_t *resource UNUSED_PARAM,
             const coap_endpoint_t *local_interface UNUSED_PARAM,
             coap_address_t *peer UNUSED_PARAM,
             coap_pdu_t *request,
             str *token UNUSED_PARAM,
             coap_pdu_t *response) {
  coap_tick_t t;
  size_t size;
  unsigned char *data;

  /* FIXME: re-set my_clock_base to clock_offset if my_clock_base == 0
   * and request is empty. When not empty, set to value in request payload
   * (insist on query ?ticks). Return Created or Ok.
   */

  /* if my_clock_base was deleted, we pretend to have no such resource */
  response->hdr->code =
    my_clock_base ? COAP_RESPONSE_CODE(204) : COAP_RESPONSE_CODE(201);

  resource->dirty = 1;

  coap_get_data(request, &size, &data);

  if (size == 0)        /* re-init */
    my_clock_base = clock_offset;
  else {
    my_clock_base = 0;
    coap_ticks(&t);
    while(size--)
      my_clock_base = my_clock_base * 10 + *data++;
    my_clock_base -= t / COAP_TICKS_PER_SECOND;
  }
}

static void
hnd_delete_time(coap_context_t *ctx UNUSED_PARAM,
                struct coap_resource_t *resource UNUSED_PARAM,
                const coap_endpoint_t *local_interface UNUSED_PARAM,
                coap_address_t *peer UNUSED_PARAM,
                coap_pdu_t *request UNUSED_PARAM,
                str *token UNUSED_PARAM,
                coap_pdu_t *response UNUSED_PARAM) {
  my_clock_base = 0;    /* mark clock as "deleted" */

  /* type = request->hdr->type == COAP_MESSAGE_CON  */
  /*   ? COAP_MESSAGE_ACK : COAP_MESSAGE_NON; */
}


// MiM: CoAP pub/sub handler functions ////////////////////////


void 
hnd_put_ps(coap_context_t  *ctx, struct coap_resource_t *resource, const coap_endpoint_t *local_interface, 
	     coap_address_t *peer, coap_pdu_t *request, str *token,
	     coap_pdu_t *response) {

  debug("hnd_put_ps()\n");

  size_t size;
  unsigned char *data;
  unsigned char topic[30] = {0};
  coap_key_t urikey;
  struct topic_value *tv = NULL;
  
  int expired = -1; //whether the Max-Age for this resource is expired or not

  coap_opt_iterator_t opt_iter;
  coap_opt_t *option;
  unsigned char *p;
  
  //read the topic, it is part of the uri option ( /ps/topic )
  coap_hash_request_uri(request, urikey);
  //urikey is used as a hash key in resource value hash table...
  //find the right resource value by using the urikey
  debug("urikey=0x%x%x%x%x\n", urikey[0],urikey[1],urikey[2], urikey[3]);

  //cf should be the same as the topic ct; otherwise return error 
  //(MiM 4.1.2016)
  // check Content-Format option in PUT request
  debug("checking Content-Format option in PUT...\n");
  option = coap_check_option(request, COAP_OPTION_CONTENT_FORMAT, &opt_iter);
  
  if(option){
    //compare to topic's ct attribute
    p = COAP_OPT_VALUE(option);
    
    unsigned int opt_value = coap_decode_var_bytes(p, COAP_OPT_LENGTH(option));
    debug("Content-Format = %u\n", opt_value);
    coap_attr_t *ct = coap_find_attr(resource, "ct", 2);
    if(ct){
      debug("topic ct=%d\n", atoi(ct->value.s));
      
      if(atoi(ct->value.s) != opt_value){
	debug("content type not allowed!\n");
	response->hdr->code = COAP_RESPONSE_CODE(415);//Unsupported ct
	return;
      }
      else {
	debug("topic's ct matches with content-format option\n");
      }
    }
    else {
      //no ct attribute in topic
      debug("did not find ct for topic!\n");
      response->hdr->code = COAP_RESPONSE_CODE(415);//Unsupported ct
      return;
    }

  }//if(option..
  else {
    //no content-format option in PUT message
    debug("no content-format option in PUT message!\n");
    response->hdr->code = COAP_RESPONSE_CODE(415);//Unsupported ct
    return;
  }

  
  coap_attr_t *maxage = coap_find_attr(resource, "max-age", 7);
  if(maxage != NULL){
    debug("Max-Age of this resource is %s\n", maxage->value.s);
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    if(now.tv_sec > atoi(maxage->value.s)){
      debug("resource has expired its Max-Age!\n");
      expired = 1;
    }
    else {
      debug("Max-Age not expired\n");
      expired = 0;
    }

  }
  else {
    debug("no Max-Age for this resource\n");
    expired = -1;
  }


  //read the topic value from the payload
  if(coap_get_data(request, &size, &data)){
    //debug("payload size = %d, data=%s\n", size, data); 
    memcpy(topic, data, size);
    debug("topic value = %s\n", topic);
   
   
    int found = 0;
    //Check if the topic already exists: 
    HASH_FIND_INT(topic_values, urikey, tv);
    
    if(tv != NULL){
      debug("topic already has a value\n");
      found = 1;
    }

    //If the topic value does not exist, 
    //create new hash table entry with (urikey, topic_value)
    if(found == 0){
      //delete the resource if Max-Age expired since no PUTs in time:
      if(expired == 1){
	int deleted = coap_delete_resource(ctx, resource->key);
	if(deleted == 1){
	  debug("resource deleted\n");
	  response->hdr->code = COAP_RESPONSE_CODE(404);//NOT FOUND
#ifdef COAP_STATS
	  n_error_responses++;
	  n_sent_messages++;
#endif
	  return;
	}
	else {
	  debug("resource not deleted/found");
	  response->hdr->code = COAP_RESPONSE_CODE(404);//NOT FOUND or internal error or something like that?
#ifdef COAP_STATS
	  n_error_responses++;
	  n_sent_messages++;
#endif
	  return;
	}
      }//if(expired==1...


      if(expired == 0){
	debug("Max-Age not expired, first PUT on this topic in time, deleting Max-Age\n");
	LL_DELETE(resource->link_attr, maxage);
	coap_delete_attr(maxage);
	maxage = NULL;
      }
      
      tv = (struct topic_value*)calloc(1, sizeof(struct topic_value));
      memcpy(&(tv->urikeyhash), urikey, 4);//MiM 5.11.2015
      tv->vlen = size;
      tv->tvalue = (unsigned char*)calloc(1, size+1); 
      memcpy(tv->tvalue, topic, size);
      HASH_ADD_INT(topic_values, urikeyhash, tv);//MiM 5.11.2015
      
      
      debug("added value to resource = 0x%x%x%x%x, HASH_COUNT=%d\n", tv->urikeyhash[0],tv->urikeyhash[1], tv->urikeyhash[2],tv->urikeyhash[3], HASH_COUNT(topic_values));
      
    } //if found==0

    else { //topic already existed, update the existing value
      debug("updating the topic value\n");
      if(size != tv->vlen)
	tv->tvalue = (unsigned char*)calloc(1, size+1); //MiM 15.12.2015 != 
      tv->vlen = size;
      memcpy(tv->tvalue, topic, size);
     
    }
  

    //Check Max-Age option in PUT request:
    debug("checking Max-Age option...\n");
   
    option = coap_check_option(request, COAP_OPTION_MAXAGE, &opt_iter);
    if(option){
      p = COAP_OPT_VALUE(option);
      unsigned int option_length = coap_opt_length(option);
      unsigned int max_age = 0;
      char pvalue[option_length+1];
      memcpy(pvalue, p, option_length);
      pvalue[option_length] = '\0';
      max_age = atoi(pvalue);


      /* calculate current time and expiration time */
      struct timeval now;
      gettimeofday(&now, NULL);
      now.tv_sec += max_age;

      if(maxage){
	//delete the old max-age attribute and then create new one
	LL_DELETE(resource->link_attr, maxage);	
	coap_delete_attr(maxage);
	maxage = NULL;
      }
      
      //create max-age attribute
      char *expiration = calloc(1, 32);
      if(expiration == NULL){
	debug("error in calloc\n");
	response->hdr->code = COAP_RESPONSE_CODE(500);//Internal Server Error
	return;
      }
      sprintf(expiration, "%d", now.tv_sec);
      char *attrname = calloc(1, 8);
      sprintf(attrname, "%s", "max-age");
      
      
      coap_attr_t *a = NULL;
      a = coap_add_attr(resource, attrname/*"max-age"*/, 7, expiration, 32, COAP_ATTR_FLAGS_RELEASE_NAME | COAP_ATTR_FLAGS_RELEASE_VALUE); 
      //flags == 3
      
    
    } //if (option)...
    else {
      //no max-age option in PUT, delete possible existing max-age attribute:
      debug("no max-age option in PUT, deleting existing max-age\n");
      if(maxage != NULL){
	debug("delete maxage attr and set to NULL\n");
	LL_DELETE(resource->link_attr, maxage);
	coap_delete_attr(maxage);
	maxage = NULL;
      }
    }
    

    //return 2.04 Changed
    response->hdr->code = COAP_RESPONSE_CODE(204);
    
    //CHECK REQUEST TYPE (NON/CON); if NON, change resource's flag to NON so that the notifications will be NON type messages (MiM)
    //TODO: this is not anymore in draft version 04 (MiM 12.1.2016)
    if(request->hdr->type == COAP_MESSAGE_NON){
      debug("setting notifications to NON type\n");
      coap_resource_set_mode(resource, COAP_RESOURCE_FLAGS_NOTIFY_NON);
    }
    else if(request->hdr->type == COAP_MESSAGE_CON){
      debug("setting notifications to CON type\n");
      coap_resource_set_mode(resource, COAP_RESOURCE_FLAGS_NOTIFY_CON);
    }

#ifdef COAP_STATS
	  n_sent_messages++;
	  n_publishments++;
#endif

    debug("notifying subscribers...\n");
    resource->dirty = 1; //notify subscribers

#ifdef COAP_STATS
    debug("notifying statistics subscribers...\n");
    pubsub_stats_resource->dirty = 1; //notify statistics subscribers
#endif

  }
  else {
    debug("error reading data from request\n");
    //send error response 4.00 Bad Request
    debug("error code = %s\n", coap_response_phrase(COAP_RESPONSE_CODE(400)));
    response->hdr->code = COAP_RESPONSE_CODE(400);
#ifdef COAP_STATS
	  n_error_responses++;
	  n_sent_messages++;
#endif
  }


}

void 
hnd_get_ps(coap_context_t  *ctx, struct coap_resource_t *resource, const coap_endpoint_t *local_interface,
	   coap_address_t *peer, coap_pdu_t *request, str *token,
	   coap_pdu_t *response) {
  

  debug("hnd_get_ps()\n");

#ifdef COAP_STATS
  n_sent_messages++;
#endif
  
  coap_key_t urikey;
  struct topic_value *tv = NULL;
  
  coap_opt_iterator_t opt_iter;
  coap_opt_t *option;
  unsigned char buf[40];
  coap_subscription_t *subscription;
  unsigned char *p;
  
  unsigned int observe_action;//MiM 30.9.2015

  //MiM 5.1.2015
  coap_attr_t *cf = coap_find_attr(resource, "ct", 2);
  if(cf == NULL){
    debug("topic does not have ct!\n");
    response->hdr->code = COAP_RESPONSE_CODE(415);//Unsupported ct or else?
    return;
  }

  //Check the Accept (content-format) option (MiM 15.1.2015):
  if(request != NULL){
    option = coap_check_option(request, COAP_OPTION_ACCEPT, &opt_iter);
    if(option){
      unsigned int cformat = atoi(cf->value.s);
      p = COAP_OPT_VALUE(option);

      unsigned int opt_value = coap_decode_var_bytes(p, COAP_OPT_LENGTH(option));
      debug("decoded content-format = %u\n", opt_value);
     
      if(cformat != opt_value){
	debug("accepted content-format %d differs from topic ct=%d!\n",opt_value, cformat);
	response->hdr->code = COAP_RESPONSE_CODE(415);//Unsupported ct
	return;
      }
    }
    else {
      debug("no accepted content-formats option in GET message!\n");
      //assume any ct is good for the client...
      //automatic unsubscription messages by coap-client do not contain cf option, either...
      //response->hdr->code = COAP_RESPONSE_CODE(415);//Unsupported ct
      //return;
    }
  }

  if(request != NULL){
    //topic is part of uri option (NOT if observable notification; request=0 then)
    coap_hash_request_uri(request, urikey);
  }
  else {
    //observable notification, get the urikey...
    memcpy(&urikey, resource->key, 4);      
  }
  debug("urikey=0x%x%x%x%x\n", urikey[0],urikey[1],urikey[2], urikey[3]);


  HASH_FIND_INT(topic_values, urikey, tv);
  if(tv != NULL){
   
    //check for observe option
    if (request != NULL &&
	coap_check_option(request, COAP_OPTION_OBSERVE, &opt_iter)) {
      // check the OBSERVE option value: 0 = subscribe, 1 = unsubscribe
      option = coap_check_option(request, COAP_OPTION_OBSERVE, &opt_iter);
      if(option){
	//MiM 30.9.2015:
	observe_action = coap_decode_var_bytes(coap_opt_value(option), coap_opt_length(option));
	debug("observe_action = %u\n", observe_action);

	p = COAP_OPT_VALUE(option);
       
	if(observe_action==0 || coap_opt_length(option) == 0 || *p == '0'){
	  debug("OBSERVE value == 0, adding subscriber\n");
	  subscription = coap_add_observer(resource, local_interface, peer, token);
	  if (subscription) {
#ifdef COAP_STATS
  n_subscribers++;
#endif
	    
	    debug("adding OBSERVE option to response\n");
	    coap_add_option(response, COAP_OPTION_OBSERVE, coap_encode_var_bytes(buf, ctx->observe), buf);
	  }
        }
	else if(*p == '1' || observe_action == 1){
	  debug("OBSERVE option == 1, unsubscribing\n");
	  //unsubscribe the topic by deleting the observer
	  coap_delete_observer(resource, peer, token);
#ifdef COAP_STATS
	  n_subscribers--;
#endif
        }
	else{
	  debug("unknown OBSERVE option %u\n", observe_action);
	  //error response BAD REQUEST 4.00
	  response->hdr->code = COAP_RESPONSE_CODE(400);
#ifdef COAP_STATS
	    n_error_responses++;
#endif
	  return;
	}//else
      }//if(option)
    }//if (request != NULL && ....
    else {
      debug("request == NULL or OBSERVE not found\n");
      //request==NULL --> send notification
      //request!=NULL && OBSERVE not found i.e. GET without observe == READ
    }
       
    if (resource->dirty == 1){
      debug("resource->dirty, ctx->observe=%d\n", ctx->observe);
      debug("adding OBSERVE option in notification...\n");
      coap_add_option(response, COAP_OPTION_OBSERVE, 
		      coap_encode_var_bytes(buf, ctx->observe), buf);
     
      
#ifdef COAP_STATS
      n_sent_notifications++;
#endif
    } //resource->dirty...


    //if content:
    if(tv->tvalue){
      
      //Add Content-Format option (MiM 4.1.2015):
      coap_attr_t *cf = coap_find_attr(resource, "ct", 2);
      if(cf){
	unsigned int cformat = atoi(cf->value.s);
	
	debug("adding Content-Format = %u option\n", cformat);
	coap_add_option(response, COAP_OPTION_CONTENT_FORMAT, coap_encode_var_bytes(buf, cformat), buf);
      }
      else {
	debug("topic has no ct!\n");
	//TODO: error response = ?
      }
     
      //add Max-Age option if needed:
      coap_attr_t *maxage = coap_find_attr(resource, "max-age", 7);
      if(maxage){
	debug("adding Max-Age option if not expired...\n");
	struct timeval now;
	gettimeofday(&now, NULL);
	int ma = atoi(maxage->value.s) - now.tv_sec;
	if(ma <= 0){
	  debug("Max-Age expired! returning No Content 2.04\n");
	  response->hdr->code = COAP_RESPONSE_CODE(204); //NO CONTENT
	  return;
	}

	coap_add_option(response, COAP_OPTION_MAXAGE, coap_encode_var_bytes(buf, ma), buf);
      }
      
      //add data value:
      response->hdr->code = COAP_RESPONSE_CODE(205);//CONTENT
      debug("topic value = %s, topic length=%d\n", tv->tvalue, tv->vlen);
      coap_add_data(response, tv->vlen, tv->tvalue);
      
      
    }
    else {  //if no content:
      debug("no topic value\n");
      response->hdr->code = COAP_RESPONSE_CODE(204); //NO CONTENT
    }

   

  }//if(tv != NULL)
  else {
    debug("error: did not find topic value with key 0x%x%x%x%x\n", urikey[0], urikey[1], urikey[2],urikey[3]);
    response->hdr->code = COAP_RESPONSE_CODE(204); //NO CONTENT

    //if Max-Age expired, delete the resource:
    coap_attr_t *maxage = coap_find_attr(resource, "max-age", 7);
    if(maxage){
      struct timeval now;
      gettimeofday(&now, NULL);
      
      if(now.tv_sec > atoi(maxage->value.s)){
	debug("Max-Age expired and no value, deleting resource\n");
	coap_delete_resource(ctx, urikey);
#ifdef COAP_STATS
	n_error_responses++;
	n_topics--;
#endif
	response->hdr->code = COAP_RESPONSE_CODE(404); //NOT FOUND
      }
    }
  }

#ifdef COAP_STATS
pubsub_stats_resource->dirty = 1;
#endif
}

#ifdef COAP_STATS
void hnd_get_ps_stats(coap_context_t  *ctx, struct coap_resource_t *resource,  const coap_endpoint_t *local_interface, coap_address_t *peer, coap_pdu_t *request, str *token, coap_pdu_t *response){
  
  debug("hnd_get_ps_stats\n");

  coap_opt_iterator_t opt_iter;
  coap_opt_t *option;
  unsigned char buf[40];
  coap_subscription_t *subscription;
  unsigned char *p;
  

  //check for observe option
  if (request != NULL &&
      coap_check_option(request, COAP_OPTION_OBSERVE, &opt_iter)) {
    // check the OBSERVE option value: 0 = subscribe, 1 = unsubscribe
    option = coap_check_option(request, COAP_OPTION_OBSERVE, &opt_iter);
    if(option){
      p = COAP_OPT_VALUE(option);
      
      if(coap_opt_length(option) == 0 || *p == '0'){
	debug("OBSERVE value == 0, adding subscriber\n");
	subscription = coap_add_observer(resource, local_interface, peer, token);
	n_subscribers++;
	if (subscription) {
	  debug("adding OBSERVE option to response\n");
	  coap_add_option(response, COAP_OPTION_OBSERVE, coap_encode_var_bytes(buf, ctx->observe), buf);
	}
      }//p==0
      else if(*p == '1'){
	debug("OBSERVE option == 1, unsubscribing\n");
	//unsubscribe the topic by deleting the observer
	coap_delete_observer(resource, peer, token);
	n_subscribers--; 
      }//p==1
      else{
	debug("unknown OBSERVE option %c\n", p);
	//error response BAD REQUEST 4.00
	response->hdr->code = COAP_RESPONSE_CODE(400);
	n_error_responses++;
	n_sent_messages++;
	return;
      }//else
    }//if(option)
  }//if (request != NULL && ....
  else {
    debug("request == NULL or OBSERVE not found\n");
    //request==NULL --> send notification
    //request!=NULL && OBSERVE not found i.e. GET without observe == READ
  }
 
    
  if (resource->dirty == 1){
    debug("resource->dirty, ctx->observe=%d\n", ctx->observe);
    debug("adding OBSERVE option in notification...\n");
    coap_add_option(response, COAP_OPTION_OBSERVE, 
		    coap_encode_var_bytes(buf, ctx->observe), buf);
    n_sent_notifications ++;
  
  }//if(resource->dirty...
  
  //add Content-Format option if resource has Content-Format attribute:
  coap_attr_t *cf = coap_find_attr(resource, "ct", 2);
  if(cf){
    unsigned int cformat = atoi(cf->value.s);
    debug("adding Content-Format = %u option\n", cformat);
    coap_add_option(response, COAP_OPTION_CONTENT_FORMAT, coap_encode_var_bytes(buf, cformat), buf);
  }
  
  
  
  //add data value:
  response->hdr->code = COAP_RESPONSE_CODE(205);//CONTENT
  
  //create the stats string to be returned
  n_sent_messages ++; //number of all sent messages (including stats and errors)
  n_sent_stats ++; //number of sent statistics messages
  //n_sent_notifications = 0; //number of sent notifications (including stats)
  //n_topics = 0; //number of pubsub topics under /ps (including statistics)
  //n_subscribers = 0; //number of subscribers
  //n_publishments = 0; //number of received pub messages (PUT)
  //n_error_responses = 0; //number of sent error messages (including stats)
  
  char returnstring[100] = {0};
  int len = sprintf(returnstring, "%d:%d:%d:%d:%d:%d:%d", n_sent_messages, n_sent_stats, n_sent_notifications, n_topics, n_subscribers, n_publishments, n_error_responses);
  //debug("len, returnstring = %d, %s\n", len, returnstring);


  coap_add_data(response, len, returnstring);
  

}
#endif

void 
hnd_delete_ps(coap_context_t  *ctx, struct coap_resource_t *resource,  const coap_endpoint_t *local_interface,
	     coap_address_t *peer, coap_pdu_t *request, str *token,
	     coap_pdu_t *response) {

  debug("hnd_delete_ps()\n");

  //topic is part of uri option
  coap_key_t urikey;
  coap_hash_request_uri(request, urikey);

  //remove the topic
  int deleted = coap_delete_resource(ctx, resource->key);

#ifdef COAP_STATS
  n_topics--;
  n_sent_messages++;
#endif

  //delete also the topic value:
  struct topic_value *tv = NULL;
  HASH_FIND_INT(topic_values, urikey, tv);
  if(tv != NULL){
    HASH_DEL(topic_values, tv);
    debug("topic value deleted from topic_values\n");
  }
  else{
    debug("topic value not found in topic_values\n");
  }
  debug("HASH size = %d\n", HASH_COUNT(topic_values)); 

  if(deleted == 1){
    //send response DELETED
    debug("topic deleted!\n");
    response->hdr->code = COAP_RESPONSE_CODE(202);
  }
  else if(deleted == 0){
    debug("error deleting topic\n");
    response->hdr->code = COAP_RESPONSE_CODE(404);
#ifdef COAP_STATS
  n_error_responses++;
#endif
  }
  else {
    debug("unknown error deleting topic\n");
    response->hdr->code = COAP_RESPONSE_CODE(500);//server internal error
#ifdef COAP_STATS
  n_error_responses++;
#endif
  }
#ifdef COAP_STATS
  pubsub_stats_resource->dirty = 1;
#endif
}


void 
hnd_post_ps(coap_context_t  *ctx, struct coap_resource_t *resource,  const coap_endpoint_t *local_interface,
	     coap_address_t *peer, coap_pdu_t *request, str *token,
	     coap_pdu_t *response) {

  debug("hnd_post_ps()\nURI length=%d and value=%s\n",resource->uri.length, resource->uri.s);

  size_t size;
  unsigned char *data;
  unsigned char topic[20] = {0};

  coap_opt_iterator_t opt_iter;
  coap_opt_t *option;
  unsigned char *p;


  //read the "topic" from the payload (CoRE link-format including also ct!)
  if(coap_get_data(request, &size, &data)){
    memcpy(topic, data, size);
    debug("topic;ct = %s\n", topic);
  }
  else {
    debug("error reading data from request\n");
    //send error response 4.00 Bad Request
    debug("error code = %s\n", coap_response_phrase(COAP_RESPONSE_CODE(400)));
    response->hdr->code = COAP_RESPONSE_CODE(400);
#ifdef COAP_STATS
    n_error_responses++;
    n_sent_messages++;
#endif
    return;
  }
  
  //Parse the topic (MiM 16.12.2015)
  //it should be in the following format:
  // <topic>;ct=0 [;rt="sensor";title="sensor topic value"]
  //TODO: optional attributes in [] not yet supported!
  //TODO: multiple content types not yet supported! (comma separation)
  int topic_start = -1;
  int topic_end = -1; 
  int topic_length = -1;
  int attr_length = -1;
  
  unsigned char *content_type;
  int ct = 0;
  for(int i=0;i<size;i++){
    if(topic[i]=='<')
      topic_start = i;
    else if(topic[i]=='>')
      topic_end = i; 
  }
  if(topic_start<topic_end && topic_start != -1 && topic_end != -1){
    topic_length = topic_end - topic_start -1;
    attr_length = size - topic_end -5;
    //debug("attr_length=%d\n", attr_length);
    content_type = (unsigned char*)calloc(1, attr_length+1);
    if(content_type == NULL){
      //allocation failed
      response->hdr->code = COAP_RESPONSE_CODE(500);//Internal Server Error
      return;
    }
    memcpy(content_type, &topic[topic_end+5], attr_length);
    ct = atoi(content_type);
    debug("ct=%d\n", ct);
  }
  else {
    debug("error parsing topic: length=%d, start=%d, end=%d\n", topic_length, topic_start, topic_end);
    // return an error code
    response->hdr->code = COAP_RESPONSE_CODE(400);//BAD REQUEST
    return;
  }

  //Check that the current resource has ct=40; otherwise creating (sub)topic is not allowed
  coap_attr_t *cf = coap_find_attr(resource, "ct", 2);
  if(cf != NULL){
    unsigned int cformat = atoi(cf->value.s);
    if(cformat != 40){
      debug("resource's content-format = %u does not allow creating (sub)topics\n", cformat);
      response->hdr->code = COAP_RESPONSE_CODE(403);//Forbidden
      return;
    }
    else if(cf == NULL){
      debug("resource does not have ct attribute!\n");
      /*
      unsigned char bufber[50];
      coap_print_link(resource, bufber, 50, 0);
      debug("resource link=%s\n", bufber);       
      */		
      response->hdr->code = COAP_RESPONSE_CODE(405);//Method Not Allowed
      return;
    }
  }

  //Check the max-age of this resource
  coap_attr_t *maxage = coap_find_attr(resource, "max-age", 7);
  if(maxage != NULL){
    struct timeval now;
    gettimeofday(&now, NULL);

    if(now.tv_sec > atoi(maxage->value.s)){
      debug("resource has expired its Max-Age!\n");
      response->hdr->code = COAP_RESPONSE_CODE(404);//Not Found

      //also delete this resource
      coap_delete_resource(ctx, resource->key); 
      debug("deleted the resource\n");

      return;
    }
    else {
      //delete this resource's max-age attribute
      LL_DELETE(resource->link_attr, maxage);
      coap_delete_attr(maxage);
      debug("max-age attribute deleted\n");
    }
  }
   

  //check the current resources whether this topic already exists?
  coap_key_t pskey;
  //unsigned char *path = (unsigned char*)calloc(1, topic_length+4);//MiM 16.12.2015
  unsigned char *path = (unsigned char*)calloc(1, topic_length+resource->uri.length+2); 

  memcpy(&path[0], resource->uri.s, resource->uri.length);
  path[resource->uri.length] = '/';
  memcpy(&path[resource->uri.length+1], &topic[topic_start+1], topic_length);
  debug("path=%s, length=%d\n", path, topic_length+resource->uri.length+1);
  /*
  path[0] = 'p';
  path[1] = 's';
  path[2] = '/';
  
  memcpy(&path[3], &topic[topic_start+1], topic_length); //MiM 16.12.2015
  //debug("path=%s, length=%d\n", path, topic_length+3);
  */

  //coap_hash_path(path, topic_length+3, pskey); 
  coap_hash_path(path, topic_length+resource->uri.length+1, pskey);

  if(coap_get_resource_from_key(ctx, pskey) == NULL){
    //create the resource

    coap_resource_t *newr = coap_resource_init((unsigned char *)path, /*topic_length+3*/topic_length+resource->uri.length+1, 0);

    if(newr == NULL){
      debug("error creating new resource\n");
      response->hdr->code = COAP_RESPONSE_CODE(500);//Internal Server Error
      return;
    }
    
    coap_register_handler(newr, COAP_REQUEST_POST, hnd_post_ps);
    coap_register_handler(newr, COAP_REQUEST_PUT, hnd_put_ps);
    coap_register_handler(newr, COAP_REQUEST_GET, hnd_get_ps);
    coap_register_handler(newr, COAP_REQUEST_DELETE, hnd_delete_ps);

    coap_add_attr(newr, "ct", 2, content_type, attr_length, /*COAP_ATTR_FLAGS_RELEASE_NAME |*/ COAP_ATTR_FLAGS_RELEASE_VALUE ); //MiM 16.12.2015

    newr->observable = 1; //make it observable so that clients can subscribe it

    //Check Max-Age:
    option = coap_check_option(request, COAP_OPTION_MAXAGE, &opt_iter);
    if(option){
      p = COAP_OPT_VALUE(option);
      unsigned int option_length = coap_opt_length(option);
      unsigned int max_age = 0;
      char pvalue[option_length+1];
      memcpy(pvalue, p, option_length);
      pvalue[option_length] = '\0';
      max_age = atoi(pvalue);
      debug("MAX_AGE option length = %d and value = %u\n", option_length, max_age);


      /* calculate current time and expiration time */
      struct timeval now;
      gettimeofday(&now, NULL);
      now.tv_sec += max_age;
      char *expiration = calloc(1, 32);
      if(expiration == NULL){
	debug("error in calloc\n");
	response->hdr->code = COAP_RESPONSE_CODE(500);//Internal Server Error
	return;
      }
      sprintf(expiration, "%d", now.tv_sec);
      char *attrname = calloc(1, 8);
      sprintf(attrname, "%s", "max-age");
      

      coap_attr_t *a = NULL;
      a = coap_add_attr(newr, attrname/*"max-age"*/, 7, expiration, 32, COAP_ATTR_FLAGS_RELEASE_NAME | COAP_ATTR_FLAGS_RELEASE_VALUE); //flags == 3
     
    }

    coap_add_resource(ctx, newr);
    
    debug("newr resource key = %d, resource uri=%s, uri.length=%d\n", newr->key, newr->uri.s, newr->uri.length);

    //TODO: add new resource's link to its parent (==this resource) as a value
 
   
    size_t len = 50;
    size_t offset = 0;
    unsigned char *buffer = (unsigned char*)calloc(1, len);

    coap_print_link(newr, buffer, &len, &offset);
    debug("newr link=%s, len=%d, offset=%d\n", buffer, len, offset);       
      
    //get the current value and append new resource to it:
    struct topic_value *tv = NULL;
    HASH_FIND_INT(topic_values, resource->key, tv);
    if(tv == NULL){
      //first subtopic to this resource
      debug("first subtopic value to this resource, creating new\n");
    }
    else {
      //this resource already has subtopic(s)
      debug("resource already has a subtopic value, appending\n");
    }

    //TODO: CHECK THE MAXAGE OF THIS RESOURCE! IF expired, can not create subtopic!?

    //send response 2.01 Created
    debug("response code = %s\n", coap_response_phrase(COAP_RESPONSE_CODE(201)));
    response->hdr->code = COAP_RESPONSE_CODE(201);

    /*
    //add Location-Path options:
    int opt_size =  coap_add_option(response, COAP_OPTION_LOCATION_PATH, 2, "ps");
    opt_size = coap_add_option(response, COAP_OPTION_LOCATION_PATH, topic_length, &path[3]); 
    */
    
    int start = 0;
    int opt_size;
    for(int i=0;i<topic_length+resource->uri.length+1;i++){
      if(path[i] == '/'){
	opt_size = coap_add_option(response, COAP_OPTION_LOCATION_PATH, i-start, &path[start]); 
	start = i+1;
	debug("added location-path option of size %d\n", opt_size);
      }
    } 
    opt_size = coap_add_option(response, COAP_OPTION_LOCATION_PATH, topic_length+resource->uri.length+1-start, &path[start]); 
    debug("added location-path option of size %d\n", opt_size);
    
#ifdef COAP_STATS
    n_sent_messages++;
    n_topics++;
#endif

  }
  else { 
    
    //send error response 4.03 Forbidden
    debug("error code = %s\n", coap_response_phrase(COAP_RESPONSE_CODE(403)));
    response->hdr->code = COAP_RESPONSE_CODE(403);
#ifdef COAP_STATS
  n_error_responses++;
  n_sent_messages++;
#endif
  }


}



// end of CoAP pub/sub handler functions by MiM /////////////////




#ifndef WITHOUT_ASYNC
static void
hnd_get_async(coap_context_t *ctx,
              struct coap_resource_t *resource UNUSED_PARAM,
              const coap_endpoint_t *local_interface UNUSED_PARAM,
              coap_address_t *peer,
              coap_pdu_t *request,
              str *token UNUSED_PARAM,
              coap_pdu_t *response) {
  coap_opt_iterator_t opt_iter;
  coap_opt_t *option;
  unsigned long delay = 5;
  size_t size;

  if (async) {
    if (async->id != request->hdr->id) {
      coap_opt_filter_t f;
      coap_option_filter_clear(f);
      response->hdr->code = COAP_RESPONSE_CODE(503);
    }
    return;
  }

  option = coap_check_option(request, COAP_OPTION_URI_QUERY, &opt_iter);
  if (option) {
    unsigned char *p = COAP_OPT_VALUE(option);

    delay = 0;
    for (size = COAP_OPT_LENGTH(option); size; --size, ++p)
      delay = delay * 10 + (*p - '0');
  }

  async = coap_register_async(ctx,
                              peer,
                              request,
                              COAP_ASYNC_SEPARATE | COAP_ASYNC_CONFIRM,
                              (void *)(COAP_TICKS_PER_SECOND * delay));
}

static void
check_async(coap_context_t *ctx,
            const coap_endpoint_t *local_if,
            coap_tick_t now) {
  coap_pdu_t *response;
  coap_async_state_t *tmp;

  size_t size = sizeof(coap_hdr_t) + 13;

  if (!async || now < async->created + (unsigned long)async->appdata)
    return;

  response = coap_pdu_init(async->flags & COAP_ASYNC_CONFIRM
             ? COAP_MESSAGE_CON
             : COAP_MESSAGE_NON,
             COAP_RESPONSE_CODE(205), 0, size);
  if (!response) {
    debug("check_async: insufficient memory, we'll try later\n");
    async->appdata =
      (void *)((unsigned long)async->appdata + 15 * COAP_TICKS_PER_SECOND);
    return;
  }

  response->hdr->id = coap_new_message_id(ctx);

  if (async->tokenlen)
    coap_add_token(response, async->tokenlen, async->token);

  coap_add_data(response, 4, (unsigned char *)"done");

  if (coap_send(ctx, local_if, &async->peer, response) == COAP_INVALID_TID) {
    debug("check_async: cannot send response for message %d\n",
    response->hdr->id);
  }
  coap_delete_pdu(response);
  coap_remove_async(ctx, async->id, &tmp);
  coap_free_async(async);
  async = NULL;
}
#endif /* WITHOUT_ASYNC */

static void
init_resources(coap_context_t *ctx) {
  coap_resource_t *r;

  r = coap_resource_init(NULL, 0, 0);
  coap_register_handler(r, COAP_REQUEST_GET, hnd_get_index);

  coap_add_attr(r, (unsigned char *)"ct", 2, (unsigned char *)"0", 1, 0);
  coap_add_attr(r, (unsigned char *)"title", 5, (unsigned char *)"\"General Info\"", 14, 0);
  coap_add_resource(ctx, r);

  /* store clock base to use in /time */
  my_clock_base = clock_offset;

  r = coap_resource_init((unsigned char *)"time", 4, COAP_RESOURCE_FLAGS_NOTIFY_NON);//CON vs NON
  coap_register_handler(r, COAP_REQUEST_GET, hnd_get_time);
  coap_register_handler(r, COAP_REQUEST_PUT, hnd_put_time);
  coap_register_handler(r, COAP_REQUEST_DELETE, hnd_delete_time);

  coap_add_attr(r, (unsigned char *)"ct", 2, (unsigned char *)"0", 1, 0);
  coap_add_attr(r, (unsigned char *)"title", 5, (unsigned char *)"\"Internal Clock\"", 16, 0);
  coap_add_attr(r, (unsigned char *)"rt", 2, (unsigned char *)"\"Ticks\"", 7, 0);
  r->observable = 1;
  coap_add_attr(r, (unsigned char *)"if", 2, (unsigned char *)"\"clock\"", 7, 0);

  coap_add_resource(ctx, r);
  time_resource = r;


  //MiM: CoAP pub/sub //////////////////////////////////////
  coap_resource_t *psr; //pub-sub resource

  psr = coap_resource_init((unsigned char *)"ps", 2, COAP_RESOURCE_FLAGS_NOTIFY_CON); //The last flag determines whether the observe notifications are CON or NON type messages (MiM); PUT message type determines this for pubsub...
  coap_register_handler(psr, COAP_REQUEST_POST, hnd_post_ps);
  coap_register_handler(psr, COAP_REQUEST_PUT, hnd_put_ps);
  coap_register_handler(psr, COAP_REQUEST_GET, hnd_get_ps);
  coap_register_handler(psr, COAP_REQUEST_DELETE, hnd_delete_ps);

  coap_add_attr(psr, "rt", 2, "core.ps", 7, 0);
  coap_add_attr(psr, (unsigned char *)"ct", 2, (unsigned char *)"40", 2, 0); //content-type=40=app/link-format
  coap_add_attr(psr, (unsigned char *)"title", 5, (unsigned char *)"\"CoAP pubsub broker\"", 20, 0);


  coap_add_resource(ctx, psr);
  pubsub_resource = psr;


#ifdef COAP_STATS
  //pub sub statistics resource (MiM 19.11.2015), not part of standard draft
  coap_resource_t *ps_stats; //pub-sub statistics resource

  ps_stats = coap_resource_init((unsigned char *)"ps/stats", 8, COAP_RESOURCE_FLAGS_NOTIFY_NON); //The last flag determines whether the observe notifications are CON or NON type messages (MiM)
  
  coap_register_handler(ps_stats, COAP_REQUEST_GET, hnd_get_ps_stats);
 
  coap_add_attr(ps_stats, (unsigned char *)"ct", 2, (unsigned char *)"0", 1, 0);

  ps_stats->observable = 1;

  n_topics++;

  coap_add_resource(ctx, ps_stats);
  pubsub_stats_resource = ps_stats;
  
#endif //COAP_STATS

  //end of CoAP pub/sub by MiM ///////////////////////////////


#ifndef WITHOUT_ASYNC
  r = coap_resource_init((unsigned char *)"async", 5, 0);
  coap_register_handler(r, COAP_REQUEST_GET, hnd_get_async);

  coap_add_attr(r, (unsigned char *)"ct", 2, (unsigned char *)"0", 1, 0);
  coap_add_resource(ctx, r);
#endif /* WITHOUT_ASYNC */
}

static void
usage( const char *program, const char *version) {
  const char *p;

  p = strrchr( program, '/' );
  if ( p )
    program = ++p;

  fprintf( stderr, "%s v%s -- CoAP publish-subscribe broker\n"
     "(c) 2015-2016 Mikko Majanen <mikko.majanen@vtt.fi>\n\n"
     "usage: %s [-A address] [-p port]\n\n"
     "\t-A address\tinterface address to bind to\n"
     "\t-g group\tjoin the given multicast group\n"
     "\t-p port\t\tlisten on specified port\n"
     "\t-v num\t\tverbosity level (default: 3)\n",
     program, version, program );
}

static coap_context_t *
get_context(const char *node, const char *port) {
  coap_context_t *ctx = NULL;
  int s;
  struct addrinfo hints;
  struct addrinfo *result, *rp;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
  hints.ai_socktype = SOCK_DGRAM; /* Coap uses UDP */
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

  s = getaddrinfo(node, port, &hints, &result);
  if ( s != 0 ) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
    return NULL;
  }

  /* iterate through results until success */
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    coap_address_t addr;

    if (rp->ai_addrlen <= sizeof(addr.addr)) {
      coap_address_init(&addr);
      addr.size = rp->ai_addrlen;
      memcpy(&addr.addr, rp->ai_addr, rp->ai_addrlen);

      ctx = coap_new_context(&addr);
      if (ctx) {
        /* TODO: output address:port for successful binding */
        goto finish;
      }
    }
  }

  fprintf(stderr, "no context available for interface '%s'\n", node);

  finish:
  freeaddrinfo(result);
  return ctx;
}

static int
join(coap_context_t *ctx, char *group_name){
  struct ipv6_mreq mreq;
  struct addrinfo   *reslocal = NULL, *resmulti = NULL, hints, *ainfo;
  int result = -1;

  /* we have to resolve the link-local interface to get the interface id */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;

  result = getaddrinfo("::", NULL, &hints, &reslocal);
  if (result < 0) {
    fprintf(stderr, "join: cannot resolve link-local interface: %s\n",
            gai_strerror(result));
    goto finish;
  }

  /* get the first suitable interface identifier */
  for (ainfo = reslocal; ainfo != NULL; ainfo = ainfo->ai_next) {
    if (ainfo->ai_family == AF_INET6) {
      mreq.ipv6mr_interface =
                ((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_scope_id;
      break;
    }
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_DGRAM;

  /* resolve the multicast group address */
  result = getaddrinfo(group_name, NULL, &hints, &resmulti);

  if (result < 0) {
    fprintf(stderr, "join: cannot resolve multicast address: %s\n",
            gai_strerror(result));
    goto finish;
  }

  for (ainfo = resmulti; ainfo != NULL; ainfo = ainfo->ai_next) {
    if (ainfo->ai_family == AF_INET6) {
      mreq.ipv6mr_multiaddr =
                ((struct sockaddr_in6 *)ainfo->ai_addr)->sin6_addr;
      break;
    }
  }

  result = setsockopt(ctx->sockfd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
          (char *)&mreq, sizeof(mreq));
  if (result < 0)
    perror("join: setsockopt");

 finish:
  freeaddrinfo(resmulti);
  freeaddrinfo(reslocal);

  return result;
}

int
main(int argc, char **argv) {
  coap_context_t  *ctx;
  char *group = NULL;
  fd_set readfds;
  struct timeval tv, *timeout;
  int result;
  coap_tick_t now;
  coap_queue_t *nextpdu;
  char addr_str[NI_MAXHOST] = "::";
  char port_str[NI_MAXSERV] = "5683";
  int opt;
  coap_log_t log_level = LOG_DEBUG;//LOG_WARNING;

  clock_offset = time(NULL);

  while ((opt = getopt(argc, argv, "A:g:p:v:")) != -1) {
    switch (opt) {
    case 'A' :
      strncpy(addr_str, optarg, NI_MAXHOST-1);
      addr_str[NI_MAXHOST - 1] = '\0';
      break;
    case 'g' :
      group = optarg;
      break;
    case 'p' :
      strncpy(port_str, optarg, NI_MAXSERV-1);
      port_str[NI_MAXSERV - 1] = '\0';
      break;
    case 'v' :
      log_level = strtol(optarg, NULL, 10);
      break;
    default:
      usage( argv[0], PACKAGE_VERSION );
      exit( 1 );
    }
  }

  coap_set_log_level(log_level);

  ctx = get_context(addr_str, port_str);
  if (!ctx)
    return -1;

  init_resources(ctx);

  /* join multicast group if requested at command line */
  if (group)
    join(ctx, group);

  signal(SIGINT, handle_sigint);

  while ( !quit ) {
    FD_ZERO(&readfds);
    FD_SET( ctx->sockfd, &readfds );

    nextpdu = coap_peek_next( ctx );

    coap_ticks(&now);
    while (nextpdu && nextpdu->t <= now - ctx->sendqueue_basetime) {
      coap_retransmit( ctx, coap_pop_next( ctx ) );
      nextpdu = coap_peek_next( ctx );
    }

    if ( nextpdu && nextpdu->t <= COAP_RESOURCE_CHECK_TIME ) {
      /* set timeout if there is a pdu to send before our automatic timeout occurs */
      tv.tv_usec = ((nextpdu->t) % COAP_TICKS_PER_SECOND) * 1000000 / COAP_TICKS_PER_SECOND;
      tv.tv_sec = (nextpdu->t) / COAP_TICKS_PER_SECOND;
      timeout = &tv;
    } else {
      tv.tv_usec = 0;
      tv.tv_sec = COAP_RESOURCE_CHECK_TIME;
      timeout = &tv;
    }
    result = select( FD_SETSIZE, &readfds, 0, 0, timeout );

    if ( result < 0 ) {         /* error */
      if (errno != EINTR)
        perror("select");
    } else if ( result > 0 ) {  /* read from socket */
      if ( FD_ISSET( ctx->sockfd, &readfds ) ) {
        coap_read( ctx );       /* read received data */
        /* coap_dispatch( ctx );  /\* and dispatch PDUs from receivequeue *\/ */
      }
    } else {      /* timeout */
      if (time_resource) {
        time_resource->dirty = 1;
      }
    }

#ifndef WITHOUT_ASYNC
    /* check if we have to send asynchronous responses */
    check_async(ctx, ctx->endpoint, now);
#endif /* WITHOUT_ASYNC */

#ifndef WITHOUT_OBSERVE
    /* check if we have to send observe notifications */
    coap_check_notify(ctx);
#endif /* WITHOUT_OBSERVE */
  }

  coap_free_context(ctx);

  return 0;
}
