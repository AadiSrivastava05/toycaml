use crate::DummyVM;
use crate::OCamlSlot;
use crate::GLOBAL_ROOTS;

use mmtk::util::opaque_pointer::*;
use mmtk::util::ObjectReference;
use mmtk::vm::RootsWorkFactory;
use mmtk::vm::Scanning;
use mmtk::vm::SlotVisitor;
use mmtk::Mutator;

pub struct VMScanning {}

// Documentation: https://docs.mmtk.io/api/mmtk/vm/scanning/trait.Scanning.html
impl Scanning<DummyVM> for VMScanning {
    fn scan_roots_in_mutator_thread(
        _tls: VMWorkerThread,
        _mutator: &'static mut Mutator<DummyVM>,
        _factory: impl RootsWorkFactory<OCamlSlot>,
    ) {
        // TODO(Isfarul): Add once mutators are done
        unimplemented!()
    }
    fn scan_vm_specific_roots(_tls: VMWorkerThread, mut factory: impl RootsWorkFactory<OCamlSlot>) {
        let slots = GLOBAL_ROOTS
            .read()
            .unwrap()
            .iter()
            .map(|root| root.0.to_raw_address().into())
            .collect();

        factory.create_process_roots_work(slots);
    }
    fn scan_object<SV: SlotVisitor<OCamlSlot>>(
        _tls: VMWorkerThread,
        object: ObjectReference,
        slot_visitor: &mut SV,
    ) {
        let addr = object.to_raw_address();
        // TODO(Isfarul): Deal with infix objects and headers later
        let header = addr.sub(std::mem::size_of::<OCamlSlot>());

        // TODO(Isfarul): move to it's own function
        let header_word: usize = unsafe { header.load() };
        let object_size: usize = header_word >> 10;

        for field_idx in 0..object_size {
            let slot = addr
                .add(field_idx * std::mem::size_of::<OCamlSlot>())
                .into();
            slot_visitor.visit_slot(slot);
        }
    }
    fn notify_initial_thread_scan_complete(_partial_scan: bool, _tls: VMWorkerThread) {
        // Nothing to do here
    }
    fn supports_return_barrier() -> bool {
        // Unused function in mmtk-core
        unimplemented!()
    }
    fn prepare_for_roots_re_scanning() {
        // TODO(Isfarul): Do we need to do anything here?
    }
}
