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
   * Same goes for FSTEagerGarbageCollector
 * Tie eager garbage collection to transaction commits
   * Done in a somewhat generic way: if a delegate implements FSTTransactional, use it for transactions. Otherwise don't
  
# Phased Implementation #

 * Work on tests first?
 * Maybe add reference delegates without removing existing GC?
 * Leave trigger for LRU for last, so that it functions as NoOp.

     