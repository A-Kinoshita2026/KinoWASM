;; life_ok.wat - minimal valid module for kw_core_lifetest.
;; Exports f(i32)->i32 returning arg+41 (f(1)=42). No WASI imports, so the
;; standalone drivers can load it without extra host functions.
(module
  (memory 1)
  (func (export "f") (param i32) (result i32)
    local.get 0
    i32.const 41
    i32.add))
