use std::collections::HashMap;
use std::sync::atomic::AtomicUsize;
use std::sync::{OnceLock, RwLock};

use lazy_static::lazy_static;
use mmtk::vm::VMBinding;
use mmtk::{Mutator, MMTK};

pub mod active_plan;
pub mod api;
pub mod collection;
pub mod object_model;
pub mod reference_glue;
pub mod scanning;
pub mod slot;

pub type OCamlSlot = crate::slot::FieldSlot;
// TODO(Isfarul): change this to an appropriate implementation
pub type OCamlSlice = crate::slot::UnimplementedMemorySlice;

#[derive(Default)]
pub struct DummyVM;

// Documentation: https://docs.mmtk.io/api/mmtk/vm/trait.VMBinding.html
impl VMBinding for DummyVM {
    type VMObjectModel = object_model::VMObjectModel;
    type VMScanning = scanning::VMScanning;
    type VMCollection = collection::VMCollection;
    type VMActivePlan = active_plan::VMActivePlan;
    type VMReferenceGlue = reference_glue::VMReferenceGlue;
    type VMSlot = OCamlSlot;
    type VMMemorySlice = OCamlSlice;

    /// Allowed maximum alignment in bytes.
    const MAX_ALIGNMENT: usize = 1 << 6;
}

use mmtk::util::{Address, ObjectReference};

impl DummyVM {
    pub fn object_start_to_ref(start: Address) -> ObjectReference {
        // Safety: start is the allocation result, and it should not be zero with an offset.
        unsafe {
            ObjectReference::from_raw_address_unchecked(
                start + crate::object_model::OBJECT_REF_OFFSET,
            )
        }
    }
}

pub static SINGLETON: OnceLock<Box<MMTK<DummyVM>>> = OnceLock::new();

fn mmtk() -> &'static MMTK<DummyVM> {
    SINGLETON.get().unwrap()
}

struct Roots(ObjectReference);

// TODO(Isfarul): what else to use here?
struct MutatorState {
    size: *mut AtomicUsize,
    base: Address,
    mutator: *mut Mutator<DummyVM>,
}

// TODO(Isfarul): Safety
unsafe impl Sync for MutatorState {}
unsafe impl Send for MutatorState {}


lazy_static! {
    static ref GLOBAL_ROOTS: RwLock<Vec<Roots>> = RwLock::new(Vec::new());
    static ref MUTATORS: RwLock<HashMap<Address, MutatorState>> = RwLock::new(HashMap::new());
}
