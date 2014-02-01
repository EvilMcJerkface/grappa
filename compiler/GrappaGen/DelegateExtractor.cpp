#undef DEBUG
#include "DelegateExtractor.hpp"

#include <llvm/InstVisitor.h>
#include <llvm/IR/IRBuilder.h>

using namespace llvm;

/// definedInRegion - Return true if the specified value is defined in the
/// extracted region.
static bool definedInRegion(const SetVector<BasicBlock *> &Blocks, Value *V) {
  if (Instruction *I = dyn_cast<Instruction>(V))
    if (Blocks.count(I->getParent()))
      return true;
  return false;
}

/// definedInCaller - Return true if the specified value is defined in the
/// function being code extracted, but not in the region being extracted.
/// These values must be passed in as live-ins to the function.
static bool definedInCaller(const SetVector<BasicBlock *> &Blocks, Value *V) {
  if (isa<Argument>(V)) return true;
  if (Instruction *I = dyn_cast<Instruction>(V))
    if (!Blocks.count(I->getParent()))
      return true;
  return false;
}

template< typename InsertType >
GetElementPtrInst* struct_elt_ptr(Value *struct_ptr, int idx, const Twine& name,
                                  InsertType* insert) {
  auto i32_ty = Type::getInt32Ty(insert->getContext());
  return GetElementPtrInst::Create(struct_ptr, (Value*[]){
    Constant::getNullValue(i32_ty),
    ConstantInt::get(i32_ty, idx)
  }, name, insert);
};


DelegateExtractor::DelegateExtractor(Module& mod, GlobalPtrInfo& ginfo):
  ctx(&mod.getContext()), mod(&mod), ginfo(ginfo)
{
  layout = new DataLayout(&mod);
}

void DelegateExtractor::findInputsOutputsUses(ValueSet& inputs, ValueSet& outputs,
                                              GlobalPtrMap& gptrs) const {
  // map loaded value back to its pointer
  std::map<Value*,Value*> gvals;
  
  for (auto bb : bbs) {
    for (auto& ii : *bb) {
      
      if (auto gld = dyn_cast_addr<GLOBAL_SPACE,LoadInst>(&ii)) {
        auto p = gld->getPointerOperand();
        if (gptrs.count(p) == 0) gptrs[p] = {};
        gptrs[p].loads++;
        gvals[gld] = p;
      } else if (auto gi = dyn_cast_addr<GLOBAL_SPACE,StoreInst>(&ii)) {
        auto p = gi->getPointerOperand();
        if (gptrs.count(p) == 0) gptrs[p] = {};
        gptrs[p].stores++;
      }
      
      for_each_op(op, ii) {
        if (definedInCaller(bbs, *op)) inputs.insert(*op);
        if (gvals.count(*op) > 0) {
          gptrs[gvals[*op]].uses++;
        }
      }
      for_each_use(use, ii) {
        if (!definedInRegion(bbs, *use)) {
          outputs.insert(&ii);
          break;
        }
      }
    }
  }
}

void DelegateExtractor::viewUnextracted() {
  ViewGraph(this, "delegate(unextracted) in " + outer_fn->getName());
}

