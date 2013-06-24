/*
	Copyright (C) 2013 University of Lugano

	This file is part of LibPaxos.

	LibPaxos is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Libpaxos is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with LibPaxos.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "proposer.h"
#include "carray.h"
#include "quorum.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

struct instance
{
	iid_t iid;
	ballot_t ballot;
	ballot_t value_ballot;
	paxos_msg* value;
	int closed;
	struct quorum quorum;
	struct timeval created_at;
};

struct proposer 
{
	int id;
	struct carray* values;
	iid_t next_prepare_iid;
	struct carray* prepare_instances; /* Instances waiting for prepare acks */
	// TODO accept_instances should be a hash table
	struct carray* accept_instances; /* Instance waiting for accept acks */
};

struct timeout_iterator
{
	int pi, ai;
	struct timeval timeout;
	struct proposer* proposer;
};

static struct instance* instance_new(iid_t iid, ballot_t ballot);
static void instance_free(struct instance* inst);
static struct instance* instance_find(struct carray* c, iid_t iid);
static int instance_match(void* arg, void* item);
static struct carray* instance_remove(struct carray* c, struct instance* inst);
static int instance_has_timedout(struct instance* inst, struct timeval* now);
static paxos_msg* wrap_value(char* value, size_t size);
static void prepare_preempt(struct proposer* p, struct instance* inst, prepare_req* out);
static ballot_t proposer_next_ballot(struct proposer* p, ballot_t b);
static int timeval_diff(struct timeval* t1, struct timeval* t2);


struct proposer*
proposer_new(int id)
{
	struct proposer *p;
	int instances = 128;
	p = malloc(sizeof(struct proposer));
	p->id = id;
	p->values = carray_new(instances);
	p->next_prepare_iid = 0;
	p->prepare_instances = carray_new(instances);
	p->accept_instances = carray_new(instances);
	return p;
}

void
proposer_free(struct proposer* p)
{
	int i;
	for (i = 0; i < carray_count(p->values); ++i)
		free(carray_at(p->values, i));
	carray_free(p->values);
	for (i = 0; i < carray_count(p->prepare_instances); ++i)
		instance_free(carray_at(p->prepare_instances, i));
	carray_free(p->prepare_instances);
	for (i = 0; i < carray_count(p->accept_instances); ++i)
		instance_free(carray_at(p->accept_instances, i));
	carray_free(p->accept_instances);
	free(p);
}

struct timeout_iterator*
proposer_timeout_iterator(struct proposer* p)
{
	struct timeout_iterator* iter;
	iter = malloc(sizeof(struct timeout_iterator));
	iter->pi = iter->ai = 0;
	iter->proposer = p;
	gettimeofday(&iter->timeout, NULL);
	return iter;
}

void
proposer_propose(struct proposer* p, char* value, size_t size)
{
	paxos_msg* msg;
	msg = wrap_value(value, size);
	carray_push_back(p->values, msg);
}

int
proposer_prepared_count(struct proposer* p)
{
	return carray_count(p->prepare_instances);
}

void
proposer_prepare(struct proposer* p, prepare_req* out)
{
	struct instance* inst;
	iid_t iid = ++(p->next_prepare_iid);
	inst = instance_new(iid, proposer_next_ballot(p, 0));
	carray_push_back(p->prepare_instances, inst);
	*out = (prepare_req) {inst->iid, inst->ballot};
}

