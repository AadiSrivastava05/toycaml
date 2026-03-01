use std::collections::HashMap;
use std::sync::RwLockReadGuard;

use crate::{DummyVM, MutatorState, MUTATORS};
use mmtk::util::{opaque_pointer::*, Address};
use mmtk::vm::ActivePlan;
use mmtk::Mutator;

pub struct VMActivePlan {}

struct MutatorIterator<'a> {
    _guard: RwLockReadGuard<'a, HashMap<Address, MutatorState>>,
    mutators: Box<[*mut Mutator<DummyVM>]>,
    cursor: usize,
}

impl<'a> MutatorIterator<'a> {
    fn new(
        guard: RwLockReadGuard<'a, HashMap<Address, MutatorState>>,
        mutators: Box<[*mut Mutator<DummyVM>]>,
    ) -> Self {
        Self {
            _guard: guard,
            mutators,
            cursor: 0,
        }
    }
}

impl<'a> Iterator for MutatorIterator<'a> {
    type Item = &'a mut Mutator<DummyVM>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.cursor > self.mutators.len() {
            None
        } else {
            self.cursor += 1;
            Some(unsafe { &mut *self.mutators[self.cursor - 1] })
        }
    }
}

// Documentation: https://docs.mmtk.io/api/mmtk/vm/active_plan/trait.ActivePlan.html
impl ActivePlan<DummyVM> for VMActivePlan {
    fn number_of_mutators() -> usize {
        MUTATORS.read().unwrap().len()
    }

    fn is_mutator(tls: VMThread) -> bool {
        MUTATORS.read().unwrap().contains_key(&tls.0.to_address())
    }

    fn mutator(tls: VMMutatorThread) -> &'static mut Mutator<DummyVM> {
        let mutator = MUTATORS
            .read()
            .unwrap()
            .get(&tls.0 .0.to_address())
            .unwrap()
            .mutator;

        unsafe { &mut *mutator }
    }

    fn mutators<'a>() -> Box<dyn Iterator<Item = &'a mut Mutator<DummyVM>> + 'a> {
        let guard = MUTATORS.read().unwrap();
        let mutators = guard.values().map(|val| val.mutator).collect();

        Box::new(MutatorIterator::new(guard, mutators))
    }
}
