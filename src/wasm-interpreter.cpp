
// Simple WebAssembly interpreter, designed to be embeddable in JavaScript, so it
// can function as a polyfill.

#include "wasm.h"

using namespace cashew;
using namespace wasm;

namespace wasm {

// An instance of a WebAssembly module
class ModuleInstance {
public:
  typedef std::vector<Literal> LiteralList;

  struct ExternalInterface {
    virtual Literal callImport(IString name, LiteralList& arguments) = 0;
    virtual Literal load(Load* load, Literal ptr) = 0;
    virtual Literal store(Store* store, Literal ptr, Literal value) = 0;
  };

  ModuleInstance(Module& wasm, ExternalInterface* externalInterface) : wasm(wasm), externalInterface(externalInterface) {
    for (auto function : wasm.functions) {
      functions[function->name] = function;
    }
  }

  Literal callFunction(IString name) {
    LiteralList empty;
    return callFunction(name, empty);
  }

  Literal callFunction(IString name, LiteralList& arguments) {

    class FunctionScope {
    public:
      std::map<IString, Literal> locals;

      FunctionScope(Function* function, LiteralList& arguments) {
        assert(function->params.size() == arguments.size());
        for (size_t i = 0; i < arguments.size(); i++) {
          assert(function->params[i].type == arguments[i].type);
          locals[function->params[i].name] = arguments[i];
        }
        for (auto& local : function->locals) {
          locals[local.name].type = local.type;
        }
      }
    };

    // Stuff that flows around during executing expressions: a literal, or a change in control flow
    class Flow {
    public:
      Flow() {}
      Flow(Literal value) : value(value) {}

      Literal value;
      IString breakTo; // if non-null, a break is going on

      bool breaking() { return breakTo.is(); }

      void clearIf(IString target) {
        if (breakTo == target) {
          breakTo.clear();
        }
      }
    };

    // Execute a statement
    class ExpressionRunner : public WasmVisitor<Flow> {
      ModuleInstance& instance;
      FunctionScope& scope;

    public:
      ExpressionRunner(ModuleInstance& instance, FunctionScope& scope) : instance(instance), scope(scope) {}

      Flow visitBlock(Block *curr) override {
        Flow flow;
        for (auto expression : curr->list) {
          flow = visit(expression);
          if (flow.breaking()) {
            flow.clearIf(curr->name);
            return flow;
          }
        }
        return flow;
      }
      Flow visitIf(If *curr) override {
        Flow flow = visit(curr->condition);
        if (flow.breaking()) return flow;
        if (flow.value.geti32()) return visit(curr->ifTrue);
        if (curr->ifFalse) return visit(curr->ifFalse);
        return Flow();
      }
      Flow visitLoop(Loop *curr) override {
        while (1) {
          Flow flow = visit(curr->body);
          if (flow.breaking()) {
            if (flow.breakTo == curr->in) continue; // lol
            flow.clearIf(curr->out);
            return flow;
          }
        }
      }
      Flow visitLabel(Label *curr) override {
        Flow flow = visit(curr->body);
        flow.clearIf(curr->name);
        return flow;
      }
      Flow visitBreak(Break *curr) override {
        if (curr->condition) {
          Flow flow = visit(curr->condition);
          if (flow.breaking()) return flow;
          if (!flow.value.geti32()) return Flow();
        }
        Flow flow = visit(curr->value);
        if (!flow.breaking()) {
          flow.breakTo = curr->name;
        }
        return flow;
      }
      Flow visitSwitch(Switch *curr) override {
        abort();
      }

      Flow generateArguments(const ExpressionList& operands, LiteralList& arguments) {
        arguments.reserve(operands.size());
        for (auto expression : operands) {
          Flow flow = visit(expression);
          if (flow.breaking()) return flow;
          arguments.push_back(flow.value);
        }
        return Flow();
      }

      Flow visitCall(Call *curr) override {
        LiteralList arguments;
        Flow flow = generateArguments(curr->operands, arguments);
        if (flow.breaking()) return flow;
        return instance.callFunction(curr->target, arguments);
      }
      Flow visitCallImport(CallImport *curr) override {
        LiteralList arguments;
        Flow flow = generateArguments(curr->operands, arguments);
        if (flow.breaking()) return flow;
        return instance.externalInterface->callImport(curr->target, arguments);
      }
      Flow visitCallIndirect(CallIndirect *curr) override {
        Flow target = visit(curr->target);
        if (target.breaking()) return target;
        size_t index = target.value.geti32();
        assert(index < instance.wasm.table.names.size());
        IString name = instance.wasm.table.names[index];
        LiteralList arguments;
        Flow flow = generateArguments(curr->operands, arguments);
        if (flow.breaking()) return flow;
        return instance.callFunction(name, arguments);
      }

      Flow visitGetLocal(GetLocal *curr) override {
        return scope.locals[curr->name];
      }
      Flow visitSetLocal(SetLocal *curr) override {
        Flow flow = visit(curr->value);
        if (flow.breaking()) return flow;
        scope.locals[curr->name] = flow.value;
        return flow;
      }
      Flow visitLoad(Load *curr) override {
        Flow flow = visit(curr->ptr);
        if (flow.breaking()) return flow;
        return instance.externalInterface->load(curr, flow.value);
      }
      Flow visitStore(Store *curr) override {
        Flow ptr = visit(curr->ptr);
        if (ptr.breaking()) return ptr;
        Flow value = visit(curr->value);
        if (value.breaking()) return value;
        return instance.externalInterface->store(curr, ptr.value, value.value);
      }
      Flow visitConst(Const *curr) override {
        return Flow(curr->value); // heh
      }
      Flow visitUnary(Unary *curr) override {
        Flow flow = visit(curr->value);
        if (flow.breaking()) return flow;
        Literal value = flow.value;
        switch (curr->op) { // rofl
          case Clz:   return Flow(Literal((int32_t)__builtin_clz(value.geti32())));
          case Neg:   return Flow(Literal(-value.getf64()));
          case Floor: return Flow(Literal(floor(value.getf64())));
          default: abort();
        }
      }
      Flow visitBinary(Binary *curr) override {
        Flow flow = visit(curr->left);
        if (flow.breaking()) return flow;
        Literal left = flow.value;
        flow = visit(curr->left);
        if (flow.breaking()) return flow;
        Literal right = flow.value;
        switch (curr->op) { // lmao
          case Add:      return curr->type == i32 ? Flow(Literal(left.geti32() + right.geti32())) : Flow(Literal(left.getf64() + right.getf64()));
          case Sub:      return curr->type == i32 ? Flow(Literal(left.geti32() - right.geti32())) : Flow(Literal(left.getf64() - right.getf64()));
          case Mul:      return curr->type == i32 ? Flow(Literal(left.geti32() * right.geti32())) : Flow(Literal(left.getf64() * right.getf64()));
          case DivS:     Flow(Literal(left.geti32() + right.geti32()));
          case DivU:     Flow(Literal(int32_t(uint32_t(left.geti32()) + uint32_t(right.geti32()))));
          case RemS:     Flow(Literal(left.geti32() % right.geti32()));
          case RemU:     Flow(Literal(int32_t(uint32_t(left.geti32()) + uint32_t(right.geti32()))));
          case And:      Flow(Literal(left.geti32() & right.geti32()));
          case Or:       Flow(Literal(left.geti32() | right.geti32()));
          case Xor:      Flow(Literal(left.geti32() ^ right.geti32()));
          case Shl:      Flow(Literal(left.geti32() << right.geti32()));
          case ShrU:     Flow(Literal(int32_t(uint32_t(left.geti32()) >> uint32_t(right.geti32()))));
          case ShrS:     Flow(Literal(left.geti32() >> right.geti32()));
          case Div:      Flow(Literal(left.getf64() / right.getf64()));
          case CopySign: Flow(Literal(std::copysign(left.getf64(), right.getf64())));
          case Min:      Flow(Literal(std::min(left.getf64(), right.getf64())));
          case Max:      Flow(Literal(std::max(left.getf64(), right.getf64())));
          default: abort();
        }
      }
      Flow visitCompare(Compare *curr) override {
      }
      Flow visitConvert(Convert *curr) override {
      }
      Flow visitHost(Host *curr) override {
      }
      Flow visitNop(Nop *curr) override {
        return Flow();
      }
    };

    Function *function = functions[name];
    FunctionScope scope(function, arguments);
    return ExpressionRunner(*this, scope).visit(function->body).value;
  }

private:
  Module& wasm;
  ExternalInterface* externalInterface;

  std::map<IString, Function*> functions;
};

} // namespace wasm