int proposer_receive_prepare_ack(struct proposer* p, prepare_ack* ack,
	prepare_req* out)
{
	struct instance* inst;
	
	inst = instance_find(p->prepare_instances, ack->iid);
	
	if (inst == NULL) {
		LOG(DBG, ("Promise dropped, instance %u not pending\n", ack->iid));
		return 0;
	}
	
	if (ack->ballot < inst->ballot) {
		LOG(DBG, ("Promise dropped, too old\n"));
		return 0;
	}
	
	if (ack->ballot == inst->ballot) {	// preempted?
		
		if (!quorum_add(&inst->quorum, ack->acceptor_id)) {
			LOG(DBG, ("Promise dropped %d, instance %u has a quorum\n",
				ack->acceptor_id, inst->iid));
			return 0;
		}
		
		LOG(DBG, ("Received valid promise from: %d, iid: %u\n",
			ack->acceptor_id, inst->iid));	
		
		if (ack->value_size > 0) {
			LOG(DBG, ("Promise has value\n"));
			if (inst->value == NULL) {
				inst->value_ballot = ack->value_ballot;
				inst->value = wrap_value(ack->value, ack->value_size);
			} else if (ack->value_ballot > inst->value_ballot) {
				carray_push_back(p->values, inst->value);
				inst->value_ballot = ack->value_ballot;
				inst->value = wrap_value(ack->value, ack->value_size);
				LOG(DBG, ("Value in promise saved, removed older value\n"));
			} else if (ack->value_ballot == inst->value_ballot) {
				// TODO this assumes that the QUORUM is 2!
				LOG(DBG, ("Instance %d closed\n", inst->iid));
				inst->closed = 1;	
			} else {
				LOG(DBG, ("Value in promise ignored\n"));
			}
		}
		
		return 0;
		
	} else {
		LOG(DBG, ("Instance %u preempted: ballot %d ack ballot %d\n",
			inst->iid, inst->ballot, ack->ballot));
		prepare_preempt(p, inst, out);
		return 1;
	}
}

accept_req* 
proposer_accept(struct proposer* p)
{
	struct instance* inst;

	// is there a prepared instance?
	while ((inst = carray_front(p->prepare_instances)) != NULL) {
		if (inst->closed)
			instance_free(carray_pop_front(p->prepare_instances));
		else if (!quorum_reached(&inst->quorum))
			return NULL;
		else break;
	}
	
	if (inst == NULL)
		return NULL;
	
	LOG(DBG, ("Trying to accept iid %u\n", inst->iid));
	
	// is there a value?
	if (inst->value == NULL) {
		inst->value = carray_pop_front(p->values);
		if (inst->value == NULL) {
			LOG(DBG, ("No value to accept\n"));
			return NULL;	
		}
		LOG(DBG,("Popped next value\n"));
	} else {
		LOG(DBG, ("Instance has value\n"));
	}
	
	// we have both a prepared instance and a value
	inst = carray_pop_front(p->prepare_instances);
	quorum_init(&inst->quorum, QUORUM);
	carray_push_back(p->accept_instances, inst);
	
	accept_req* req = malloc(sizeof(accept_req) + inst->value->data_size);
	req->iid = inst->iid;
	req->ballot = inst->ballot;
	req->value_size = inst->value->data_size;
	memcpy(req->value, inst->value->data, req->value_size);

	return req;
}

int
proposer_receive_accept_ack(struct proposer* p, accept_ack* ack, prepare_req* out)
{
	struct instance* inst;
	
	inst = instance_find(p->accept_instances, ack->iid);
	
	if (inst == NULL) {
		LOG(DBG, ("Accept ack dropped, iid:%u not pending\n", ack->iid));
		return 0;
	}
	
	if (ack->ballot == inst->ballot) {
		assert(ack->value_ballot == inst->ballot);
		if (!quorum_add(&inst->quorum, ack->acceptor_id)) {
			LOG(DBG, ("Dropping duplicate accept from: %d, iid: %u\n", 
				ack->acceptor_id, inst->iid));
			return 0;
		}
		
		if (quorum_reached(&inst->quorum)) {
			LOG(DBG, ("Quorum reached for instance %u\n", inst->iid));
			p->accept_instances = instance_remove(p->accept_instances, inst);
			instance_free(inst);
		}
		
		return 0;
		
	} else {
		LOG(DBG, ("Instance %u preempted: ballot %d ack ballot %d\n",
			inst->iid, inst->ballot, ack->ballot));
		
		p->accept_instances = instance_remove(p->accept_instances, inst);
		carray_push_front(p->prepare_instances, inst);
		prepare_preempt(p, inst, out);
		return  1; 
	}
}

