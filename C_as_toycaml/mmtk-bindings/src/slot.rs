use std::{
    hash::Hash,
    marker::PhantomData,
    sync::atomic::{AtomicUsize, Ordering},
};

use mmtk::{
    util::{Address, ObjectReference},
    vm::slot::{MemorySlice, Slot},
};

/// Represents the field of an OCaml object
/// This may either be a tagged integer or a tagged
/// pointer. They are discriminated by their LSB, which
/// is 0 for a pointer and 1 for an integer.
/// Note that this forces the pointer to be 2 byte aligned.
/// This is not a concern for OCaml's runtime, since the fields of an
/// object which are pointers will always be word-aligned.
/// The integer's max range is restricted to 63 bits as well.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
#[repr(transparent)]
pub struct FieldSlot {
    slot_addr: *mut AtomicUsize,
}

impl FieldSlot {
    // TODO: Isfarul: Ordering is too strict
    pub fn get(&self) -> usize {
        unsafe { (*self.slot_addr).load(Ordering::SeqCst) }
    }

    // TODO: Isfarul: Ordering is too strict
    pub fn store(&self, val: usize) {
        unsafe {
            (*self.slot_addr).store(val, Ordering::SeqCst);
        }
    }
}

impl Slot for FieldSlot {
    fn load(&self) -> Option<mmtk::util::ObjectReference> {
        if self.get() & 1 == 0 {
            let addr = unsafe { Address::from_usize(self.get()) };
            ObjectReference::from_raw_address(addr)
        } else {
            None
        }
    }

    fn store(&self, object: mmtk::util::ObjectReference) {
        let tagged_addr = object.to_raw_address().as_usize();
        self.store(tagged_addr);
    }
}

/// Retrieve an unsigned integer from a slot
///
/// Returns Err if the slot was a pointer instead
impl TryFrom<FieldSlot> for usize {
    type Error = &'static str;

    fn try_from(value: FieldSlot) -> Result<Self, Self::Error> {
        if value.get() & 1 == 0 {
            Err("Tried to interpret tagged pointer as usize")
        } else {
            Ok(value.get() >> 1)
        }
    }
}

/// Retrieve an unsigned integer from a slot
///
/// Returns Err if the slot was a pointer instead
impl TryFrom<FieldSlot> for isize {
    type Error = &'static str;

    fn try_from(value: FieldSlot) -> Result<Self, Self::Error> {
        value.try_into().map(|res: usize| res as isize)
    }
}

/// Create a FieldSlot from an object reference
impl From<Address> for FieldSlot {
    fn from(addr: Address) -> Self {
        // TODO(Isfarul): Can this be ObjectReference instead?
        Self {
            slot_addr: addr.to_mut_ptr(),
        }
    }
}

/// Create an ObjectReference from a FieldSlot
impl TryFrom<FieldSlot> for Address {
    type Error = &'static str;

    fn try_from(value: FieldSlot) -> Result<Self, Self::Error> {
        // TODO(Isfarul): Can this be ObjectReference instead?
        if value.get() & 1 == 0 {
            Ok(unsafe { Address::from_usize(value.get()) })
        } else {
            Err("Tried to interpret an int as a reference")
        }
    }
}

unsafe impl Send for FieldSlot {}

/// Memory slice type with empty implementations.
#[derive(Debug, PartialEq, Eq, Clone, Hash)]
// TODO(Isfarul): Is this needed?
// Not atm, maybe later
pub struct UnimplementedMemorySlice<SL: Slot = FieldSlot>(PhantomData<SL>);

/// Slot iterator for `UnimplementedMemorySlice`.
pub struct UnimplementedMemorySliceSlotIterator<SL: Slot>(PhantomData<SL>);

impl<SL: Slot> Iterator for UnimplementedMemorySliceSlotIterator<SL> {
    type Item = SL;

    fn next(&mut self) -> Option<Self::Item> {
        unimplemented!()
    }
}

impl<SL: Slot> MemorySlice for UnimplementedMemorySlice<SL> {
    type SlotType = SL;
    type SlotIterator = UnimplementedMemorySliceSlotIterator<SL>;

    fn iter_slots(&self) -> Self::SlotIterator {
        unimplemented!()
    }

    fn object(&self) -> Option<ObjectReference> {
        unimplemented!()
    }

    fn start(&self) -> Address {
        unimplemented!()
    }

    fn bytes(&self) -> usize {
        unimplemented!()
    }

    fn copy(_src: &Self, _tgt: &Self) {
        unimplemented!()
    }
}
