# Major Pieces #

 * Reference Delegates
   * These implement the various hooks in a document lifecycle
     * A new target added
     * Documents added to target
     * Documents removed from a target
     * A target removed
     * Limbo document changes
     * Documents included in a mutation
     * Documents no longer included in a mutation
   * They are implemented per-persistence, per-GC strategy
     * We don't have to support every GC strategy with every persistence backend
     * Implementations can take advantage of persistence backend internals
 * Remove NoOp Garbage collector
   * Replace with LRU, which is essentially the same for testing purposes
 * Remove \[nearly\] all instances of FSTGarbageSource
   * The in-memory reference set for limbo documents is the only remaining user
     * This should be extracted into its own component and no longer use a garbage collector
   * Same goes for FSTEagerGarbageCollector
 * Tie eager garbage collection to transaction commits
   * Done in a somewhat generic way: if a delegate implements FSTTransactional, use it for transactions. Otherwise don't
  
# Phased Implementation #

 * Work on extracting limbo document structures from SyncEngine first, and remove that usage of 
 garbage collector
 * Implement LRU reference delegates, maybe expand garbage collector interface to take over
 what is currently called a reference delegate
   * This should include tests for the LRU algorithm (See FSTLRUGarbageCollectorTests.mm) 
 * Swap eager gc with FSTMemoryEagerReferenceDelegate (maybe renamed to a garbage collector)
   * Remove explicit calls to collect garbage, as well as local store references to a garbage collector
   * Ensure that memory persistence ties eager garbage collection to transaction commit, see FSTTransactional
   and FSTTransactionRunner.SetBackingPersistence.
 * Leave trigger for LRU for last, so that it functions as NoOp.
   * A prototype of `shouldGC` exists, depends on a variety of thresholds passed into 
   FSTLRUGarbageCollector, as well as the passage of time, and the ability to take the 
   current size of the persistence layer.
   * A triggering mechanism needs to be implemented.
     * Needs to be tied to async queue
     * Could be based on time (e.g. check every n seconds)
     * Could be based on number of mutations

     