static struct instance*
next_timedout(struct carray* c, int* i, struct timeval* t)
{
	struct instance* inst;
	while (*i < carray_count(c)) {
		inst = carray_at(c, *i);
		(*i)++;
		// printf("quorum? %d timeout? %d\n", quorum_reached(&inst->quorum),
			// instance_has_timedout(inst, t));
		if (quorum_reached(&inst->quorum))
			continue;
		if (instance_has_timedout(inst, t))
			return inst;
	}
	return NULL;
}

int
timeout_iterator_next(struct timeout_iterator* iter, prepare_req* req)
{
	struct instance* inst;
	struct proposer* p = iter->proposer;

	inst = next_timedout(p->prepare_instances, &iter->pi, &iter->timeout);
	if (inst != NULL) {
		gettimeofday(&inst->created_at, NULL);
		*req = (prepare_req){inst->iid, inst->ballot};
		return 1;
	}
	
	inst = next_timedout(p->accept_instances, &iter->ai, &iter->timeout);
	if (inst != NULL) {
		prepare_preempt(p, inst, req);		
		p->accept_instances = instance_remove(p->accept_instances, inst);
		carray_push_front(p->prepare_instances, inst);
		return 1;
	}
	
	return 0;
}

void
timeout_iterator_free(struct timeout_iterator* iter)
{
	free(iter);
}

static struct instance*
instance_new(iid_t iid, ballot_t ballot)
{
	struct instance* inst;
	inst = malloc(sizeof(struct instance));
	inst->iid = iid;
	inst->ballot = ballot;
	inst->value_ballot = 0;
	inst->value = NULL;
	inst->closed = 0;
	gettimeofday(&inst->created_at, NULL);
	quorum_init(&inst->quorum, QUORUM);
	return inst;
}

static void
instance_free(struct instance* inst)
{
	if (inst->value != NULL)
		free(inst->value);
	free(inst);
}

static int
instance_has_timedout(struct instance* inst, struct timeval* now)
{
	int diff = timeval_diff(&inst->created_at, now);
	return diff >= paxos_config.proposer_instance_timeout;
}

static struct instance*
instance_find(struct carray* c, iid_t iid) 
{
	int i;
	for (i = 0; i < carray_count(c); ++i) {
		struct instance* inst = carray_at(c, i);
		if (inst->iid == iid)
			return carray_at(c, i);
	}
	return NULL;
}

static int
instance_match(void* arg, void* item)
{
	struct instance* a = arg;
	struct instance* b = item;
    return a->iid == b->iid;
}

static struct carray*
instance_remove(struct carray* c, struct instance* inst)
{
	struct carray* tmp;
	tmp = carray_reject(c, instance_match, inst);
	carray_free(c);
	return tmp;
}

static paxos_msg*
wrap_value(char* value, size_t size)
{
	paxos_msg* msg = malloc(size + sizeof(paxos_msg));
	msg->data_size = size;
	msg->type = submit;
	memcpy(msg->data, value, size);
	return msg;
}

void
prepare_preempt(struct proposer* p, struct instance* inst, prepare_req* out)
{
	inst->ballot = proposer_next_ballot(p, inst->ballot);
	quorum_init(&inst->quorum, QUORUM);
	*out = (prepare_req) {inst->iid, inst->ballot};
	gettimeofday(&inst->created_at, NULL);
}

static ballot_t
proposer_next_ballot(struct proposer* p, ballot_t b)
{
	if (b > 0)
		return MAX_N_OF_PROPOSERS + b;
	else
		return MAX_N_OF_PROPOSERS + p->id;
}

/* Returns t2 - t1 in microseconds. */
static int
timeval_diff(struct timeval* t1, struct timeval* t2)
{
    int us;
    us = (t2->tv_sec - t1->tv_sec) * 1e6;
    if (us < 0) return 0;
    us += (t2->tv_usec - t1->tv_usec);
    return us;
}
