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

  if(d->empty()){ // lock will be immediately granted, else not.
    d->push_back(*lreq); // put new lock req onto back of txn deque in lock_table_
    return true;
  }

  d->push_back(*lreq); // put new lock req onto back of txn deque in lock_table_
  txn_waits_[txn]++; // increment lock wait counter

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

  if(d->empty()){ // lock will be immediately granted, else not.
    d->push_back(*lreq); // put new lock req onto back of txn deque in lock_table_
    return true;
  }

  d->push_back(*lreq); // put new lock req onto back of txn deque in lock_table_
  txn_waits_[txn]++; // increment lock wait counter

  return false; // lock will not be immediately granted.
}

bool LockManagerB::ReadLock(Txn* txn, const Key& key) {
  LockRequest *lreq = new LockRequest(SHARED, txn);
  deque<LockRequest> *d;
  if(lock_table_.count(key) == 0){
    d = new deque<LockRequest>;
    lock_table_[key] = d;
  }
  else{
    d = lock_table_[key]; //get deque at key in lock_table_
  }

  deque<LockRequest>::iterator i;
  int allSharedFlag = 1;
  for(i = d->begin(); i != d->end(); i++){ // check to see if duplicate txn's present
    if(i->mode_ != SHARED) // check to see if all members are shared
      allSharedFlag--;
    if(i->txn_ == txn)
      DIE("Duplicate txn's found in txn que for same key!");
  }

  if(txn_waits_.count(txn) == 0) // check if txn is in txn_waits_
    txn_waits_[txn] = 0;// if not, put it in
  //txn_waits_[txn]++; // increment lock wait counter

  if(d->empty() || allSharedFlag){ // lock will be immediately granted, else not.
    d->push_back(*lreq); // put new lock req onto back of txn deque in lock_table_
    return true;
  }

  d->push_back(*lreq); // put new lock req onto back of txn deque in lock_table_
  txn_waits_[txn]++; // increment lock wait counter

  return false; // lock will not be immediately granted.
}

void LockManagerB::Release(Txn* txn, const Key& key) {
  if(lock_table_.count(key) == 0){
    return; // get outta here if no locks at key
  }
  deque<LockRequest> *d = lock_table_[key];

  if(d->size() == 1){
    d->pop_front(); // release lock
    return;
  }
  else if(d->front().txn_ == txn && d->size() > 1 && d->at(1).mode_ == EXCLUSIVE){// just like exc case
    d->pop_front(); // release lock
    txn_waits_[d->front().txn_]--; // decrement txncnt in txn_waits_
    if(txn_waits_[d->front().txn_] == 0)
      ready_txns_->push_back(d->front().txn_);
    return;
  }
  else if(d->front().txn_ == txn && d->size() > 1 && d->at(1).mode_ == SHARED){
    d->pop_front(); // release lock
    deque<LockRequest>::iterator i;
    for(i = d->begin(); i->mode_ == SHARED && i != d->end(); i++){
      txn_waits_[i->txn_]--; // decrement txncnt in txn_waits_
      if(txn_waits_[i->txn_] == 0)
        ready_txns_->push_back(i->txn_);
    }
    return;
  }

  deque<LockRequest>::iterator i;
  for(i = d->begin(); i != d->end(); i++){ // walk through deque, starting with head
    if(i->txn_ == txn){
      txn_waits_[i->txn_]--; // decrement txncnt in txn_waits_
      d->erase(i); // issues with continuing to walk??
      break; // exit if no check for mult instances of txn in deque
    }
  }
  for(i = d->begin(); i->mode_ == SHARED && i != d->end(); i++){
    txn_waits_[i->txn_]--; // decrement txncnt in txn_waits_
    if(txn_waits_[i->txn_] == 0)
      ready_txns_->push_back(i->txn_);
  }
  return;
}

LockMode LockManagerB::Status(const Key& key, vector<Txn*>* owners) {
  
  if(lock_table_.count(key) == 0){ // no locks on key
    return UNLOCKED;
  }
  deque<LockRequest> *d = lock_table_[key]; // get deque at key

  if(d->empty())
      return UNLOCKED;
  else if(d->front().mode_ == EXCLUSIVE){
    owners->pop_back(); // delete previous owner
    owners->push_back(d->front().txn_);
    return d->front().mode_;
  }

  while(!owners->empty()) // delete previous owners
      owners->pop_back();

  deque<LockRequest>::iterator i = d->begin(); // go thru list for all consec SHARED txns
  while(i->mode_ == SHARED && i != d->end()){
    owners->push_back(i->txn_);
    i++;
  }
  return d->front().mode_;
}