Function* DelegateExtractor::extractFunction() {
  assert( gptrs.size() == 1 );
  auto gptr = gptrs[0];
  
  auto bbin = bbs[0];
  
  // because of how we construct regions, should not begin with Phi
  // (should start with global load/store...)
  assert( ! isa<PHINode>(*bbin->begin()) );
  
  SmallSet<BasicBlock*,4> external_preds;
  for (auto bb : bbs) {
    for_each(p, bb, pred) {
      if (bbs.count(*p) == 0) {
        external_preds.insert(*p);
      }
    }
  }
  if (external_preds.size() > 1) {
    errs() << "!! more than one entry into extracted region!\n";
    viewUnextracted();
    assert(false);
  }
  
  Instruction* first = bbin->begin();
  auto& dl = first->getDebugLoc();
  outs() << "\n-------------------\nextracted delegate: "
         << "(line " << dl.getLine() << ")\n";
  for (auto bb : bbs) { outs() << *bb; }
  outs() << "--------------------\n";
  
  ValueSet inputs, outputs;
  GlobalPtrMap gptrs;
  findInputsOutputsUses(inputs, outputs, gptrs);
  
  outs() << "inputs:\n";
  for (auto in : inputs) {
    outs() << "  " << in->getName() << " (" << layout->getTypeAllocSize(in->getType()) << ")\n";
  }
  outs() << "outputs:\n";
  for (auto out : outputs) {
    outs() << "  " << out->getName() << " (" << layout->getTypeAllocSize(out->getType()) << ")\n";
  }
  outs().flush();
  
//  DEBUG( errs() << "\nins:"; for (auto v : inputs) errs() << "\n  " << *v; errs() << "\n"
//    << "outs:"; for (auto v : outputs) errs() << "\n  " << *v; errs() << "\n"
//    << "gptrs:\n"; for (auto v : gptrs) {
//      auto u = v.second;
//      errs() << "  " << v.first->getName()
//    << " { loads:" << u.loads << ", stores:" << u.stores << ", uses:" << u.uses << " }\n";
//  } errs() << "\n" );
  
  // create struct types for inputs & outputs
  SmallVector<Type*,8> in_types, out_types;
  for (auto& p : inputs)  {  in_types.push_back(p->getType()); }
  for (auto& p : outputs) { out_types.push_back(p->getType()); }
  
  auto in_struct_ty = StructType::get(*ctx, in_types);
  auto out_struct_ty = StructType::get(*ctx, out_types);
  
  // create function shell
  auto new_fn_ty = FunctionType::get(i16_ty, (Type*[]){ void_ptr_ty, void_ptr_ty }, false);
  auto new_fn = Function::Create(new_fn_ty, GlobalValue::InternalLinkage, "delegate." + bbin->getName(), mod);
  
  auto entrybb = BasicBlock::Create(*ctx, "d.entry", new_fn);
  
  auto i64_num = [=](long v) { return ConstantInt::get(i64_ty, v); };
  
  ValueToValueMapTy clone_map;
  //      std::map<Value*,Value*> arg_map;
  
  Function::arg_iterator argi = new_fn->arg_begin();
  auto in_arg_ = argi++;
  auto out_arg_ = argi++;
  
  DEBUG( errs() << "in_arg_:" << *in_arg_ << "\n" );
  DEBUG( errs() << "out_arg_:" << *out_arg_ << "\n" );
  
  auto in_arg = new BitCastInst(in_arg_, in_struct_ty->getPointerTo(), "bc.in", entrybb);
  auto out_arg = new BitCastInst(out_arg_, out_struct_ty->getPointerTo(), "bc.out", entrybb);
  
  // copy delegate block into new fn & remap values to use args
  for (auto bb : bbs) {
    clone_map[bb] = CloneBasicBlock(bb, clone_map, ".clone", new_fn);
  }
  
  auto newbb = dyn_cast<BasicBlock>(clone_map[bbin]);
  
  std::map<Value*,Value*> remaps;
  
  for (int i = 0; i < outputs.size(); i++) {
    auto v = outputs[i];
    auto ge = struct_elt_ptr(out_arg, i, "out.clr", entrybb);
    new StoreInst(Constant::getNullValue(v->getType()), ge, false, entrybb);
  }
  
  for (int i = 0; i < inputs.size(); i++) {
    auto v = inputs[i];
    auto gep = struct_elt_ptr(in_arg, i, "d.ge.in." + v->getName(), entrybb);
    auto in_val = new LoadInst(gep, "d.in." + v->getName(), entrybb);
    
    DEBUG( errs() << "in_val: " << *in_val << " (parent:" << in_val->getParent()->getName() << ", fn:" << in_val->getParent()->getParent()->getName() << ")\n" );
    
    Value *final_in = in_val;
    
    // if it's a global pointer, get local address for it
    if (auto in_gptr_ty = dyn_cast_addr<GLOBAL_SPACE>(in_val->getType())) {
      auto ptr_ty = in_gptr_ty->getElementType()->getPointerTo();
      auto bc = new BitCastInst(in_val, void_gptr_ty, "getptr.bc", entrybb);
      auto vptr = CallInst::Create(ginfo.get_pointer_fn, (Value*[]){ bc }, "getptr", entrybb);
      auto ptr = new BitCastInst(vptr, ptr_ty, "localptr", entrybb);
      final_in = ptr;
    }
    
    clone_map[v] = final_in;
  }
  
  // jump from entrybb to block cloned from inblock
  BranchInst::Create(newbb, entrybb);
  
  auto old_fn = bbin->getParent();
  
  auto retbb = BasicBlock::Create(*ctx, "ret." + bbin->getName(), new_fn);
  
  auto retTy = Type::getInt16Ty(*ctx);
  
  // create PHI for selecting which return value to use
  // make sure it's the first thing in the BB
  auto retphi = PHINode::Create(retTy, exits.size(), "d.retphi", retbb);
  
  // return from end of created block
  ReturnInst::Create(*ctx, retphi, retbb);
  
  // store outputs before return
  for (int i = 0; i < outputs.size(); i++) {
    assert(clone_map.count(outputs[i]) > 0);
    auto v = dyn_cast<Instruction>(clone_map[outputs[i]]);
    assert(v && "how is an output not an instruction?");
    
    // insert gep/store at end of value's block
    auto store_pt = v->getParent()->getTerminator();
    auto ep = struct_elt_ptr(out_arg, i, "d.out.gep." + v->getName(), store_pt);
    new StoreInst(v, ep, true, store_pt);
  }
  
  //////////////
  // emit call
  
//  assert(bbin->getTerminator()->getNumSuccessors() == 1);
  
//  auto prevbb = bbin->getUniquePredecessor();
//  assert(prevbb != nullptr);
  
  // create new bb to replace old block and call
  auto callbb = BasicBlock::Create(bbin->getContext(), "d.call.blk", old_fn, bbin);
  
  for_each(predit, bbin, pred) {
    auto bb = *predit;
    if (bbs.count(bb) == 0) {
      bb->getTerminator()->replaceUsesOfWith(bbin, callbb);
    }
  }
  
  auto call_pt = callbb;
  
  // allocate space for in/out structs near top of function
  auto in_struct_alloca = new AllocaInst(in_struct_ty, 0, "d.in_struct", old_fn->begin()->begin());
  auto out_struct_alloca = new AllocaInst(out_struct_ty, 0, "d.out_struct", old_fn->begin()->begin());
  
  // copy inputs into struct
  for (int i = 0; i < inputs.size(); i++) {
    auto v = inputs[i];
    auto gep = struct_elt_ptr(in_struct_alloca, i, "dc.in", call_pt);
    new StoreInst(v, gep, false, call_pt);
  }
  
  //////////////////////////////
  // for debug:
  // init out struct to 0
  for (int i = 0; i < outputs.size(); i++) {
    auto v = outputs[i];
    auto gep = struct_elt_ptr(out_struct_alloca, i, "dc.out.dbg", call_pt);
    new StoreInst(Constant::getNullValue(v->getType()), gep, false, call_pt);
  }
  //////////////////////////////
  
  assert(gptr && ginfo.get_core_fn);
  
  auto target_core = CallInst::Create(ginfo.get_core_fn, (Value*[]){
    new BitCastInst(gptr, void_gptr_ty, "", call_pt)
  }, "", call_pt);
  
  assert(ginfo.call_on_fn);
  assert(target_core);
  assert(new_fn);
  assert(in_struct_alloca);
  assert(out_struct_alloca);
  assert(call_pt);
  
  auto calli = CallInst::Create(ginfo.call_on_fn, (Value*[]){
    target_core,
    new_fn,
    new BitCastInst(in_struct_alloca, void_ptr_ty, "", call_pt),
    i64_num(layout->getTypeAllocSize(in_struct_ty)),
    new BitCastInst(out_struct_alloca, void_ptr_ty, "", call_pt),
    i64_num(layout->getTypeAllocSize(out_struct_ty))
  }, "", call_pt);
  
  ////////////////////////////
  // switch among exit blocks
  
  auto switchi = SwitchInst::Create(calli, callbb, exits.size(), call_pt);
  
  DEBUG( errs() << "exits => " << exits.size() << "\n" );
  unsigned exit_id = 0;
  for (auto& e : exits) {
    auto retcode = ConstantInt::get(retTy, exit_id);
    
    // bb in delegate that we're coming from
    auto oldbb = e.second;
    assert(clone_map.count(oldbb) > 0);
    auto predbb = dyn_cast<BasicBlock>(clone_map[oldbb]);
    assert(predbb && predbb->getParent() == new_fn);
    
    // hook up exit from region with phi node in return block
    retphi->addIncoming(retcode, predbb);
    
    // jump to old exit block when call returns the code for it
    auto targetbb = e.first;
    switchi->addCase(retcode, targetbb);
    
    assert(switchi->getParent() == callbb);
    for (auto& inst : *targetbb) if (auto phi = dyn_cast<PHINode>(&inst)) {
      int i;
      while ((i = phi->getBasicBlockIndex(oldbb)) >= 0)
        phi->setIncomingBlock(i, callbb);
    }
    
    // in extracted delegate, remap branches outside to retbb
    clone_map[targetbb] = retbb;
    
    exit_id++;
  }
  
  // use clone_map to remap values in new function
  // (including branching to new retbb instead of exit blocks)
  for_each(inst, new_fn, inst) {
    RemapInstruction(&*inst, clone_map, RF_IgnoreMissingEntries);
  }
  
  // load outputs
  for (int i = 0; i < outputs.size(); i++) {
    auto v = outputs[i];
    auto gep = struct_elt_ptr(out_struct_alloca, i, "dc.out." + v->getName(), switchi);
    auto ld = new LoadInst(gep, "d.call.out." + v->getName(), switchi);
    
    DEBUG( errs() << "replacing output:\n" << *v << "\nwith:\n" << *ld << "\nin:\n" );
    std::vector<User*> Users(v->use_begin(), v->use_end());
    for (unsigned u = 0, e = Users.size(); u != e; ++u) {
      Instruction *inst = cast<Instruction>(Users[u]);
      DEBUG( errs() << *inst << "  (parent: " << inst->getParent()->getParent()->getName() << ") " );
      if (inst->getParent()->getParent() != new_fn) {
        inst->replaceUsesOfWith(v, ld);
      } else {
        DEBUG( errs() << " !! wrong function!" );
      }
      DEBUG( errs() << "\n" );
    }
  }
  
  // any global* loads/stores must be local, so fix them up
  for (auto bb = new_fn->begin(); bb != new_fn->end(); bb++) {
    for (auto inst = bb->begin(); inst != bb->end(); ) {
      Instruction *orig = inst;
      inst++;
      Value *v = nullptr;
      bool isVolatile = false;
      if (auto ld = dyn_cast_addr<GLOBAL_SPACE,LoadInst>(orig)) {
        outs() << "!! found a global load:" << *orig << "\n";
        ginfo.replace_global_with_local(ld->getPointerOperand(), ld);

      } else if (auto st = dyn_cast_addr<GLOBAL_SPACE,StoreInst>(orig)) {
        outs() << "!! found a global store:" << *orig << "\n";
        ginfo.replace_global_with_local(st->getPointerOperand(), st);
        
      } else if (auto addrcast = dyn_cast<AddrSpaceCastInst>(orig)) {
        Value *new_val = nullptr;
        if (addrcast->getSrcTy() == addrcast->getType()) {
          new_val = addrcast->getOperand(0);
        } else if (addrcast->getSrcTy()->getPointerAddressSpace() == addrcast->getType()->getPointerAddressSpace()) {
          new_val = new BitCastInst(addrcast->getOperand(0), addrcast->getType(), addrcast->getName(), addrcast);
        }
        if (new_val) {
          errs() << "!! removing obsolete addrspacecast...\n";
          addrcast->replaceAllUsesWith(new_val);
          addrcast->eraseFromParent();
        }
      }
    }
  }

  // verify that all uses of these bbs are contained
  for (auto bb : bbs) {
    for (auto u = bb->use_begin(), ue = bb->use_end(); u != ue; u++) {
      if (auto uu = dyn_cast<Instruction>(*u)) {
        if (bbs.count(uu->getParent()) == 0) {
          errs() << "use escaped => " << *uu << *bb;
          assert(false);
        }
      }
    }
    for (auto& i : *bb) {
      for (auto u = i.use_begin(), ue = i.use_end(); u != ue; u++) {
        if (auto uu = dyn_cast<Instruction>(*u)) {
          if (bbs.count(uu->getParent()) == 0) {
            errs() << "use escaped => " << *uu << *bb;
            assert(false);
          }
        }
      }
    }
  }
  // delete old bbs
  for (auto bb : bbs) for (auto& i : *bb) i.dropAllReferences();
  for (auto bb : bbs) bb->eraseFromParent();
  
  DEBUG(
    errs() << "----------------\nconstructed delegate fn:\n" << *new_fn;
    errs() << "----------------\ncall site:\n" << *callbb;
  );
  
  outs() << "-------------------\n";
  return new_fn;
}

