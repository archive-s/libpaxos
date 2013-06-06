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


#ifndef _PAXOS_CONFIG_H_
#define _PAXOS_CONFIG_H_

/*** PROTOCOL SETTINGS ***/

/* 
    The maximum number of proposers must be fixed beforehand
    (this is because of unique ballot generation).
    The proposers must be started with different IDs.
    This number MUST be a power of 10.
*/
#define MAX_N_OF_PROPOSERS  10

/* 
    The number of acceptors must be fixed beforehand.
    The acceptors must be started with different IDs.
*/
#define N_OF_ACCEPTORS  3

/* 
    Rule for calculating whether the number of accept_ack messages (phase 2b) 
    is sufficient to declare the instance closed and deliver 
    the corresponding value. i.e.:
    Paxos     -> ((int)(N_OF_ACCEPTORS/2))+1;
    FastPaxos -> 1 + (int)((double)(N_OF_ACCEPTORS*2)/3);
*/

#define QUORUM (((int)(N_OF_ACCEPTORS/2))+1)


/*** ACCEPTORS DB SETTINGS ***/

/*
    Setting for how 'strict' the durability of acceptors should be.
    From weaker and faster to stricter and durable.
    Acceptors use Berkeley DB as a stable storage layer.
    
    No durability on crash:
    0   -> Uses in-memory storage
        Writes to disk if the memory cache is full.
    10  -> Transactional Data Store (write, in-memory logging)
        (DB_LOG_IN_MEMORY)
    Durability despite process crash:
    11  -> Transactional Data Store (write, no-sync on commit)
        (DB_TXN_NOSYNC)
    12 ->Transactional Data Store (write, write-no-sync on commit)
        (DB_TXN_WRITE_NOSYNC)
    Durability despite OS crash:
    13  -> Transactional Data Store (write, sync on commit)
        (default transactional storage)
    20  -> "Manually" call DB->sync before answering requests
        (may corrupt database file on crash)
*/
#define DURABILITY_MODE 0

/*
    This defines where the acceptors create their database files.
    A._DB_PATH is the absolute path of a directory.
    If it does not exist will be created. Unless starting in recovery mode, 
    the content of the directory will deleted.
    A._DB_FNAME is the name for the db file.
    %d is replaced by 'acceptor_id'
    The concatenation of those MUST fit in 512 chars
*/
#define ACCEPTOR_DB_PATH "/tmp/acceptor_%d", acceptor_id
#define ACCEPTOR_DB_FNAME "acc_db_%d.bdb", acceptor_id

/*
    Acceptor's access method on their underlying DB.
    Only DB_BTREE and DB_RECNO are available, other methods
    requires additional configuration and do not fit well.
    Acceptors use Berkeley DB as a stable storage layer.
*/
// #define ACCEPTOR_ACCESS_METHOD DB_BTREE
#define ACCEPTOR_ACCESS_METHOD DB_RECNO


/*** STRUCTURES SETTINGS ***/

/* 
  Size of the in-meory table of instances for the learner.
  MUST be bigger than PROPOSER_PREEXEC_WIN_SIZE (double or more)
*/
#define LEARNER_ARRAY_SIZE 2048

#endif
