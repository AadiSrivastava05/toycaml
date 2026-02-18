use std::collections::HashSet;
use std::hint;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::RwLock;
use std::thread::{self, ThreadId};

use crate::active_plan::VMActivePlan;
use crate::mmtk;
use crate::DummyVM;

use lazy_static::lazy_static;
use mmtk::memory_manager::start_worker;
use mmtk::util::opaque_pointer::*;
use mmtk::util::Address;
use mmtk::vm::GCThreadContext;
use mmtk::vm::{ActivePlan, Collection};
use mmtk::Mutator;

pub struct VMCollection {}

// TODO(Isfarul): Use for tracing/diagnostics/debugging behaviour
lazy_static! {
    static ref GC_THREADS: RwLock<HashSet<ThreadId>> = RwLock::new(HashSet::new());
}

static WANTS_TO_STOP: AtomicBool = AtomicBool::new(false);
static WORLD_HAS_STOPPED: AtomicBool = AtomicBool::new(false);

// Documentation: https://docs.mmtk.io/api/mmtk/vm/collection/trait.Collection.html
impl Collection<DummyVM> for VMCollection {
    // TODO(Isfarul): Can potentially use tls for logging which GC thread requested it
    fn stop_all_mutators<F>(_tls: VMWorkerThread, mut mutator_visitor: F)
    where
        F: FnMut(&'static mut Mutator<DummyVM>),
    {
        WANTS_TO_STOP.store(true, Ordering::SeqCst);

        // TODO(Isfarul): Add logging

        // Block for the world to stop all the mutators
        // TODO(Isfarul): Figure out memory ordering
        while !WORLD_HAS_STOPPED.load(Ordering::SeqCst) {
            hint::spin_loop();
        }

        for mutator in VMActivePlan::mutators() {
            mutator_visitor(mutator);
        }

        // TODO(Isfarul): Add logging
    }

    // TODO(Isfarul): Use tls for logging which GC thread requested it
    fn resume_mutators(_tls: VMWorkerThread) {
        WANTS_TO_STOP.store(true, Ordering::SeqCst);
    }

    // TODO(Isfarul): Is there a good way to have a mapping of mutator threads on both sides?
    fn block_for_gc(_tls: VMMutatorThread) {
        // TODO(Both): Do we actually need to do anything here? OCaml domains more or less
        // function independently of each other.
    }

    // We use threads internal to MMTk and don't expose it to/expect it from the VM
    // Note that the thread may live up to the lifetime of the entire program, it will
    // be used internally by MMTk to service GC work packets
    fn spawn_gc_thread(_tls: VMThread, ctx: GCThreadContext<DummyVM>) {
        let _ = thread::Builder::new()
            .name("MMTk Worker".to_string())
            .spawn(move || {
                register_current_thread();

                // Start the worker loop
                // We don't really need this to be a valid address, just something unique
                let worker_tls = VMWorkerThread(VMThread(OpaquePointer::from_address(unsafe {
                    Address::from_usize(thread_id::get())
                })));
                match ctx {
                    GCThreadContext::Worker(w) => start_worker(mmtk(), worker_tls, w),
                }

                unregister_current_thread();
            });
    }
}

fn register_current_thread() {
    let id = std::thread::current().id();
    GC_THREADS.write().unwrap().insert(id);
}

fn unregister_current_thread() {
    let id = std::thread::current().id();
    GC_THREADS.write().unwrap().remove(&id);
}

pub extern "C" fn world_has_stopped() {
    // TODO(Isfarul): Figure out memory ordering, Julia uses SeqCst
    WORLD_HAS_STOPPED.store(true, Ordering::SeqCst);
}

pub extern "C" fn wants_to_stop() -> bool {
    // TODO(Isfarul): Racy??
    // TODO(Both): Expose the boolean to VM directly? (See above)
    WANTS_TO_STOP.load(Ordering::SeqCst)
}
