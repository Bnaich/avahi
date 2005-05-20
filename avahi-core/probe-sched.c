/* $Id$ */

/***
  This file is part of avahi.
 
  avahi is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  avahi is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
  Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with avahi; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "probe-sched.h"
#include "util.h"

#define AVAHI_PROBE_DEFER_MSEC 70

typedef struct AvahiProbeJob AvahiProbeJob;

struct AvahiProbeJob {
    AvahiProbeScheduler *scheduler;
    AvahiTimeEvent *time_event;
    
    gboolean chosen; /* Use for packet assembling */
    GTimeVal delivery;

    AvahiRecord *record;
    
    AVAHI_LLIST_FIELDS(AvahiProbeJob, jobs);
};

struct AvahiProbeScheduler {
    AvahiInterface *interface;
    AvahiTimeEventQueue *time_event_queue;

    AVAHI_LLIST_HEAD(AvahiProbeJob, jobs);
};

static AvahiProbeJob* job_new(AvahiProbeScheduler *s, AvahiRecord *record) {
    AvahiProbeJob *pj;
    
    g_assert(s);
    g_assert(record);

    pj = g_new(AvahiProbeJob, 1);
    pj->scheduler = s;
    pj->record = avahi_record_ref(record);
    pj->time_event = NULL;
    pj->chosen = FALSE;
    
    AVAHI_LLIST_PREPEND(AvahiProbeJob, jobs, s->jobs, pj);

    return pj;
}

static void job_free(AvahiProbeScheduler *s, AvahiProbeJob *pj) {
    g_assert(pj);

    if (pj->time_event)
        avahi_time_event_queue_remove(s->time_event_queue, pj->time_event);

    AVAHI_LLIST_REMOVE(AvahiProbeJob, jobs, s->jobs, pj);

    avahi_record_unref(pj->record);
    g_free(pj);
}


AvahiProbeScheduler *avahi_probe_scheduler_new(AvahiInterface *i) {
    AvahiProbeScheduler *s;

    g_assert(i);

    s = g_new(AvahiProbeScheduler, 1);
    s->interface = i;
    s->time_event_queue = i->monitor->server->time_event_queue;

    AVAHI_LLIST_HEAD_INIT(AvahiProbeJob, s->jobs);
    
    return s;
}

void avahi_probe_scheduler_free(AvahiProbeScheduler *s) {
    g_assert(s);

    avahi_probe_scheduler_clear(s);
    g_free(s);
}

void avahi_probe_scheduler_clear(AvahiProbeScheduler *s) {
    g_assert(s);
    
    while (s->jobs)
        job_free(s, s->jobs);
}
 
static gboolean packet_add_probe_query(AvahiProbeScheduler *s, AvahiDnsPacket *p, AvahiProbeJob *pj) {
    guint size;
    AvahiKey *k;
    gboolean b;

    g_assert(s);
    g_assert(p);
    g_assert(pj);

    g_assert(!pj->chosen);
    
    /* Estimate the size for this record */
    size =
        avahi_key_get_estimate_size(pj->record->key) +
        avahi_record_get_estimate_size(pj->record);

    /* Too large */
    if (size > avahi_dns_packet_space(p))
        return FALSE;

    /* Create the probe query */
    k = avahi_key_new(pj->record->key->name, pj->record->key->class, AVAHI_DNS_TYPE_ANY);
    b = !!avahi_dns_packet_append_key(p, k, FALSE);
    g_assert(b);

    /* Mark this job for addition to the packet */
    pj->chosen = TRUE;

    /* Scan for more jobs whith matching key pattern */
    for (pj = s->jobs; pj; pj = pj->jobs_next) {
        if (pj->chosen)
            continue;

        /* Does the record match the probe? */
        if (k->class != pj->record->key->class || !avahi_domain_equal(k->name, pj->record->key->name))
            continue;
        
        /* This job wouldn't fit in */
        if (avahi_record_get_estimate_size(pj->record) > avahi_dns_packet_space(p))
            break;

        /* Mark this job for addition to the packet */
        pj->chosen = TRUE;
    }

    avahi_key_unref(k);
            
    return TRUE;
}

static void elapse_callback(AvahiTimeEvent *e, gpointer data) {
    AvahiProbeJob *pj = data, *next;
    AvahiProbeScheduler *s;
    AvahiDnsPacket *p;
    guint n;

    g_assert(pj);
    s = pj->scheduler;

    p = avahi_dns_packet_new_query(s->interface->hardware->mtu);
    n = 1;
    
    /* Add the import probe */
    if (!packet_add_probe_query(s, p, pj)) {
        guint size;
        AvahiKey *k;
        gboolean b;

        avahi_dns_packet_free(p);

        /* The probe didn't fit in the package, so let's allocate a larger one */

        size =
            avahi_key_get_estimate_size(pj->record->key) +
            avahi_record_get_estimate_size(pj->record) +
            AVAHI_DNS_PACKET_HEADER_SIZE;
        
        if (size > AVAHI_DNS_PACKET_MAX_SIZE)
            size = AVAHI_DNS_PACKET_MAX_SIZE;
        
        p = avahi_dns_packet_new_query(size);

        k = avahi_key_new(pj->record->key->name, pj->record->key->class, AVAHI_DNS_TYPE_ANY);
        b = avahi_dns_packet_append_key(p, k, FALSE) && avahi_dns_packet_append_record(p, pj->record, FALSE, 0);
        avahi_key_unref(k);

        if (b) {
            avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_NSCOUNT, 1);
            avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_QDCOUNT, 1);
            avahi_interface_send_packet(s->interface, p);
        } else
            g_warning("Probe record too large, cannot send");   
        
        avahi_dns_packet_free(p);
        job_free(s, pj);

        return;
    }

    /* Try to fill up packet with more probes, if available */
    for (pj = s->jobs; pj; pj = pj->jobs_next) {

        if (pj->chosen)
            continue;
        
        if (!packet_add_probe_query(s, p, pj))
            break;
        
        n++;
    }

    avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_QDCOUNT, n);

    n = 0;

    /* Now add the chosen records to the authorative section */
    for (pj = s->jobs; pj; pj = next) {

        next = pj->jobs_next;

        if (!pj->chosen)
            continue;

        if (!avahi_dns_packet_append_record(p, pj->record, FALSE, 0)) {
            g_warning("Bad probe size estimate!");

            /* Unmark all following jobs */
            for (; pj; pj = pj->jobs_next)
                pj->chosen = FALSE;
            
            break;
        }

        job_free(s, pj);
        
        n ++;
    }
    
    avahi_dns_packet_set_field(p, AVAHI_DNS_FIELD_NSCOUNT, n);

    /* Send it now */
    avahi_interface_send_packet(s->interface, p);
    avahi_dns_packet_free(p);
}

gboolean avahi_probe_scheduler_post(AvahiProbeScheduler *s, AvahiRecord *record, gboolean immediately) {
    AvahiProbeJob *pj;
    GTimeVal tv;
    
    g_assert(s);
    g_assert(record);
    g_assert(!avahi_key_is_pattern(record->key));
    
    avahi_elapse_time(&tv, immediately ? 0 : AVAHI_PROBE_DEFER_MSEC, 0);

    /* Create a new job and schedule it */
    pj = job_new(s, record);
    pj->delivery = tv;
    pj->time_event = avahi_time_event_queue_add(s->time_event_queue, &pj->delivery, elapse_callback, pj);

/*     g_message("Accepted new probe job."); */

    return TRUE;
}