Value* DelegateExtractor::find_provenance(Instruction *inst, Value *val) {
  if (!val) val = inst;
  
  // see if we already know the answer
  if (provenance.count(val)) {
    provenance[inst] = provenance[val];
    return provenance[inst];
  }
  
  // check if it's a potentially-valid pointer in its own right
  // TODO: pull this out into own function, use in 'valid_in_delegate' to check what 'find_provenance' returns...
  if (dyn_cast_addr<SYMMETRIC_SPACE>(val->getType())) {
    return (provenance[inst] = val);
  } else if (dyn_cast_addr<GLOBAL_SPACE>(val->getType())) {
    if (gptrs.count(val)) {
      return (provenance[inst] = val);
    }
  } else if (isa<GlobalVariable>(val)) {
    return (provenance[inst] = val);
  }
  
  // now start inspecting the instructions and recursing
  if (auto gep = dyn_cast<GetElementPtrInst>(inst)) {
    return (provenance[gep] = find_provenance(inst, gep->getPointerOperand()));
  } else if (auto c = dyn_cast<CastInst>(inst)) {
    return (provenance[c] = find_provenance(inst, c->getOperand(0)));
  }
  
  return nullptr;
}

bool DelegateExtractor::valid_in_delegate(Instruction* inst, ValueSet& available_vals) {
  if (auto gld = dyn_cast_addr<GLOBAL_SPACE,LoadInst>(inst)) {
    if (gptrs.count(gld->getPointerOperand())) {
      available_vals.insert(gld);
      DEBUG( errs() << "load to same gptr: ok\n" );
      return true;
    }
    
  } else if (auto g = dyn_cast_addr<GLOBAL_SPACE,StoreInst>(inst)) {
    if (gptrs.count(g->getPointerOperand())) {
      DEBUG( errs() << "store to same gptr: great!\n" );
      return true;
    } else {
      DEBUG( errs() << "store to different gptr: not supported yet.\n" );
      return false;
    }
    
  } else if (dyn_cast_addr<SYMMETRIC_SPACE,LoadInst>(inst) ||
             dyn_cast_addr<SYMMETRIC_SPACE,StoreInst>(inst)) {
    DEBUG(errs() << "symmetric access: ok\n");
    return true;
    
  } else if (isa<LoadInst>(inst) || isa<StoreInst>(inst)) {
    Value *ptr = nullptr;
    if (auto m = dyn_cast<LoadInst>(inst))  ptr = m->getPointerOperand();
    if (auto m = dyn_cast<StoreInst>(inst)) ptr = m->getPointerOperand();
    
    if (isa<GlobalVariable>(ptr)) {
      DEBUG( errs() << "!! static global variable\n" );
      return true;
    } else {
      DEBUG( errs() << "load/store to normal memory: " << *inst << "\n" );
      return false;
    }
    
  } else if ( auto gep = dyn_cast<GetElementPtrInst>(inst) ) {
    
    if (gep->getAddressSpace() == SYMMETRIC_SPACE) return true;
    if (isa<GlobalVariable>(gep->getPointerOperand())) return true;
    
    // TODO: fix this, some GEP's should be alright...
    return false;
    
  } else if ( isa<PHINode>(inst) ) {
    return true;
  } else if ( isa<TerminatorInst>(inst) ) {
    if ( isa<BranchInst>(inst) || isa<SwitchInst>(inst) ) { return true; }
    else { return false; }
  } else if ( isa<InvokeInst>(inst)) {
    return false;
  } else if (auto call = dyn_cast<CallInst>(inst)) {
    auto fn = call->getCalledFunction();
    if (fn->getName() == "llvm.dbg.value" ||
        fn->doesNotAccessMemory())
      return true;
    
    // allow method calls on `symmetric*`
    // what we should see is an addrspacecast value as the first op
    if (auto l = dyn_cast<AddrSpaceCastInst>(call->getOperand(0))) {
      if (l->getSrcTy()->getPointerAddressSpace() == SYMMETRIC_SPACE) {
        errs() << "method call on symmetric pointer\n";
        return true;
      } else if (gptrs.count(l->getOperand(0))) {
        errs() << "method call on same global ptr\n";
        return true;
      }
    }
    
    // TODO: detect if function is pure / inline it and see??
    return false;
  } else {
    return true;
    //          bool is_bin_op = isa<BinaryOperator>(inst);
    //
    //          if (is_bin_op) outs() << "checking ops: " << *inst << "\n";
    //
    //          bool valid = std::all_of(inst->op_begin(), inst->op_end(), [&](Value* op){
    //            if (is_bin_op) {
    //              outs() << *op << "\n  avail:" << available_vals.count(op) << ", const:" << dyn_cast<Constant>(op) << "\n";
    //            }
    //            if (available_vals.count(op) > 0 || isa<Constant>(op)) {
    //              return true;
    //            } else {
    //              return false;
    //            }
    //          });
    //          available_vals.insert(inst);
    //          return valid;
  }
  return false;
};


