// Author: Alexander Thomson (thomson@cs.yale.edu)
// Modified by: Christina Wallin (christina.wallin@yale.edu)

#include "txn/txn_processor.h"
#include <stdio.h>

#include <set>

#include "txn/lock_manager.h"

// Thread & queue counts for StaticThreadPool initialization.
#define THREAD_COUNT 100
#define QUEUE_COUNT 10

TxnProcessor::TxnProcessor(CCMode mode)
    : mode_(mode), tp_(THREAD_COUNT, QUEUE_COUNT), next_unique_id_(1) {
  if (mode_ == LOCKING_EXCLUSIVE_ONLY)
    lm_ = new LockManagerA(&ready_txns_);
  else if (mode_ == LOCKING)
    lm_ = new LockManagerB(&ready_txns_);

  // Start 'RunScheduler()' running as a new task in its own thread.
  tp_.RunTask(
        new Method<TxnProcessor, void>(this, &TxnProcessor::RunScheduler));
}

TxnProcessor::~TxnProcessor() {
  if (mode_ == LOCKING_EXCLUSIVE_ONLY || mode_ == LOCKING)
    delete lm_;
}

void TxnProcessor::NewTxnRequest(Txn* txn) {
  // Atomically assign the txn a new number and add it to the incoming txn
  // requests queue.
  mutex_.Lock();
  txn->unique_id_ = next_unique_id_;
  next_unique_id_++;
  txn_requests_.Push(txn);
  mutex_.Unlock();
}

Txn* TxnProcessor::GetTxnResult() {
  Txn* txn;
  while (!txn_results_.Pop(&txn)) {
    // No result yet. Wait a bit before trying again (to reduce contention on
    // atomic queues).
    sleep(0.000001);
  }
  return txn;
}

void TxnProcessor::RunScheduler() {
  switch (mode_) {
    case SERIAL:                 RunSerialScheduler();
    case LOCKING:                RunLockingScheduler();
    case LOCKING_EXCLUSIVE_ONLY: RunLockingScheduler();
    case OCC:                    RunOCCScheduler();
    case P_OCC:                  RunOCCParallelScheduler();
  }
}

void TxnProcessor::RunSerialScheduler() {
  Txn* txn;
  while (tp_.Active()) {
    // Get next txn request.
    if (txn_requests_.Pop(&txn)) {
      // Execute txn.
      ExecuteTxn(txn);

      // Commit/abort txn according to program logic's commit/abort decision.
      if (txn->Status() == COMPLETED_C) {
        ApplyWrites(txn);
        txn->status_ = COMMITTED;
      } else if (txn->Status() == COMPLETED_A) {
        txn->status_ = ABORTED;
      } else {
        // Invalid TxnStatus!
        DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
      }

      // Return result to client.
      txn_results_.Push(txn);
    }
  }
}

void TxnProcessor::RunLockingScheduler() {
  Txn* txn;
  while (tp_.Active()) {
    // Start processing the next incoming transaction request.
    if (txn_requests_.Pop(&txn)) {
      int blocked = 0;
      // Request read locks.
      for (set<Key>::iterator it = txn->readset_.begin();
           it != txn->readset_.end(); ++it) {
        if (!lm_->ReadLock(txn, *it))
          blocked++;
      }

      // Request write locks.
      for (set<Key>::iterator it = txn->writeset_.begin();
           it != txn->writeset_.end(); ++it) {
        if (!lm_->WriteLock(txn, *it))
          blocked++;
      }

      // If all read and write locks were immediately acquired, this txn is
      // ready to be executed.
      if (blocked == 0)
        ready_txns_.push_back(txn);
    }

    // Process and commit all transactions that have finished running.
    while (completed_txns_.Pop(&txn)) {
      // Release read locks.
      for (set<Key>::iterator it = txn->readset_.begin();
           it != txn->readset_.end(); ++it) {
        lm_->Release(txn, *it);
      }
      // Release write locks.
      for (set<Key>::iterator it = txn->writeset_.begin();
           it != txn->writeset_.end(); ++it) {
        lm_->Release(txn, *it);
      }

      // Commit/abort txn according to program logic's commit/abort decision.
      if (txn->Status() == COMPLETED_C) {
        ApplyWrites(txn);
        txn->status_ = COMMITTED;
      } else if (txn->Status() == COMPLETED_A) {
        txn->status_ = ABORTED;
      } else {
        // Invalid TxnStatus!
        DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
      }

      // Return result to client.
      txn_results_.Push(txn);
    }

    // Start executing all transactions that have newly acquired all their
    // locks.
    while (ready_txns_.size()) {
      // Get next ready txn from the queue.
      txn = ready_txns_.front();
      ready_txns_.pop_front();

      // Start txn running in its own thread.
      tp_.RunTask(new Method<TxnProcessor, void, Txn*>(
            this,
            &TxnProcessor::ExecuteTxn,
            txn));
    }
  }
}

