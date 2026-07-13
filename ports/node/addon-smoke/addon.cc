// addon.cc — the smallest possible statically-linked Node native addon: exports add(a, b).
//
// NanOS has no ELF `.so` loader, so there is no `.node` dlopen. Required native modules are compiled
// INTO node.nxe and registered via Node's linked-module mechanism (NODE_MODULE_LINKED), reached from
// JS as process._linkedBinding('addon_smoke'). This proves that path end to end (plan 02 Task 2.5).
#include <node.h>
namespace {
void Add(const v8::FunctionCallbackInfo<v8::Value>& args) {
  double a = args[0].As<v8::Number>()->Value();
  double b = args[1].As<v8::Number>()->Value();
  args.GetReturnValue().Set(a + b);
}
void Init(v8::Local<v8::Object> exports, v8::Local<v8::Value>, void*) {
  NODE_SET_METHOD(exports, "add", Add);
}
}  // namespace
NODE_MODULE_LINKED(addon_smoke, Init)
