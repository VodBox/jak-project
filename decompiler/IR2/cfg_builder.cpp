/*!
 * @file cfg_builder.cpp
 * Initial conversion from Control Flow Graph to IR2 Form.
 */

#include "cfg_builder.h"
#include "decompiler/util/MatchParam.h"

namespace decompiler {
namespace {

Form* cfg_to_ir(FormPool& pool, const Function& f, const CfgVtx* vtx);

/*!
 * If it's a form containing multiple elements, return a pointer to the branch element and the end
 * and also a pointer to the Form containing the branch element.
 * Otherwise returns nullptr.  Useful to modify or remove branches found at the end of blocks,
 * and inline things into the begin they were found in.
 */
std::pair<BranchElement*, Form*> get_condition_branch_as_vector(Form* in) {
  // With the current Form setup, we'll never have to dig deper to find the branch.
  // so we can just return the input as the Form*.
  //  If this changes, this can be fixed here, rather than refactoring the whole thing.
  if (in->size() > 1) {
    auto irb = dynamic_cast<BranchElement*>(in->back());
    assert(irb);
    return std::make_pair(irb, in);
  }
  return std::make_pair(nullptr, nullptr);
}

/*!
 * Given an IR, find a branch IR at the end, and also the location of it so it can be patched.
 * Returns nullptr as the first item in the pair if it didn't work.
 * Use this to inspect a sequence ending in branch and have to ability to replace the branch with
 * something else if needed.
 */
std::pair<BranchElement*, FormElement**> get_condition_branch(Form* in) {
  BranchElement* condition_branch = dynamic_cast<BranchElement*>(in->back());
  FormElement** condition_branch_location = in->back_ref();

  if (!condition_branch) {
    auto as_return = dynamic_cast<ReturnElement*>(in->back());
    if (as_return) {
      return get_condition_branch(as_return->dead_code);
    }
  }

  if (!condition_branch) {
    auto as_break = dynamic_cast<BreakElement*>(in->back());
    if (as_break) {
      return get_condition_branch(as_break->dead_code);
    }
  }
  return std::make_pair(condition_branch, condition_branch_location);
}

/*!
 * Given a CondWithElse IR, remove the internal branches and set the condition to be an actual
 * compare IR instead of a branch.
 * Doesn't "rebalance" the leading condition because this runs way before expression compaction.
 */
void clean_up_cond_with_else(FormPool& pool, FormElement* ir) {
  auto cwe = dynamic_cast<CondWithElseElement*>(ir);
  assert(cwe);
  for (auto& e : cwe->entries) {
    // don't reclean already cleaned things.
    if (e.cleaned) {
      continue;
    }
    auto jump_to_next = get_condition_branch(e.condition);
    assert(jump_to_next.first);
    assert(jump_to_next.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
    // patch the branch to next with a condition.
    auto replacement = jump_to_next.first->op()->condition().get_as_form(pool);
    replacement->invert();
    *(jump_to_next.second) = replacement;

    // check the jump at the end of a block.
    auto jump_to_end = get_condition_branch(e.body);
    assert(jump_to_end.first);
    assert(jump_to_end.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
    assert(jump_to_end.first->op()->condition().kind() == IR2_Condition::Kind::ALWAYS);

    // if possible, we just want to remove this from the sequence its in.
    // but sometimes there's a case with nothing in it so there is no sequence.
    // in this case, we can just replace the branch with a NOP IR to indicate that nothing
    // happens in this case, but there was still GOAL code to test for it.
    // this happens rarely, as you would expect.
    auto as_end_of_sequence = get_condition_branch_as_vector(e.body);
    if (as_end_of_sequence.first) {
      assert(as_end_of_sequence.second->size() > 1);
      as_end_of_sequence.second->pop_back();
    } else {
      // we need to have _something_ as the body, so we just put an (empty).
      *(jump_to_end.second) = pool.alloc_element<EmptyElement>();
    }
    e.cleaned = true;
  }
}

/*!
 * Replace the branch at the end of an until loop's condition with a condition.
 */
void clean_up_until_loop(FormPool& pool, UntilElement* ir) {
  auto condition_branch = get_condition_branch(ir->condition);
  assert(condition_branch.first);
  assert(condition_branch.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
  auto replacement = condition_branch.first->op()->condition().get_as_form(pool);
  replacement->invert();
  *(condition_branch.second) = replacement;
}

/*!
 * Remove the true branch at the end of an infinite while loop.
 */
void clean_up_infinite_while_loop(FormPool& pool, WhileElement* ir) {
  auto jump = get_condition_branch(ir->body);
  assert(jump.first);
  assert(jump.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
  assert(jump.first->op()->condition().kind() == IR2_Condition::Kind::ALWAYS);
  auto as_end_of_sequence = get_condition_branch_as_vector(ir->body);
  if (as_end_of_sequence.first) {
    // there's more in the sequence, just remove the last thing.
    assert(as_end_of_sequence.second->size() > 1);
    as_end_of_sequence.second->pop_back();
  } else {
    // Nothing else in the sequence, just replace the jump with an (empty)
    *(jump.second) = pool.alloc_element<EmptyElement>();
  }
  ir->cleaned = true;  // so we don't try this later...
}

/*!
 * Remove the branch in a return statement
 */
void clean_up_return(FormPool& pool, ReturnElement* ir) {
  auto jump_to_end = get_condition_branch(ir->return_code);
  assert(jump_to_end.first);
  assert(jump_to_end.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
  assert(jump_to_end.first->op()->condition().kind() == IR2_Condition::Kind::ALWAYS);
  auto as_end_of_sequence = get_condition_branch_as_vector(ir->return_code);
  if (as_end_of_sequence.first) {
    assert(as_end_of_sequence.second->size() > 1);
    as_end_of_sequence.second->pop_back();
  } else {
    *(jump_to_end.second) = pool.alloc_element<EmptyElement>();
  }
}

/*!
 * Remove the branch in a break (really return-from nonfunction scope)
 */
void clean_up_break(FormPool& pool, BreakElement* ir) {
  auto jump_to_end = get_condition_branch(ir->return_code);
  assert(jump_to_end.first);
  assert(jump_to_end.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
  assert(jump_to_end.first->op()->condition().kind() == IR2_Condition::Kind::ALWAYS);
  auto as_end_of_sequence = get_condition_branch_as_vector(ir->return_code);
  if (as_end_of_sequence.first) {
    assert(as_end_of_sequence.second->size() > 1);
    as_end_of_sequence.second->pop_back();
  } else {
    *(jump_to_end.second) = pool.alloc_element<EmptyElement>();
  }
}

/*!
 * Does the instruction in the delay slot set a register to false?
 * Note. a beql s7, x followed by a or y, x, r0 will count as this. I don't know why but
 * GOAL does this on comparisons to false.
 */
bool delay_slot_sets_false(BranchElement* branch) {
  if (branch->op()->branch_delay().kind() == IR2_BranchDelay::Kind::SET_REG_FALSE) {
    return true;
  }

  if (branch->op()->condition().kind() == IR2_Condition::Kind::FALSE &&
      branch->op()->branch_delay().kind() == IR2_BranchDelay::Kind::SET_REG_REG) {
    auto& cond = branch->op()->condition();
    auto& delay = branch->op()->branch_delay();
    auto cond_reg = cond.src(0).var().reg();
    auto src_reg = delay.var(1).reg();
    return cond_reg == src_reg;
  }

  return false;
}

/*!
 * Does the instruction in the delay slot set a register to a truthy value, like in a GOAL
 * or form branch?  Either it explicitly sets #t, or it tests the value for being not false,
 * then uses that
 */
bool delay_slot_sets_truthy(BranchElement* branch) {
  if (branch->op()->branch_delay().kind() == IR2_BranchDelay::Kind::SET_REG_TRUE) {
    return true;
  }

  if (branch->op()->condition().kind() == IR2_Condition::Kind::TRUTHY &&
      branch->op()->branch_delay().kind() == IR2_BranchDelay::Kind::SET_REG_REG) {
    auto& cond = branch->op()->condition();
    auto& delay = branch->op()->branch_delay();
    auto cond_reg = cond.src(0).var().reg();
    auto src_reg = delay.var(1).reg();
    return cond_reg == src_reg;
  }

  return false;
}

/*!
 * Try to convert a short circuit to an and.
 */
bool try_clean_up_sc_as_and(FormPool& pool, const Function& func, ShortCircuitElement* ir) {
  Register destination;
  Variable ir_dest;
  for (int i = 0; i < int(ir->entries.size()) - 1; i++) {
    auto branch = get_condition_branch(ir->entries.at(i).condition);
    assert(branch.first);
    if (!delay_slot_sets_false(branch.first)) {
      return false;
    }

    if (i == 0) {
      // first case, remember the destination
      ir_dest = branch.first->op()->branch_delay().var(0);
      destination = ir_dest.reg();
    } else {
      // check destination against the first case.
      if (destination != branch.first->op()->branch_delay().var(0).reg()) {
        return false;
      }
    }
  }

  ir->kind = ShortCircuitElement::AND;
  ir->final_result = ir_dest;

  bool live_out_result = false;

  // now get rid of the branches
  for (int i = 0; i < int(ir->entries.size()) - 1; i++) {
    auto branch = get_condition_branch(ir->entries.at(i).condition);
    assert(branch.first);

    if (func.ir2.has_reg_use) {
      auto& branch_info = func.ir2.reg_use.op.at(branch.first->op()->op_id());

      if (i == 0) {
        live_out_result = (branch_info.written_and_unused.find(ir_dest.reg()) ==
                           branch_info.written_and_unused.end());
      } else {
        bool this_live_out = (branch_info.written_and_unused.find(ir_dest.reg()) ==
                              branch_info.written_and_unused.end());
        if (live_out_result != this_live_out) {
          lg::error("Bad live out result on {}. At 0 was {} now at {} is {}",
                    func.guessed_name.to_string(), live_out_result, i, this_live_out);
        }
        assert(live_out_result == this_live_out);
      }
    }

    auto replacement = branch.first->op()->condition().get_as_form(pool);
    replacement->invert();
    *(branch.second) = replacement;
  }

  ir->used_as_value = live_out_result;
  return true;
}

/*!
 * Try to convert a short circuit to an or.
 * Note - this will convert an and to a very strange or, so always use the try as and first.
 */
bool try_clean_up_sc_as_or(FormPool& pool, const Function& func, ShortCircuitElement* ir) {
  Register destination;
  Variable ir_dest;
  for (int i = 0; i < int(ir->entries.size()) - 1; i++) {
    auto branch = get_condition_branch(ir->entries.at(i).condition);
    assert(branch.first);
    if (!delay_slot_sets_truthy(branch.first)) {
      return false;
    }
    if (i == 0) {
      // first case, remember the destination
      ir_dest = branch.first->op()->branch_delay().var(0);
      destination = ir_dest.reg();
    } else {
      // check destination against the first case.
      if (destination != branch.first->op()->branch_delay().var(0).reg()) {
        return false;
      }
    }
  }

  ir->kind = ShortCircuitElement::OR;
  ir->final_result = ir_dest;

  bool live_out_result = false;

  for (int i = 0; i < int(ir->entries.size()) - 1; i++) {
    auto branch = get_condition_branch(ir->entries.at(i).condition);
    assert(branch.first);

    if (func.ir2.has_reg_use) {
      auto& branch_info = func.ir2.reg_use.op.at(branch.first->op()->op_id());

      if (i == 0) {
        live_out_result = (branch_info.written_and_unused.find(ir_dest.reg()) ==
                           branch_info.written_and_unused.end());
      } else {
        bool this_live_out = (branch_info.written_and_unused.find(ir_dest.reg()) ==
                              branch_info.written_and_unused.end());
        assert(live_out_result == this_live_out);
      }
    }

    auto replacement = branch.first->op()->condition().get_as_form(pool);
    *(branch.second) = replacement;
  }

  ir->used_as_value = live_out_result;
  return true;
}

void clean_up_sc(FormPool& pool, const Function& func, ShortCircuitElement* ir);

/*!
 * A form like (and x (or y z)) will be recognized as a single SC Vertex by the CFG pass.
 * In the case where we fail to clean it up as an AND or an OR, we should attempt splitting.
 * Part of the complexity here is that we want to clean up the split recursively so things like
 * (and x (or y (and a b)))
 * or
 * (and x (or y (and a b)) c d (or z))
 * will work correctly.  This may require doing more splitting on both sections!
 */
bool try_splitting_nested_sc(FormPool& pool, const Function& func, ShortCircuitElement* ir) {
  auto first_branch = get_condition_branch(ir->entries.front().condition);
  assert(first_branch.first);
  bool first_is_and = delay_slot_sets_false(first_branch.first);
  bool first_is_or = delay_slot_sets_truthy(first_branch.first);
  assert(first_is_and != first_is_or);  // one or the other but not both!

  int first_different = -1;  // the index of the first one that's different.

  for (int i = 1; i < int(ir->entries.size()) - 1; i++) {
    auto branch = get_condition_branch(ir->entries.at(i).condition);
    assert(branch.first);
    bool is_and = delay_slot_sets_false(branch.first);
    bool is_or = delay_slot_sets_truthy(branch.first);
    assert(is_and != is_or);

    if (first_different == -1) {
      // haven't seen a change yet.
      if (first_is_and != is_and) {
        // change!
        first_different = i;
        break;
      }
    }
  }

  assert(first_different != -1);

  std::vector<ShortCircuitElement::Entry> nested_ir;
  for (int i = first_different; i < int(ir->entries.size()); i++) {
    nested_ir.push_back(ir->entries.at(i));
  }

  auto s = int(ir->entries.size());
  for (int i = first_different; i < s; i++) {
    ir->entries.pop_back();
  }

  // nested_sc has no parent yet.
  auto nested_sc = pool.alloc_element<ShortCircuitElement>(nested_ir);
  clean_up_sc(pool, func, nested_sc);

  // the real trick
  ShortCircuitElement::Entry nested_entry;
  // sets both parents
  nested_entry.condition = pool.alloc_single_form(ir, nested_sc);
  ir->entries.push_back(nested_entry);

  clean_up_sc(pool, func, ir);

  return true;
}

/*!
 * Try to clean up a single short circuit IR. It may get split up into nested IR_ShortCircuits
 * if there is a case like (and a (or b c))
 */
void clean_up_sc(FormPool& pool, const Function& func, ShortCircuitElement* ir) {
  assert(ir->entries.size() > 1);
  if (!try_clean_up_sc_as_and(pool, func, ir)) {
    if (!try_clean_up_sc_as_or(pool, func, ir)) {
      if (!try_splitting_nested_sc(pool, func, ir)) {
        assert(false);
      }
    }
  }
}

const SimpleAtom* get_atom_src(const Form* form) {
  auto* elt = form->try_as_single_element();
  if (elt) {
    auto* as_expr = dynamic_cast<SimpleExpressionElement*>(elt);
    if (as_expr) {
      if (as_expr->expr().is_identity()) {
        return &as_expr->expr().get_arg(0);
      }
    }
  }
  return nullptr;
}

/*!
 * A GOAL comparison which produces a boolean is recognized as a cond-no-else by the CFG analysis.
 * But it should not be decompiled as a branching statement.
 * This either succeeds or asserts and must be called with with something that can be converted
 * successfully
 */
void convert_cond_no_else_to_compare(FormPool& pool,
                                     const Function& f,
                                     FormElement** ir_loc,
                                     Form* parent_form) {
  CondNoElseElement* cne = dynamic_cast<CondNoElseElement*>(*ir_loc);
  assert(cne);
  auto condition = get_condition_branch(cne->entries.front().condition);
  assert(condition.first);
  auto body = dynamic_cast<SetVarElement*>(cne->entries.front().body->try_as_single_element());
  assert(body);
  auto dst = body->dst();
  auto src_atom = get_atom_src(body->src());
  assert(src_atom);
  assert(src_atom->is_sym_ptr());
  assert(src_atom->get_str() == "#f");
  assert(cne->entries.size() == 1);

  auto condition_as_single =
      dynamic_cast<BranchElement*>(cne->entries.front().condition->try_as_single_element());
  auto condition_replacement = condition.first->op()->condition().get_as_form(pool);
  auto crf = pool.alloc_single_form(nullptr, condition_replacement);
  auto replacement = pool.alloc_element<SetVarElement>(dst, crf, true);
  replacement->parent_form = cne->parent_form;

  if (condition_as_single) {
    *ir_loc = replacement;
  } else {
    //    lg::error("Weird case in {}", f.guessed_name.to_string());
    (void)f;
    auto seq = cne->entries.front().condition;
    seq->pop_back();
    seq->push_back(replacement);

    parent_form->pop_back();
    for (auto& x : seq->elts()) {
      parent_form->push_back(x);
    }
    //        auto condition_as_seq = dynamic_cast<IR_Begin*>(cne->entries.front().condition.get());
    //        assert(condition_as_seq);
    //        if (condition_as_seq) {
    //          auto replacement = std::make_shared<IR_Begin>();
    //          replacement->forms = condition_as_seq->forms;
    //          assert(condition.second == &condition_as_seq->forms.back());
    //          replacement->forms.pop_back();
    //          replacement->forms.push_back(std::make_shared<IR_Set>(
    //              IR_Set::REG_64, dst,
    //              std::make_shared<IR_Compare>(condition.first->condition, condition.first)));
    //          *ir = replacement;
    //        }
  }
}

void clean_up_cond_no_else_final(const Function& func, CondNoElseElement* cne) {
  for (size_t idx = 0; idx < cne->entries.size(); idx++) {
    auto& entry = cne->entries.at(idx);
    if (entry.false_destination.has_value()) {
      auto fr = entry.false_destination;
      assert(fr.has_value());
      cne->final_destination = fr->reg();
    } else {
      assert(false);
    }
  }

  auto last_branch = dynamic_cast<BranchElement*>(cne->entries.back().original_condition_branch);
  assert(last_branch);

  if (func.ir2.has_reg_use) {
    auto& last_branch_info = func.ir2.reg_use.op.at(last_branch->op()->op_id());
    cne->used_as_value = last_branch_info.written_and_unused.find(cne->final_destination) ==
                         last_branch_info.written_and_unused.end();
  }

  // check that all other delay slot writes are unused.
  for (size_t i = 0; i < cne->entries.size() - 1; i++) {
    if (func.ir2.has_reg_use) {
      auto branch = dynamic_cast<BranchElement*>(cne->entries.at(i).original_condition_branch);
      auto& branch_info_i = func.ir2.reg_use.op.at(branch->op()->op_id());
      auto reg = cne->entries.at(i).false_destination;
      assert(reg.has_value());
      assert(branch);
      assert(branch_info_i.written_and_unused.find(reg->reg()) !=
             branch_info_i.written_and_unused.end());
    }
  }
}

/*!
 * Replace internal branches inside a CondNoElse IR.
 * If possible will simplify the entire expression into a comparison operation if possible.
 * Will record which registers are set to false in branch delay slots.
 * The exact behavior here isn't really clear to me. It's possible that these delay set false
 * were disabled in cases where the result of the cond was none, or was a number or something.
 * But it generally seems inconsistent.  The expression propagation step will have to deal with
 * this.
 */
void clean_up_cond_no_else(FormPool& pool,
                           const Function& f,
                           FormElement** ir_loc,
                           Form* parent_form) {
  auto cne = dynamic_cast<CondNoElseElement*>(*ir_loc);
  assert(cne);
  for (size_t idx = 0; idx < cne->entries.size(); idx++) {
    auto& e = cne->entries.at(idx);
    if (e.cleaned) {
      continue;
    }

    auto jump_to_next = get_condition_branch(e.condition);
    assert(jump_to_next.first);

    if (jump_to_next.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::SET_REG_TRUE &&
        cne->entries.size() == 1) {
      convert_cond_no_else_to_compare(pool, f, ir_loc, parent_form);
      return;
    } else {
      assert(jump_to_next.first->op()->branch_delay().kind() ==
                 IR2_BranchDelay::Kind::SET_REG_FALSE ||
             jump_to_next.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
      assert(jump_to_next.first->op()->condition().kind() != IR2_Condition::Kind::ALWAYS);

      if (jump_to_next.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::SET_REG_FALSE) {
        assert(!e.false_destination);
        e.false_destination = jump_to_next.first->op()->branch_delay().var(0);
        assert(e.false_destination);
      }

      e.original_condition_branch = *jump_to_next.second;

      auto replacement = jump_to_next.first->op()->condition().get_as_form(pool);
      replacement->invert();
      *(jump_to_next.second) = replacement;
      e.cleaned = true;

      if (idx != cne->entries.size() - 1) {
        auto jump_to_end = get_condition_branch(e.body);
        assert(jump_to_end.first);
        assert(jump_to_end.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
        assert(jump_to_end.first->op()->condition().kind() == IR2_Condition::Kind::ALWAYS);
        auto as_end_of_sequence = get_condition_branch_as_vector(e.body);
        if (as_end_of_sequence.first) {
          assert(as_end_of_sequence.second->size() > 1);
          as_end_of_sequence.second->pop_back();
        } else {
          *(jump_to_end.second) = pool.alloc_element<EmptyElement>();
        }
      }
    }
  }
}

/*!
 * Match for a (set! reg (math reg reg)) form
 */
bool is_op_3(FormElement* ir,
             MatchParam<SimpleExpression::Kind> kind,
             MatchParam<Register> dst,
             MatchParam<Register> src0,
             MatchParam<Register> src1,
             Register* dst_out = nullptr,
             Register* src0_out = nullptr,
             Register* src1_out = nullptr) {
  // should be a set reg to int math 2 ir
  auto set = dynamic_cast<SetVarElement*>(ir);
  if (!set) {
    return false;
  }

  // destination should be a register
  auto dest = set->dst();
  if (dst != dest.reg()) {
    return false;
  }

  auto math = dynamic_cast<const SimpleExpressionElement*>(set->src()->try_as_single_element());
  if (!math || kind != math->expr().kind()) {
    return false;
  }

  if (get_simple_expression_arg_count(math->expr().kind()) != 2) {
    return false;
  }

  auto arg0 = math->expr().get_arg(0);
  auto arg1 = math->expr().get_arg(1);

  if (!arg0.is_var() || src0 != arg0.var().reg() || !arg1.is_var() || src1 != arg1.var().reg()) {
    return false;
  }

  // it's a match!
  if (dst_out) {
    *dst_out = dest.reg();
  }

  if (src0_out) {
    *src0_out = arg0.var().reg();
  }

  if (src1_out) {
    *src1_out = arg1.var().reg();
  }
  return true;
}

bool is_op_2(FormElement* ir,
             MatchParam<SimpleExpression::Kind> kind,
             MatchParam<Register> dst,
             MatchParam<Register> src0,
             Register* dst_out = nullptr,
             Register* src0_out = nullptr) {
  // should be a set reg to int math 2 ir
  auto set = dynamic_cast<SetVarElement*>(ir);
  if (!set) {
    return false;
  }

  // destination should be a register
  auto dest = set->dst();
  if (dst != dest.reg()) {
    return false;
  }

  auto math = dynamic_cast<const SimpleExpressionElement*>(set->src()->try_as_single_element());
  if (!math || kind != math->expr().kind()) {
    return false;
  }

  auto arg = math->expr().get_arg(0);

  if (!arg.is_var() || src0 != arg.var().reg()) {
    return false;
  }

  // it's a match!
  if (dst_out) {
    *dst_out = dest.reg();
  }

  if (src0_out) {
    *src0_out = arg.var().reg();
  }

  return true;
}

/*!
 * Try to convert this SC Vertex into an abs (integer).
 * Will return a converted abs IR if successful, or nullptr if its not possible
 */
Form* try_sc_as_abs(FormPool& pool, const Function& f, const ShortCircuit* vtx) {
  if (vtx->entries.size() != 1) {
    return nullptr;
  }

  auto b0 = dynamic_cast<BlockVtx*>(vtx->entries.at(0));
  if (!b0) {
    return nullptr;
  }

  auto b0_ptr = cfg_to_ir(pool, f, b0);
  //  auto b0_ir = dynamic_cast<IR_Begin*>(b0_ptr.get());

  BranchElement* branch = dynamic_cast<BranchElement*>(b0_ptr->back());

  if (!branch) {
    return nullptr;
  }

  // check the branch instruction
  if (!branch->op()->likely() ||
      branch->op()->condition().kind() != IR2_Condition::Kind::LESS_THAN_ZERO_SIGNED ||
      branch->op()->branch_delay().kind() != IR2_BranchDelay::Kind::NEGATE) {
    // todo - if there was an abs(unsigned), it would be missed here.
    return nullptr;
  }

  auto input = branch->op()->condition().src(0);
  auto output = branch->op()->branch_delay().var(0);

  assert(input.is_var());
  assert(input.var().reg() == branch->op()->branch_delay().var(1).reg());

  // remove the branch
  b0_ptr->pop_back();
  // add the ash
  auto src_var = pool.alloc_single_element_form<SimpleAtomElement>(nullptr, input);
  auto src_abs = pool.alloc_single_element_form<AbsElement>(nullptr, src_var);
  auto replacement = pool.alloc_element<SetVarElement>(output, src_abs, true);
  b0_ptr->push_back(replacement);

  return b0_ptr;
}

/*!
 * Attempt to convert a short circuit expression into an arithmetic shift.
 * GOAL's shift function accepts positive/negative numbers to determine the direction
 * of the shift.
 */
Form* try_sc_as_ash(FormPool& pool, const Function& f, const ShortCircuit* vtx) {
  if (vtx->entries.size() != 2) {
    return nullptr;
  }

  // todo, I think b0 could possibly be something more complicated, depending on how we order.
  auto b0 = dynamic_cast<CfgVtx*>(vtx->entries.at(0));
  auto b1 = dynamic_cast<BlockVtx*>(vtx->entries.at(1));
  if (!b0 || !b1) {
    return nullptr;
  }

  auto b0_ptr = cfg_to_ir(pool, f, b0);
  auto b1_ptr = cfg_to_ir(pool, f, b1);

  auto branch = dynamic_cast<BranchElement*>(b0_ptr->back());
  if (!branch || b1_ptr->size() != 2) {
    return nullptr;
  }

  // check the branch instruction
  if (!branch->op()->likely() ||
      branch->op()->condition().kind() != IR2_Condition::Kind::GEQ_ZERO_SIGNED ||
      branch->op()->branch_delay().kind() != IR2_BranchDelay::Kind::DSLLV) {
    return nullptr;
  }

  /*
   *  bgezl s5, L109    ; s5 is the shift amount
      dsllv a0, a0, s5  ; a0 is both input and output here

      dsubu a1, r0, s5  ; a1 is a temp here
      dsrav a0, a0, a1  ; a0 is both input and output here
   */

  auto sa_in = branch->op()->condition().src(0);
  assert(sa_in.is_var());
  auto result = branch->op()->branch_delay().var(0);
  auto value_in = branch->op()->branch_delay().var(1);
  auto sa_in2 = branch->op()->branch_delay().var(2);
  assert(sa_in.var().reg() == sa_in2.reg());

  auto dsubu_candidate = b1_ptr->at(0);
  auto dsrav_candidate = b1_ptr->at(1);

  Register clobber;
  if (!is_op_2(dsubu_candidate, SimpleExpression::Kind::NEG, {}, sa_in.var().reg(), &clobber)) {
    return nullptr;
  }

  bool is_arith = is_op_3(dsrav_candidate, SimpleExpression::Kind::RIGHT_SHIFT_ARITH, result.reg(),
                          value_in.reg(), clobber);
  bool is_logical = is_op_3(dsrav_candidate, SimpleExpression::Kind::RIGHT_SHIFT_LOGIC,
                            result.reg(), value_in.reg(), clobber);

  if (!is_arith && !is_logical) {
    return nullptr;
  }

  std::optional<Variable> clobber_ir;
  auto dsubu_set = dynamic_cast<SetVarElement*>(dsubu_candidate);
  auto dsrav_set = dynamic_cast<SetVarElement*>(dsrav_candidate);
  assert(dsubu_set && dsrav_set);
  if (clobber != result.reg()) {
    clobber_ir = dsubu_set->dst();
  }

  Variable dest_ir = branch->op()->branch_delay().var(0);
  SimpleAtom shift_ir = branch->op()->condition().src(0);
  auto value_ir =
      dynamic_cast<const SimpleExpressionElement*>(dsrav_set->src()->try_as_single_element())
          ->expr()
          .get_arg(0);

  // remove the branch
  b0_ptr->pop_back();

  // setup
  auto value_form = pool.alloc_single_element_form<SimpleAtomElement>(nullptr, value_ir);
  auto shift_form = pool.alloc_single_element_form<SimpleAtomElement>(nullptr, shift_ir);
  auto ash_form = pool.alloc_single_element_form<AshElement>(nullptr, shift_form, value_form,
                                                             clobber_ir, is_arith);
  auto set_form = pool.alloc_element<SetVarElement>(dest_ir, ash_form, true);
  b0_ptr->push_back(set_form);

  return b0_ptr;
}

/*!
 * Try to convert a short circuiting expression into a "type-of" expression.
 * We do this before attempting the normal and/or expressions.
 */
Form* try_sc_as_type_of(FormPool& pool, const Function& f, const ShortCircuit* vtx) {
  // the assembly looks like this:
  /*
         dsll32 v1, a0, 29                   ;; (set! v1 (shl a0 61))
         beql v1, r0, L60                    ;; (bl! (= v1 r0) L60 (unknown-branch-delay))
         lw v1, binteger(s7)

         bgtzl v1, L60                       ;; (bl! (>0.s v1) L60 (unknown-branch-delay))
         lw v1, pair(s7)

         lwu v1, -4(a0)                      ;; (set! v1 (l.wu (+.i a0 -4)))
     L60:
   */

  // some of these checks may be a little bit overkill but it's a nice way to sanity check that
  // we have actually decoded everything correctly.
  if (vtx->entries.size() != 3) {
    return nullptr;
  }

  auto b0 = dynamic_cast<CfgVtx*>(vtx->entries.at(0));
  auto b1 = dynamic_cast<BlockVtx*>(vtx->entries.at(1));
  auto b2 = dynamic_cast<BlockVtx*>(vtx->entries.at(2));

  if (!b0 || !b1 || !b2) {
    return nullptr;
  }

  auto b0_ptr = cfg_to_ir(pool, f, b0);  // should be begin.
  if (b0_ptr->size() <= 1) {
    return nullptr;
  }

  auto b1_ptr = cfg_to_ir(pool, f, b1);
  auto b1_ir = dynamic_cast<BranchElement*>(b1_ptr->try_as_single_element());

  auto b2_ptr = cfg_to_ir(pool, f, b2);
  auto b2_ir = dynamic_cast<SetVarElement*>(b2_ptr->try_as_single_element());
  if (!b1_ir || !b2_ir) {
    return nullptr;
  }

  auto set_shift = dynamic_cast<SetVarElement*>(b0_ptr->at(b0_ptr->size() - 2));
  if (!set_shift) {
    return nullptr;
  }

  auto temp_reg0 = set_shift->dst();

  auto shift = dynamic_cast<SimpleExpressionElement*>(set_shift->src()->try_as_single_element());
  if (!shift || shift->expr().kind() != SimpleExpression::Kind::LEFT_SHIFT) {
    return nullptr;
  }
  auto src_reg = shift->expr().get_arg(0).var();
  auto sa = shift->expr().get_arg(1);
  if (!sa.is_int() || sa.get_int() != 61) {
    return nullptr;
  }

  auto first_branch = dynamic_cast<BranchElement*>(b0_ptr->back());
  auto second_branch = b1_ir;
  auto else_case = b2_ir;

  if (!first_branch ||
      first_branch->op()->branch_delay().kind() != IR2_BranchDelay::Kind::SET_BINTEGER ||
      first_branch->op()->condition().kind() != IR2_Condition::Kind::ZERO ||
      !first_branch->op()->likely()) {
    return nullptr;
  }
  auto temp_reg = first_branch->op()->condition().src(0).var();
  assert(temp_reg.reg() == temp_reg0.reg());
  auto dst_reg = first_branch->op()->branch_delay().var(0);

  if (!second_branch ||
      second_branch->op()->branch_delay().kind() != IR2_BranchDelay::Kind::SET_PAIR ||
      second_branch->op()->condition().kind() != IR2_Condition::Kind::GREATER_THAN_ZERO_SIGNED ||
      !second_branch->op()->likely()) {
    return nullptr;
  }

  // check we agree on destination register.
  auto dst_reg2 = second_branch->op()->branch_delay().var(0);
  assert(dst_reg2.reg() == dst_reg.reg());

  // else case is a lwu to grab the type from a basic
  assert(else_case);
  auto dst_reg3 = else_case->dst();
  assert(dst_reg3.reg() == dst_reg.reg());
  auto load_op = dynamic_cast<LoadSourceElement*>(else_case->src()->try_as_single_element());
  if (!load_op || load_op->kind() != LoadVarOp::Kind::UNSIGNED || load_op->size() != 4) {
    return nullptr;
  }
  auto load_loc =
      dynamic_cast<SimpleExpressionElement*>(load_op->location()->try_as_single_element());
  if (!load_loc || load_loc->expr().kind() != SimpleExpression::Kind::ADD) {
    return nullptr;
  }
  auto src_reg3 = load_loc->expr().get_arg(0);
  auto offset = load_loc->expr().get_arg(1);
  if (!src_reg3.is_var() || !offset.is_int()) {
    return nullptr;
  }

  assert(src_reg3.var().reg() == src_reg.reg());
  assert(offset.get_int() == -4);

  std::optional<Variable> clobber;
  if (temp_reg.reg() != dst_reg.reg()) {
    clobber = first_branch->op()->condition().src(0).var();
  }

  // remove the branch
  b0_ptr->pop_back();
  // remove the shift
  b0_ptr->pop_back();

  auto obj = pool.alloc_single_element_form<SimpleAtomElement>(nullptr, shift->expr().get_arg(0));
  auto type_op = pool.alloc_single_element_form<TypeOfElement>(nullptr, obj, clobber);
  auto op = pool.alloc_element<SetVarElement>(else_case->dst(), type_op, true);
  b0_ptr->push_back(op);
  // add the type-of

  return b0_ptr;
}

Form* merge_cond_else_with_sc_cond(FormPool& pool,
                                   const Function& f,
                                   const CondWithElse* cwe,
                                   Form* else_ir) {
  if (else_ir->size() != 2) {
    return nullptr;
  }

  auto first = dynamic_cast<ShortCircuitElement*>(else_ir->at(0));
  auto second = dynamic_cast<CondNoElseElement*>(else_ir->at(1));
  if (!first || !second) {
    return nullptr;
  }

  std::vector<CondNoElseElement::Entry> entries;
  for (auto& x : cwe->entries) {
    CondNoElseElement::Entry e;
    e.condition = cfg_to_ir(pool, f, x.condition);
    e.body = cfg_to_ir(pool, f, x.body);
    entries.push_back(std::move(e));
  }

  auto first_condition = pool.alloc_empty_form();
  first_condition->push_back(else_ir->at(0));
  for (auto& x : second->entries.front().condition->elts()) {
    first_condition->push_back(x);
  }

  second->entries.front().condition = first_condition;

  for (auto& x : second->entries) {
    entries.push_back(x);
  }
  auto result = pool.alloc_single_element_form<CondNoElseElement>(nullptr, entries);
  clean_up_cond_no_else(pool, f, result->back_ref(), result);
  return result;
}

void insert_cfg_into_list(FormPool& pool,
                          const Function& f,
                          const CfgVtx* vtx,
                          std::vector<FormElement*>* output) {
  auto as_sequence = dynamic_cast<const SequenceVtx*>(vtx);
  auto as_block = dynamic_cast<const BlockVtx*>(vtx);
  if (as_sequence) {
    // inline the sequence.
    for (auto& x : as_sequence->seq) {
      insert_cfg_into_list(pool, f, x, output);
    }
  } else if (as_block) {
    // inline the ops.
    auto start_op = f.ir2.atomic_ops->block_id_to_first_atomic_op.at(as_block->block_id);
    auto end_op = f.ir2.atomic_ops->block_id_to_end_atomic_op.at(as_block->block_id);
    for (auto i = start_op; i < end_op; i++) {
      output->push_back(f.ir2.atomic_ops->ops.at(i)->get_as_form(pool));
    }
  } else {
    auto ir = cfg_to_ir(pool, f, vtx);
    for (auto x : ir->elts()) {
      output->push_back(x);
    }
  }
}

Form* cfg_to_ir(FormPool& pool, const Function& f, const CfgVtx* vtx) {
  if (dynamic_cast<const BlockVtx*>(vtx)) {
    auto* bv = dynamic_cast<const BlockVtx*>(vtx);

    Form* output = pool.alloc_empty_form();
    auto start_op = f.ir2.atomic_ops->block_id_to_first_atomic_op.at(bv->block_id);
    auto end_op = f.ir2.atomic_ops->block_id_to_end_atomic_op.at(bv->block_id);
    for (auto i = start_op; i < end_op; i++) {
      output->push_back(f.ir2.atomic_ops->ops.at(i)->get_as_form(pool));
    }

    return output;

  } else if (dynamic_cast<const SequenceVtx*>(vtx)) {
    auto* sv = dynamic_cast<const SequenceVtx*>(vtx);
    Form* output = pool.alloc_empty_form();
    insert_cfg_into_list(pool, f, sv, &output->elts());

    return output;
  } else if (dynamic_cast<const WhileLoop*>(vtx)) {
    auto wvtx = dynamic_cast<const WhileLoop*>(vtx);

    return pool.alloc_single_element_form<WhileElement>(
        nullptr, cfg_to_ir(pool, f, wvtx->condition), cfg_to_ir(pool, f, wvtx->body));
  } else if (dynamic_cast<const UntilLoop*>(vtx)) {
    auto wvtx = dynamic_cast<const UntilLoop*>(vtx);
    auto result = pool.alloc_single_element_form<UntilElement>(
        nullptr, cfg_to_ir(pool, f, wvtx->condition), cfg_to_ir(pool, f, wvtx->body));
    clean_up_until_loop(pool, dynamic_cast<UntilElement*>(result->try_as_single_element()));
    return result;
  } else if (dynamic_cast<const UntilLoop_single*>(vtx)) {
    auto wvtx = dynamic_cast<const UntilLoop_single*>(vtx);
    auto empty = pool.alloc_single_element_form<EmptyElement>(nullptr);
    auto result = pool.alloc_single_element_form<UntilElement>(
        nullptr, cfg_to_ir(pool, f, wvtx->block), empty);
    clean_up_until_loop(pool, dynamic_cast<UntilElement*>(result->try_as_single_element()));
    return result;
  } else if (dynamic_cast<const InfiniteLoopBlock*>(vtx)) {
    auto wvtx = dynamic_cast<const InfiniteLoopBlock*>(vtx);
    auto condition = pool.alloc_single_element_form<ConditionElement>(
        nullptr, IR2_Condition::Kind::ALWAYS, nullptr, nullptr);
    auto result = pool.alloc_single_element_form<WhileElement>(nullptr, condition,
                                                               cfg_to_ir(pool, f, wvtx->block));
    clean_up_infinite_while_loop(pool,
                                 dynamic_cast<WhileElement*>(result->try_as_single_element()));
    return result;
  } else if (dynamic_cast<const CondWithElse*>(vtx)) {
    auto* cvtx = dynamic_cast<const CondWithElse*>(vtx);

    // the cfg analysis pass may recognize some things out of order, which can cause
    // fake nesting. This is actually a problem at this point because it can turn a normal
    // cond into a cond with else, which emits different instructions.  This attempts to recognize
    // an else which is actually more cases and compacts it into a single statement.  At this point
    // I don't know if this is sufficient to catch all cases.  it may even recognize the wrong
    // thing in some cases... maybe we should check the delay slot instead?
    auto else_ir = cfg_to_ir(pool, f, cvtx->else_vtx);
    auto fancy_compact_result = merge_cond_else_with_sc_cond(pool, f, cvtx, else_ir);
    if (fancy_compact_result) {
      return fancy_compact_result;
    }

    // this case is disabled because I _think_ it is now properly handled elsewhere.
    if (false /*&& dynamic_cast<IR_Cond*>(else_ir.get())*/) {
      //      auto extra_cond = dynamic_cast<IR_Cond*>(else_ir.get());
      //      std::vector<IR_Cond::Entry> entries;
      //      for (auto& x : cvtx->entries) {
      //        IR_Cond::Entry e;
      //        e.condition = cfg_to_ir(f, file, x.condition);
      //        e.body = cfg_to_ir(f, file, x.body);
      //        entries.push_back(std::move(e));
      //      }
      //      for (auto& x : extra_cond->entries) {
      //        entries.push_back(x);
      //      }
      //      std::shared_ptr<IR> result = std::make_shared<IR_Cond>(entries);
      //      clean_up_cond_no_else(&result, file);
      //      return result;
    } else {
      std::vector<CondWithElseElement::Entry> entries;
      for (auto& x : cvtx->entries) {
        CondWithElseElement::Entry e;
        e.condition = cfg_to_ir(pool, f, x.condition);
        e.body = cfg_to_ir(pool, f, x.body);
        entries.push_back(std::move(e));
      }
      auto result = pool.alloc_single_element_form<CondWithElseElement>(nullptr, entries, else_ir);
      clean_up_cond_with_else(pool,
                              dynamic_cast<CondWithElseElement*>(result->try_as_single_element()));
      return result;
    }
  } else if (dynamic_cast<const ShortCircuit*>(vtx)) {
    auto* svtx = dynamic_cast<const ShortCircuit*>(vtx);
    // try as a type of expression first
    auto as_type_of = try_sc_as_type_of(pool, f, svtx);
    if (as_type_of) {
      return as_type_of;
    }

    auto as_ash = try_sc_as_ash(pool, f, svtx);
    if (as_ash) {
      return as_ash;
    }

    auto as_abs = try_sc_as_abs(pool, f, svtx);
    if (as_abs) {
      return as_abs;
    }

    if (svtx->entries.size() == 1) {
      throw std::runtime_error("Weird short circuit form.");
    }
    // now try as a normal and/or
    std::vector<ShortCircuitElement::Entry> entries;
    for (auto& x : svtx->entries) {
      ShortCircuitElement::Entry e;
      e.condition = cfg_to_ir(pool, f, x);
      entries.push_back(e);
    }
    auto result = pool.alloc_single_element_form<ShortCircuitElement>(nullptr, entries);
    clean_up_sc(pool, f, dynamic_cast<ShortCircuitElement*>(result->try_as_single_element()));
    return result;
  } else if (dynamic_cast<const CondNoElse*>(vtx)) {
    auto* cvtx = dynamic_cast<const CondNoElse*>(vtx);
    std::vector<CondNoElseElement::Entry> entries;
    for (auto& x : cvtx->entries) {
      CondNoElseElement::Entry e;
      e.condition = cfg_to_ir(pool, f, x.condition);
      e.body = cfg_to_ir(pool, f, x.body);
      entries.push_back(std::move(e));
    }
    auto result = pool.alloc_single_element_form<CondNoElseElement>(nullptr, entries);
    clean_up_cond_no_else(pool, f, result->back_ref(), result);
    return result;
  } else if (dynamic_cast<const GotoEnd*>(vtx)) {
    auto* cvtx = dynamic_cast<const GotoEnd*>(vtx);
    auto result = pool.alloc_single_element_form<ReturnElement>(
        nullptr, cfg_to_ir(pool, f, cvtx->body), cfg_to_ir(pool, f, cvtx->unreachable_block));
    clean_up_return(pool, dynamic_cast<ReturnElement*>(result->try_as_single_element()));
    return result;
  } else if (dynamic_cast<const Break*>(vtx)) {
    auto* cvtx = dynamic_cast<const Break*>(vtx);
    auto result = pool.alloc_single_element_form<BreakElement>(
        nullptr, cfg_to_ir(pool, f, cvtx->body), cfg_to_ir(pool, f, cvtx->unreachable_block));
    clean_up_break(pool, dynamic_cast<BreakElement*>(result->try_as_single_element()));
    return result;
  }

  throw std::runtime_error("not yet implemented IR conversion.");
  return nullptr;
}

/*!
 * Post processing pass to clean up while loops - annoyingly the block before a while loop
 * has a jump to the condition branch that we need to remove.  This currently happens after all
 * conversion but this may need to be revisited depending on the final order of simplifications.
 */
void clean_up_while_loops(FormPool& pool, Form* sequence) {
  std::vector<size_t> to_remove;  // the list of branches to remove by index in this sequence
  for (int i = 0; i < sequence->size(); i++) {
    auto* form_as_while = dynamic_cast<WhileElement*>(sequence->at(i));
    if (form_as_while && !form_as_while->cleaned) {
      assert(i != 0);
      auto prev_as_branch = dynamic_cast<BranchElement*>(sequence->at(i - 1));
      assert(prev_as_branch);
      // printf("got while intro branch %s\n", prev_as_branch->print(file).c_str());
      // this should be an always jump. We'll assume that the CFG builder successfully checked
      // the brach destination, but we will check the condition.
      assert(prev_as_branch->op()->condition().kind() == IR2_Condition::Kind::ALWAYS);
      assert(prev_as_branch->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
      to_remove.push_back(i - 1);

      // now we should try to find the condition branch:

      auto condition_branch = get_condition_branch(form_as_while->condition);

      assert(condition_branch.first);
      assert(condition_branch.first->op()->branch_delay().kind() == IR2_BranchDelay::Kind::NOP);
      // printf("got while condition branch %s\n", condition_branch.first->print(file).c_str());
      auto replacement = condition_branch.first->op()->condition().get_as_form(pool);

      *(condition_branch.second) = replacement;
    }
  }

  // remove the implied forward always branches.
  for (int i = int(to_remove.size()); i-- > 0;) {
    auto idx = to_remove.at(i);
    assert(dynamic_cast<BranchElement*>(sequence->at(idx)));
    sequence->elts().erase(sequence->elts().begin() + idx);
  }
}
}  // namespace

void build_initial_forms(Function& function) {
  auto& cfg = function.cfg;
  if (!cfg->is_fully_resolved()) {
    return;
  }

  try {
    auto& pool = function.ir2.form_pool;
    auto top_level = function.cfg->get_single_top_level();
    std::vector<FormElement*> top_level_elts;
    insert_cfg_into_list(pool, function, top_level, &top_level_elts);
    auto result = pool.alloc_sequence_form(nullptr, top_level_elts);

    result->apply_form([&](Form* form) { clean_up_while_loops(pool, form); });

    result->apply([&](FormElement* form) {
      auto as_cne = dynamic_cast<CondNoElseElement*>(form);
      if (as_cne) {
        clean_up_cond_no_else_final(function, as_cne);
      }
    });

    function.ir2.top_form = result;
  } catch (std::runtime_error& e) {
    lg::warn("Failed to build initial forms in {}: {}", function.guessed_name.to_string(),
             e.what());
  }
}
}  // namespace decompiler