void TxnProcessor::RunOCCScheduler() {
  Txn* txn;
  bool validationFailure; // Set to true if validation failed.
  
  while (tp_.Active()) {
    // Start processing the next incoming transaction request.
    if (txn_requests_.Pop(&txn)) {
      txn->occ_start_time_ = GetTime();
      tp_.RunTask(
        new Method<TxnProcessor, void, Txn*>(this, &TxnProcessor::ExecuteTxn, txn));
    }
    
    while (completed_txns_.Pop(&txn)) {
      // Here, we validate the transaction.
      if (txn->Status() == COMPLETED_C) {
        validationFailure = false; // Valid until proven guilty!
      
        for (set<Key>::iterator itr = txn->readset_.begin(); itr != txn->readset_.end(); ++itr) { // Check for readset conflicts.
          if (storage_.Timestamp(*itr) >= txn->occ_start_time_) {
            validationFailure = true;
            break;
          }
        }
        
        if (!validationFailure) {
          for (set<Key>::iterator itr = txn->writeset_.begin(); itr != txn->writeset_.end(); ++itr) { // Check for writeset conflicts.
            if (storage_.Timestamp(*itr) >= txn->occ_start_time_) {
              validationFailure = true;
              break;
            }
          }
        }
        
        // Commit/restart
        if (validationFailure) {
          txn->status_ = INCOMPLETE;
          txn->reads_.clear();
          txn->writes_.clear();
          txn_requests_.Push(txn);
        } else {
          ApplyWrites(txn);
          txn_results_.Push(txn); // Return result to client.
        }
      } else if (txn->Status() == COMPLETED_A) {
        txn->status_ = ABORTED;
      } else {
        // Invalid TxnStatus!
        DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
      }
    }
  }
}

void TxnProcessor::RunOCCParallelScheduler() {
  Txn *txn;
  const int n = 10; // For now.
  const int m = 5; // For now.
  int i;
  set<Txn*> activeSetCopy;
  
  while (tp_.Active()) {
    // Start processing the next incoming transaction request.
    if (txn_requests_.Pop(&txn)) {
      txn->occ_start_time_ = GetTime();
      tp_.RunTask(
        new Method<TxnProcessor, void, Txn*>(this, &TxnProcessor::ExecuteTxn, txn));
    }
    
    for (i = 0; i < n; i++) {
      if (completed_txns_.Pop(&txn)) {
        activeSetCopy = active_set_.GetSet(); // Copy the active set.
        active_set_.Insert(txn); // Add this transaction to the active set.
        tp_.RunTask(
          new Method<TxnProcessor, void, Txn*, set<Txn*> >(this, &TxnProcessor::ValidateTxnParallel, txn, activeSetCopy));
      } else {
        break; // If there's nothing left to validate, break out of the loop.
      }
    }
    
    for (i = 0; i < m; i++) {
      if (validated_txns_.Pop(&txn)) {
        active_set_.Erase(txn); // Remove the transaction from the active set.
        if (txn->is_valid_) { // If transaction is valid, mark as committed.
          txn->status_ = COMMITTED;
          // Return result to client.
          txn_results_.Push(txn);
          // Cleanup. (What does this mean?)
        } else {
          txn->status_ = INCOMPLETE;
          txn->reads_.clear();
          txn->writes_.clear();
          txn_requests_.Push(txn); // Transaction will re-start next time we check for a new transaction request.
        }
      } else {
        break; // If there's nothing left to commit, break out of the loop.
      }
    }
  }
}

// Validates a transaction, returning true if valid and false otherwise.
// Written by JME; not in original code.
void TxnProcessor::ValidateTxnParallel(Txn* txn, set<Txn*> activeSet) {
  bool validationFailure;
  
  if (txn->Status() == COMPLETED_C) {
    validationFailure = false; // Valid until proven guilty!
  
    for (set<Key>::iterator itr = txn->readset_.begin(); itr != txn->readset_.end(); ++itr) { // Check for readset conflicts.
      if (storage_.Timestamp(*itr) >= txn->occ_start_time_) {
        validationFailure = true;
        break;
      }
    }
    
    if (!validationFailure) {
      for (set<Key>::iterator itr = txn->writeset_.begin(); itr != txn->writeset_.end(); ++itr) { // Check for writeset conflicts.
        if (storage_.Timestamp(*itr) >= txn->occ_start_time_) {
          validationFailure = true;
          break;
        }
      }
    }
    
    // Now we check for conflicts with the active set.
    if (!validationFailure) {
      for (set<Txn*>::iterator t = activeSet.begin(); t != activeSet.end() && !validationFailure; ++t) {
      
        // Check for conflicts between txn's writeset and t's readset or writeset.
        for (set<Key>::iterator itr = txn->writeset_.begin(); itr != txn->writeset_.end(); ++itr) {
          if ((*t)->readset_.find(*itr) != (*t)->readset_.end() || (*t)->writeset_.find(*itr) != (*t)->writeset_.end()) {
            validationFailure = true;
            break;
          }
        }
      }
    }
  
    // Write if valid; save validation result.
    if (validationFailure) {
      txn->is_valid_ = false;
    } else {
      ApplyWrites(txn);
      txn->is_valid_ = true;
    }
    
    validated_txns_.Push(txn); // Push the transaction onto our list of those to be validated.
  } else if (txn->Status() == COMPLETED_A) {
    txn->status_ = ABORTED;
  } else {
    // Invalid TxnStatus!
    DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
  }
}

void TxnProcessor::ExecuteTxn(Txn* txn) {
  // Read everything in from readset.
  for (set<Key>::iterator it = txn->readset_.begin();
       it != txn->readset_.end(); ++it) {
    // Save each read result iff record exists in storage.
    Value result;
    if (storage_.Read(*it, &result))
      txn->reads_[*it] = result;
  }

  // Also read everything in from writeset.
  for (set<Key>::iterator it = txn->writeset_.begin();
       it != txn->writeset_.end(); ++it) {
    // Save each read result iff record exists in storage.
    Value result;
    if (storage_.Read(*it, &result))
      txn->reads_[*it] = result;
  }

  // Execute txn's program logic.
  txn->Run();

  // Hand the txn back to the RunScheduler thread.
  completed_txns_.Push(txn);
}

void TxnProcessor::ApplyWrites(Txn* txn) {
  // Write buffered writes out to storage.
  for (map<Key, Value>::iterator it = txn->writes_.begin();
       it != txn->writes_.end(); ++it) {
    storage_.Write(it->first, it->second);
  }

  // Set status to committed.
  txn->status_ = COMMITTED;
}