BasicBlock* DelegateExtractor::findStart(BasicBlock *bb) {
  Value *gptr = nullptr;
  for (auto i = bb->begin(); i != bb->end(); i++) {
    if (auto gld = dyn_cast_addr<GLOBAL_SPACE,LoadInst>(i)) {
      gptr = gld->getPointerOperand();
    } else if (auto g = dyn_cast_addr<GLOBAL_SPACE,StoreInst>(i)) {
      gptr = g->getPointerOperand();
    } else if (auto c = dyn_cast<AddrSpaceCastInst>(i)) {
      if (c->getSrcTy()->getPointerAddressSpace() == GLOBAL_SPACE) {
        gptr = c->getOperand(0);
      }
    }
    if (gptr) {
      gptrs.insert(gptr);
      
      if (i != bb->begin()) {
        bb = bb->splitBasicBlock(i, bb->getName()+".dstart");
      }
      DEBUG( errs() << ">>>>>>* " << *bb );
      
      outer_fn = bb->getParent();
      bbs.insert(bb);
      return bb;
    }
  }
  return nullptr;
}

// Expand region to include some, all, or none of the given BB, recursively
bool DelegateExtractor::expand(BasicBlock *bb) {
  assert(gptrs.size() == 1);
  
  ValueSet vals;
  
  // to avoid the risk of having a second entry point, conservatively disallow this
  // (note: other preds would be alright if the entire loop was in the delegate)
  if (bbs.count(bb) == 0) for_each(it, bb, pred) {
    if (bbs.count(*it) == 0) {
      errs() << "possible extra entry point; stopping expand here" << *bb;
      return false;
    }
  }
  
  auto i = bb->begin();
  while ( i != bb->end() && valid_in_delegate(i,vals) ) {
    errs() << "++" << *i << "\n";
    i++;
  }
  if (i != bb->end()) errs() << "--" << *i << "\n";
  
  if (i == bb->begin()) {
    // unable to consume any instructions
    return false;
  }
  
  // otherwise we know at least this block will be included (though maybe split)
  bbs.insert(bb);
  
  if (i != bb->end()) {
    // stopped in middle of bb
    auto newbb = bb->splitBasicBlock(i, bb->getName()+".dexit");
    exits.insert({newbb, bb});
    
  } else {
    // track exits from *this* bb
    SmallSetVector<std::pair<BasicBlock*,BasicBlock*>,4> local_exits;
    
    // recurse on successors
    for (auto s = succ_begin(bb), se = succ_end(bb); s != se; s++) {
      if (bbs.count(*s) > 0) continue;
      
      if ( !expand(*s) ) {
        local_exits.insert({*s, bb});
      } // else: exits added by recursive `expand` call
    }
    // it's a problem if we have more than one exit from the same bb
    if (local_exits.size() > 1) {
      auto term = bb->getTerminator();
      if (term->getNumSuccessors() == local_exits.size()) {
        // if all the successors were exits, then we can just stop right before this terminator
        auto brbb = bb->splitBasicBlock(term,bb->getName()+".dterm");
        exits.insert({brbb,bb});
        
      } else if (term->getNumSuccessors() == local_exits.size() + 1) {
        // we have to split the branch, use conditional jump
        assert(false && "unhandled case");
      } else {
        // use switch
        assert(false && "unhandled case");
      }
    } else if (local_exits.size() == 1) {
      exits.insert(local_exits[0]);
    }
  }
  
  return true;
}

void DelegateExtractor::print(raw_ostream& o) const {
  o << "///////////////////\n";
  auto loc = (*bbs.begin())->begin()->getDebugLoc();
  o << "// Region @ " << loc.getLine() << "\n";
  o << "gptrs:\n";
  for (auto p : gptrs) o << *p << "\n";
  o << "~~~~~~~~~~~~~~~~~~~";
  o << "\nexits:\n";
  for (auto e : exits) o << "  " << e.first->getName() << " <- " << e.second->getName() << "\n";
  o << "~~~~~~~~~~~~~~~~~~~";
  for (auto bb : bbs) o << *bb;
  o << "^^^^^^^^^^^^^^^^^^^\n";
}

