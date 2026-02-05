default:
	cd mmtk-bindings; cargo build
	gcc -o test testing_toycaml_c.c runtime.c -L./mmtk-bindings/target/debug -lmmtk_toycaml

clean:
	rm test
	cd mmtk-bindings; cargo clean
