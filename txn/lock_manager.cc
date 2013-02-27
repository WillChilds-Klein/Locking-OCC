// Author: Alexander Thomson (thomson@cs.yale.edu)
//
// Lock manager implementing deterministic two-phase locking as described in
// 'The Case for Determinism in Database Systems'.

#include "txn/lock_manager.h"

LockManagerA::LockManagerA(deque<Txn*>* ready_txns) {
  ready_txns_ = ready_txns;
}

bool LockManagerA::WriteLock(Txn* txn, const Key& key) {

  LockRequest *lreq = new LockRequest(EXCLUSIVE, txn);
  deque<LockRequest> *d;
  if(lock_table_.count(key) == 0){
    d = new deque<LockRequest>;
    lock_table_[key] = d; 
  }
  else{
    d = lock_table_[key]; //get deque at key in lock_table_
  }

  deque<LockRequest>::iterator i;
  for(i = d->begin(); i != d->end(); i++){ // check to see if duplicate txn's present
    if(i->txn_ == txn)
      DIE("Duplicate txn's found in txn que for same key!");
  }

  if(txn_waits_.count(txn) == 0) // check if txn is in txn_waits_
    txn_waits_[txn] = 0;// if not, put it in
  txn_waits_[txn]++; // increment lock wait counter

  if(d->empty()){ // lock will be immediately granted, else not.
    d->push_back(*lreq); // put new lock req onto back of txn deque in lock_table_
    txn_waits_[txn]++; // increment lock wait counter
    return true;
  }

  d->push_back(*lreq); // put new lock req onto back of txn deque in lock_table_

  return false; // lock will not be immediately granted.
}

bool LockManagerA::ReadLock(Txn* txn, const Key& key) {
  // Since Part 1A implements ONLY exclusive locks, calls to ReadLock can
  // simply use the same logic as 'WriteLock'.
  return WriteLock(txn, key);
}

void LockManagerA::Release(Txn* txn, const Key& key) {

  if(lock_table_.count(key) == 0){
    return; // get outta here if no locks at key
  }
  deque<LockRequest> *d = lock_table_[key];

  if(d->front().txn_ == txn){ 
    d->pop_front(); // delete head
    if(d->size() > 0){ // put next elt on rdytxns
      txn_waits_[d->front().txn_]--;
      if(txn_waits_[d->front().txn_] == 0)
        ready_txns_->push_back(d->front().txn_);
    }
    return;
  }

  deque<LockRequest>::iterator i;
  for(i = d->begin(); i != d->end(); i++){ // walk through deque, starting with head
    if(i->txn_ == txn){
      d->erase(i); // issues with continuing to walk??
      return; // exit if no check for mult instances of txn in deque
    }
  }
  return;
}

LockMode LockManagerA::Status(const Key& key, vector<Txn*>* owners) {

  if(lock_table_.count(key) == 0){ // no locks on key
    return UNLOCKED;
  }
  deque<LockRequest> *d = lock_table_[key]; // get deque at key

  if(d->empty())
      return UNLOCKED;
  while(!owners->empty()) // delete previous owners
      owners->pop_back();

  owners->push_back(d->front().txn_); 
  return d->front().mode_;
}

LockManagerB::LockManagerB(deque<Txn*>* ready_txns) {
  ready_txns_ = ready_txns;
}

bool LockManagerB::WriteLock(Txn* txn, const Key& key) {
  return true;
}

bool LockManagerB::ReadLock(Txn* txn, const Key& key) {
  // multiple things try to access -->>> SHARED!!!
  return true;
}

void LockManagerB::Release(Txn* txn, const Key& key) {
  // CPSC 438/538:
  //
  // Implement this method!
}

LockMode LockManagerB::Status(const Key& key, vector<Txn*>* owners) {
  // CPSC 438/538:
  //
  // Implement this method!
  return UNLOCKED;
}

