/*
 * Copyright © 2018 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include "common/sid.h"

#include <map>
#include <stack>
#include <vector>
#include <optional>

namespace aco {

namespace {

/**
 * The general idea of this pass is:
 * The CFG is traversed in reverse postorder (forward) and loops are processed
 * several times until no progress is made.
 * Per BB two wait_ctx is maintained: an in-context and out-context.
 * The in-context is the joined out-contexts of the predecessors.
 * The context contains a map: gpr -> wait_entry
 * consisting of the information about the cnt values to be waited for.
 * Note: After merge-nodes, it might occur that for the same register
 *       multiple cnt values are to be waited for.
 *
 * The values are updated according to the encountered instructions:
 * - additional events increment the counter of waits of the same type
 * - or erase gprs with counters higher than to be waited for.
 */

// TODO: do a more clever insertion of wait_cnt (lgkm_cnt)
// when there is a load followed by a use of a previous load

/* Instructions of the same event will finish in-order except for smem
 * and maybe flat. Instructions of different events may not finish in-order. */
enum wait_event : uint32_t {
   event_smem = 1 << 0,
   event_lds = 1 << 1,
   event_gds = 1 << 2,
   event_vmem = 1 << 3,
   event_vmem_store = 1 << 4, /* GFX10+ */
   event_flat = 1 << 5,
   event_exp_pos = 1 << 6,
   event_exp_param = 1 << 7,
   event_exp_mrt_null = 1 << 8,
   event_gds_gpr_lock = 1 << 9,
   event_vmem_gpr_lock = 1 << 10,
   event_sendmsg = 1 << 11,
   event_ldsdir = 1 << 12,
   event_vmem_sample = 1 << 13, /* GFX12+ */
   event_vmem_bvh = 1 << 14,    /* GFX12+ */
   event_valu = 1 << 15,
   event_trans = 1 << 16,
   event_salu = 1 << 17,
   num_events = 18,
};

enum counter_type : uint8_t {
   counter_exp = 1 << wait_type_exp,
   counter_lgkm = 1 << wait_type_lgkm,
   counter_vm = 1 << wait_type_vm,
   counter_vs = 1 << wait_type_vs,
   counter_sample = 1 << wait_type_sample,
   counter_bvh = 1 << wait_type_bvh,
   counter_km = 1 << wait_type_km,
   counter_alu = 1 << wait_type_num,
   num_counters = wait_type_num + 1,
   wait_counters = BITFIELD_MASK(wait_type_num),
};

/* On GFX11+ the SIMD frontend doesn't switch to issuing instructions from a different
 * wave if there is an ALU stall. Hence we have an instruction (s_delay_alu) to signal
 * that we should switch to a different wave and contains info on dependencies as to
 * when we can switch back.
 *
 * This seems to apply only for ALU->ALU dependencies as other instructions have better
 * integration with the frontend.
 *
 * Note that if we do not emit s_delay_alu things will still be correct, but the wave
 * will stall in the ALU (and the ALU will be doing nothing else). We'll use this as
 * I'm pretty sure our cycle info is wrong at times (necessarily so, e.g. wave64 VALU
 * instructions can take a different number of cycles based on the exec mask)
 */
struct alu_delay_info {
   /* These are the values directly above the max representable value, i.e. the wait
    * would turn into a no-op when we try to wait for something further back than
    * this.
    */
   static constexpr int8_t valu_nop = 5;
   static constexpr int8_t trans_nop = 4;

   /* How many VALU instructions ago this value was written */
   int8_t valu_instrs = valu_nop;
   /* Cycles until the writing VALU instruction is finished */
   int8_t valu_cycles = 0;

   /* How many Transcedent instructions ago this value was written */
   int8_t trans_instrs = trans_nop;
   /* Cycles until the writing Transcendent instruction is finished */
   int8_t trans_cycles = 0;

   /* Cycles until the writing SALU instruction is finished*/
   int8_t salu_cycles = 0;

   bool combine(const alu_delay_info& other)
   {
      bool changed = other.valu_instrs < valu_instrs || other.trans_instrs < trans_instrs ||
                     other.salu_cycles > salu_cycles || other.valu_cycles > valu_cycles ||
                     other.trans_cycles > trans_cycles;
      valu_instrs = std::min(valu_instrs, other.valu_instrs);
      trans_instrs = std::min(trans_instrs, other.trans_instrs);
      salu_cycles = std::max(salu_cycles, other.salu_cycles);
      valu_cycles = std::max(valu_cycles, other.valu_cycles);
      trans_cycles = std::max(trans_cycles, other.trans_cycles);
      return changed;
   }

   /* Needs to be called after any change to keep the data consistent. */
   void fixup()
   {
      if (valu_instrs >= valu_nop || valu_cycles <= 0) {
         valu_instrs = valu_nop;
         valu_cycles = 0;
      }

      if (trans_instrs >= trans_nop || trans_cycles <= 0) {
         trans_instrs = trans_nop;
         trans_cycles = 0;
      }

      salu_cycles = std::max<int8_t>(salu_cycles, 0);
   }

   /* Returns true if a wait would be a no-op */
   bool empty() const
   {
      return valu_instrs == valu_nop && trans_instrs == trans_nop && salu_cycles == 0;
   }

   UNUSED void print(FILE* output) const
   {
      if (valu_instrs != valu_nop)
         fprintf(output, "valu_instrs: %u\n", valu_instrs);
      if (valu_cycles)
         fprintf(output, "valu_cycles: %u\n", valu_cycles);
      if (trans_instrs != trans_nop)
         fprintf(output, "trans_instrs: %u\n", trans_instrs);
      if (trans_cycles)
         fprintf(output, "trans_cycles: %u\n", trans_cycles);
      if (salu_cycles)
         fprintf(output, "salu_cycles: %u\n", salu_cycles);
   }
};

struct wait_entry {
   wait_imm imm;
   alu_delay_info delay;
   uint32_t events;  /* use wait_event notion */
   uint8_t counters; /* use counter_type notion */
   bool wait_on_read : 1;
   bool logical : 1;
   uint8_t vmem_types : 4; /* use vmem_type notion. for counter_vm. */

   wait_entry(wait_event event_, wait_imm imm_, alu_delay_info delay_, uint8_t counters_,
              bool logical_, bool wait_on_read_)
       : imm(imm_), delay(delay_), events(event_), counters(counters_), wait_on_read(wait_on_read_),
         logical(logical_), vmem_types(0)
   {}

   bool join(const wait_entry& other)
   {
      bool changed = (other.events & ~events) || (other.counters & ~counters) ||
                     (other.wait_on_read && !wait_on_read) || (other.vmem_types & !vmem_types) ||
                     (!other.logical && logical);
      events |= other.events;
      counters |= other.counters;
      changed |= imm.combine(other.imm);
      changed |= delay.combine(other.delay);
      wait_on_read |= other.wait_on_read;
      vmem_types |= other.vmem_types;
      logical &= other.logical;
      return changed;
   }

   void remove_alu_counter()
   {
      counters &= ~counter_alu;
      delay = alu_delay_info();
      events &= ~(event_valu | event_trans | event_salu);
   }

   void remove_wait(wait_type type, uint32_t type_events)
   {
      counters &= ~(1 << type);
      imm[type] = wait_imm::unset_counter;

      events &= ~type_events | event_flat;
      if (!(counters & counter_lgkm) && !(counters & counter_vm))
         events &= ~(type_events & event_flat);

      if (type == wait_type_vm)
         vmem_types = 0;
   }

   UNUSED void print(FILE* output) const
   {
      fprintf(output, "logical: %u\n", logical);
      imm.print(output);
      delay.print(output);
      if (events)
         fprintf(output, "events: %u\n", events);
      if (counters)
         fprintf(output, "counters: %u\n", counters);
      if (!wait_on_read)
         fprintf(output, "wait_on_read: %u\n", wait_on_read);
      if (!logical)
         fprintf(output, "logical: %u\n", logical);
      if (vmem_types)
         fprintf(output, "vmem_types: %u\n", vmem_types);
   }
};

struct target_info {
   wait_imm max_cnt;
   uint32_t events[wait_type_num] = {};
   uint16_t unordered_events;

   target_info(enum amd_gfx_level gfx_level)
   {
      max_cnt = wait_imm::max(gfx_level);
      for (unsigned i = 0; i < wait_type_num; i++)
         max_cnt[i] = max_cnt[i] ? max_cnt[i] - 1 : 0;

      events[wait_type_exp] = event_exp_pos | event_exp_param | event_exp_mrt_null |
                              event_gds_gpr_lock | event_vmem_gpr_lock | event_ldsdir;
      events[wait_type_lgkm] = event_smem | event_lds | event_gds | event_flat | event_sendmsg;
      events[wait_type_vm] = event_vmem | event_flat;
      events[wait_type_vs] = event_vmem_store;
      if (gfx_level >= GFX12) {
         events[wait_type_sample] = event_vmem_sample;
         events[wait_type_bvh] = event_vmem_bvh;
         events[wait_type_km] = event_smem | event_sendmsg;
         events[wait_type_lgkm] &= ~events[wait_type_km];
      }

      for (unsigned i = 0; i < wait_type_num; i++) {
         u_foreach_bit (j, events[i])
            counters[j] |= (1 << i);
      }
      counters[ffs(event_valu) - 1] |= counter_alu;
      counters[ffs(event_trans) - 1] |= counter_alu;
      counters[ffs(event_salu) - 1] |= counter_alu;

      unordered_events = event_smem | (gfx_level < GFX10 ? event_flat : 0);
   }

   uint8_t get_counters_for_event(wait_event event) const { return counters[ffs(event) - 1]; }

private:
   /* Bitfields of counters affected by each event */
   uint8_t counters[num_events] = {};
};

struct wait_ctx {
   Program* program;
   enum amd_gfx_level gfx_level;
   const target_info* info;

   uint32_t nonzero = 0;
   bool pending_flat_lgkm = false;
   bool pending_flat_vm = false;
   bool pending_s_buffer_store = false; /* GFX10 workaround */

   wait_imm barrier_imm[storage_count];
   uint16_t barrier_events[storage_count] = {}; /* use wait_event notion */

   std::map<PhysReg, wait_entry> gpr_map;

   wait_ctx() {}
   wait_ctx(Program* program_, const target_info* info_)
       : program(program_), gfx_level(program_->gfx_level), info(info_)
   {}

   bool join(const wait_ctx* other, bool logical)
   {
      bool changed = (other->pending_flat_lgkm && !pending_flat_lgkm) ||
                     (other->pending_flat_vm && !pending_flat_vm) || (~nonzero & other->nonzero);

      nonzero |= other->nonzero;
      pending_flat_lgkm |= other->pending_flat_lgkm;
      pending_flat_vm |= other->pending_flat_vm;
      pending_s_buffer_store |= other->pending_s_buffer_store;

      for (const auto& entry : other->gpr_map) {
         if (entry.second.logical != logical)
            continue;

         using iterator = std::map<PhysReg, wait_entry>::iterator;
         const std::pair<iterator, bool> insert_pair = gpr_map.insert(entry);
         if (insert_pair.second) {
            changed = true;
         } else {
            changed |= insert_pair.first->second.join(entry.second);
         }
      }

      for (unsigned i = 0; i < storage_count; i++) {
         changed |= barrier_imm[i].combine(other->barrier_imm[i]);
         changed |= (other->barrier_events[i] & ~barrier_events[i]) != 0;
         barrier_events[i] |= other->barrier_events[i];
      }

      return changed;
   }

   UNUSED void print(FILE* output) const
   {
      for (unsigned i = 0; i < wait_type_num; i++)
         fprintf(output, "nonzero[%u]: %u\n", i, nonzero & (1 << i) ? 1 : 0);
      fprintf(output, "pending_flat_lgkm: %u\n", pending_flat_lgkm);
      fprintf(output, "pending_flat_vm: %u\n", pending_flat_vm);
      for (const auto& entry : gpr_map) {
         fprintf(output, "gpr_map[%c%u] = {\n", entry.first.reg() >= 256 ? 'v' : 's',
                 entry.first.reg() & 0xff);
         entry.second.print(output);
         fprintf(output, "}\n");
      }

      for (unsigned i = 0; i < storage_count; i++) {
         if (!barrier_imm[i].empty() || barrier_events[i]) {
            fprintf(output, "barriers[%u] = {\n", i);
            barrier_imm[i].print(output);
            fprintf(output, "events: %u\n", barrier_events[i]);
            fprintf(output, "}\n");
         }
      }
   }
};

wait_event
get_vmem_event(wait_ctx& ctx, Instruction* instr, uint8_t type)
{
   if (instr->definitions.empty() && ctx.gfx_level >= GFX10)
      return event_vmem_store;
   wait_event ev = event_vmem;
   if (ctx.gfx_level >= GFX12 && type != vmem_nosampler)
      ev = type == vmem_bvh ? event_vmem_bvh : event_vmem_sample;
   return ev;
}

void
check_instr(wait_ctx& ctx, wait_imm& wait, alu_delay_info& delay, Instruction* instr)
{
   for (const Operand op : instr->operands) {
      if (op.isConstant() || op.isUndefined())
         continue;

      /* check consecutively read gprs */
      for (unsigned j = 0; j < op.size(); j++) {
         PhysReg reg{op.physReg() + j};
         std::map<PhysReg, wait_entry>::iterator it = ctx.gpr_map.find(reg);
         if (it == ctx.gpr_map.end() || !it->second.wait_on_read)
            continue;

         wait.combine(it->second.imm);
         if (instr->isVALU() || instr->isSALU())
            delay.combine(it->second.delay);
      }
   }

   for (const Definition& def : instr->definitions) {
      /* check consecutively written gprs */
      for (unsigned j = 0; j < def.getTemp().size(); j++) {
         PhysReg reg{def.physReg() + j};

         std::map<PhysReg, wait_entry>::iterator it = ctx.gpr_map.find(reg);
         if (it == ctx.gpr_map.end())
            continue;

         wait_imm reg_imm = it->second.imm;

         /* Vector Memory reads and writes return in the order they were issued */
         uint8_t vmem_type = get_vmem_type(ctx.gfx_level, instr);
         if (vmem_type) {
            wait_event event = get_vmem_event(ctx, instr, vmem_type);
            wait_type type = (wait_type)(ffs(ctx.info->get_counters_for_event(event)) - 1);
            if ((it->second.events & ctx.info->events[type]) == event &&
                (type != wait_type_vm || it->second.vmem_types == vmem_type))
               reg_imm[type] = wait_imm::unset_counter;
         }

         /* LDS reads and writes return in the order they were issued. same for GDS */
         if (instr->isDS() && (it->second.events & ctx.info->events[wait_type_lgkm]) ==
                                 (instr->ds().gds ? event_gds : event_lds))
            reg_imm.lgkm = wait_imm::unset_counter;

         wait.combine(reg_imm);
      }
   }
}

bool
parse_delay_alu(wait_ctx& ctx, alu_delay_info& delay, Instruction* instr)
{
   if (instr->opcode != aco_opcode::s_delay_alu)
      return false;

   unsigned imm[2] = {instr->salu().imm & 0xf, (instr->salu().imm >> 7) & 0xf};
   for (unsigned i = 0; i < 2; ++i) {
      alu_delay_wait wait = (alu_delay_wait)imm[i];
      if (wait >= alu_delay_wait::VALU_DEP_1 && wait <= alu_delay_wait::VALU_DEP_4)
         delay.valu_instrs = imm[i] - (uint32_t)alu_delay_wait::VALU_DEP_1 + 1;
      else if (wait >= alu_delay_wait::TRANS32_DEP_1 && wait <= alu_delay_wait::TRANS32_DEP_3)
         delay.trans_instrs = imm[i] - (uint32_t)alu_delay_wait::TRANS32_DEP_1 + 1;
      else if (wait >= alu_delay_wait::SALU_CYCLE_1)
         delay.salu_cycles = imm[i] - (uint32_t)alu_delay_wait::SALU_CYCLE_1 + 1;
   }

   delay.valu_cycles = instr->pass_flags & 0xffff;
   delay.trans_cycles = instr->pass_flags >> 16;

   return true;
}

void
perform_barrier(wait_ctx& ctx, wait_imm& imm, memory_sync_info sync, unsigned semantics)
{
   sync_scope subgroup_scope =
      ctx.program->workgroup_size <= ctx.program->wave_size ? scope_workgroup : scope_subgroup;
   if ((sync.semantics & semantics) && sync.scope > subgroup_scope) {
      unsigned storage = sync.storage;
      while (storage) {
         unsigned idx = u_bit_scan(&storage);

         /* LDS is private to the workgroup */
         sync_scope bar_scope_lds = MIN2(sync.scope, scope_workgroup);

         uint16_t events = ctx.barrier_events[idx];
         if (bar_scope_lds <= subgroup_scope)
            events &= ~event_lds;

         /* in non-WGP, the L1 (L0 on GFX10+) cache keeps all memory operations
          * in-order for the same workgroup */
         if (!ctx.program->wgp_mode && sync.scope <= scope_workgroup)
            events &= ~(event_vmem | event_vmem_store | event_smem);

         if (events)
            imm.combine(ctx.barrier_imm[idx]);
      }
   }
}

void
force_waitcnt(wait_ctx& ctx, wait_imm& imm)
{
   u_foreach_bit (i, ctx.nonzero)
      imm[i] = 0;
}

void
update_alu(wait_ctx& ctx, bool is_valu, bool is_trans, bool clear, int cycles)
{
   std::map<PhysReg, wait_entry>::iterator it = ctx.gpr_map.begin();
   while (it != ctx.gpr_map.end()) {
      wait_entry& entry = it->second;

      if (clear) {
         entry.remove_alu_counter();
      } else {
         entry.delay.valu_instrs += is_valu ? 1 : 0;
         entry.delay.trans_instrs += is_trans ? 1 : 0;
         entry.delay.salu_cycles -= cycles;
         entry.delay.valu_cycles -= cycles;
         entry.delay.trans_cycles -= cycles;

         entry.delay.fixup();
         if (it->second.delay.empty())
            entry.remove_alu_counter();
      }

      if (!entry.counters)
         it = ctx.gpr_map.erase(it);
      else
         it++;
   }
}

void
kill(wait_imm& imm, alu_delay_info& delay, Instruction* instr, wait_ctx& ctx,
     memory_sync_info sync_info)
{
   if (instr->opcode == aco_opcode::s_setpc_b64 || (debug_flags & DEBUG_FORCE_WAITCNT)) {
      /* Force emitting waitcnt states right after the instruction if there is
       * something to wait for. This is also applied for s_setpc_b64 to ensure
       * waitcnt states are inserted before jumping to the PS epilog.
       */
      force_waitcnt(ctx, imm);
   }

   /* Make sure POPS coherent memory accesses have reached the L2 cache before letting the
    * overlapping waves proceed into the ordered section.
    */
   if (ctx.program->has_pops_overlapped_waves_wait &&
       (ctx.gfx_level >= GFX11 ? instr->isEXP() && instr->exp().done
                               : (instr->opcode == aco_opcode::s_sendmsg &&
                                  instr->salu().imm == sendmsg_ordered_ps_done))) {
      uint8_t c = counter_vm | counter_vs;
      /* Await SMEM loads too, as it's possible for an application to create them, like using a
       * scalarization loop - pointless and unoptimal for an inherently divergent address of
       * per-pixel data, but still can be done at least synthetically and must be handled correctly.
       */
      if (ctx.program->has_smem_buffer_or_global_loads)
         c |= counter_lgkm;

      u_foreach_bit (i, c & ctx.nonzero)
         imm[i] = 0;
   }

   check_instr(ctx, imm, delay, instr);

   /* It's required to wait for scalar stores before "writing back" data.
    * It shouldn't cost anything anyways since we're about to do s_endpgm.
    */
   if ((ctx.nonzero & BITFIELD_BIT(wait_type_lgkm)) && instr->opcode == aco_opcode::s_dcache_wb) {
      assert(ctx.gfx_level >= GFX8);
      imm.lgkm = 0;
   }

   if (ctx.gfx_level >= GFX10 && instr->isSMEM()) {
      /* GFX10: A store followed by a load at the same address causes a problem because
       * the load doesn't load the correct values unless we wait for the store first.
       * This is NOT mitigated by an s_nop.
       *
       * TODO: Refine this when we have proper alias analysis.
       */
      if (ctx.pending_s_buffer_store && !instr->smem().definitions.empty() &&
          !instr->smem().sync.can_reorder()) {
         imm.lgkm = 0;
      }
   }

   if (instr->opcode == aco_opcode::ds_ordered_count &&
       ((instr->ds().offset1 | (instr->ds().offset0 >> 8)) & 0x1)) {
      imm.combine(ctx.barrier_imm[ffs(storage_gds) - 1]);
   }

   if (instr->opcode == aco_opcode::p_barrier)
      perform_barrier(ctx, imm, instr->barrier().sync, semantic_acqrel);
   else
      perform_barrier(ctx, imm, sync_info, semantic_release);

   if (!imm.empty() || !delay.empty()) {
      if (ctx.pending_flat_vm && imm.vm != wait_imm::unset_counter)
         imm.vm = 0;
      if (ctx.pending_flat_lgkm && imm.lgkm != wait_imm::unset_counter)
         imm.lgkm = 0;

      /* reset counters */
      for (unsigned i = 0; i < wait_type_num; i++)
         ctx.nonzero &= imm[i] == 0 ? ~BITFIELD_BIT(i) : UINT32_MAX;

      /* update barrier wait imms */
      for (unsigned i = 0; i < storage_count; i++) {
         wait_imm& bar = ctx.barrier_imm[i];
         uint16_t& bar_ev = ctx.barrier_events[i];
         for (unsigned j = 0; j < wait_type_num; j++) {
            if (bar[j] != wait_imm::unset_counter && imm[j] <= bar[j]) {
               bar[j] = wait_imm::unset_counter;
               bar_ev &= ~ctx.info->events[j] | event_flat;
            }
         }
         if (bar.vm == wait_imm::unset_counter && bar.lgkm == wait_imm::unset_counter)
            bar_ev &= ~event_flat;
      }

      if (ctx.program->gfx_level >= GFX11) {
         update_alu(ctx, false, false, false,
                    MAX3(delay.salu_cycles, delay.valu_cycles, delay.trans_cycles));
      }

      /* remove all gprs with higher counter from map */
      std::map<PhysReg, wait_entry>::iterator it = ctx.gpr_map.begin();
      while (it != ctx.gpr_map.end()) {
         for (unsigned i = 0; i < wait_type_num; i++) {
            if (imm[i] != wait_imm::unset_counter && imm[i] <= it->second.imm[i])
               it->second.remove_wait((wait_type)i, ctx.info->events[i]);
         }
         if (delay.valu_instrs <= it->second.delay.valu_instrs)
            it->second.delay.valu_instrs = alu_delay_info::valu_nop;
         if (delay.trans_instrs <= it->second.delay.trans_instrs)
            it->second.delay.trans_instrs = alu_delay_info::trans_nop;
         it->second.delay.fixup();
         if (it->second.delay.empty())
            it->second.remove_alu_counter();
         if (!it->second.counters)
            it = ctx.gpr_map.erase(it);
         else
            it++;
      }
   }

   if (imm.vm == 0)
      ctx.pending_flat_vm = false;
   if (imm.lgkm == 0) {
      ctx.pending_flat_lgkm = false;
      ctx.pending_s_buffer_store = false;
   }
}

void
update_barrier_imm(wait_ctx& ctx, uint8_t counters, wait_event event, memory_sync_info sync)
{
   for (unsigned i = 0; i < storage_count; i++) {
      wait_imm& bar = ctx.barrier_imm[i];
      uint16_t& bar_ev = ctx.barrier_events[i];
      if (sync.storage & (1 << i) && !(sync.semantics & semantic_private)) {
         bar_ev |= event;
         u_foreach_bit (j, counters)
            bar[j] = 0;
      } else if (!(bar_ev & ctx.info->unordered_events) && !(ctx.info->unordered_events & event)) {
         u_foreach_bit (j, counters) {
            if (bar[j] != wait_imm::unset_counter && (bar_ev & ctx.info->events[j]) == event)
               bar[j] = std::min<uint16_t>(bar[j] + 1, ctx.info->max_cnt[j]);
         }
      }
   }
}

void
update_counters(wait_ctx& ctx, wait_event event, memory_sync_info sync = memory_sync_info())
{
   uint8_t counters = ctx.info->get_counters_for_event(event);

   ctx.nonzero |= counters;

   update_barrier_imm(ctx, counters, event, sync);

   if (ctx.info->unordered_events & event)
      return;

   if (ctx.pending_flat_lgkm)
      counters &= ~counter_lgkm;
   if (ctx.pending_flat_vm)
      counters &= ~counter_vm;

   for (std::pair<const PhysReg, wait_entry>& e : ctx.gpr_map) {
      wait_entry& entry = e.second;

      if (entry.events & ctx.info->unordered_events)
         continue;

      assert(entry.events);

      u_foreach_bit (i, counters) {
         if ((entry.events & ctx.info->events[i]) == event)
            entry.imm[i] = std::min<uint16_t>(entry.imm[i] + 1, ctx.info->max_cnt[i]);
      }
   }
}

void
update_counters_for_flat_load(wait_ctx& ctx, memory_sync_info sync = memory_sync_info())
{
   assert(ctx.gfx_level < GFX10);

   ctx.nonzero |= BITFIELD_BIT(wait_type_lgkm) | BITFIELD_BIT(wait_type_vm);

   update_barrier_imm(ctx, counter_vm | counter_lgkm, event_flat, sync);

   for (std::pair<PhysReg, wait_entry> e : ctx.gpr_map) {
      if (e.second.counters & counter_vm)
         e.second.imm.vm = 0;
      if (e.second.counters & counter_lgkm)
         e.second.imm.lgkm = 0;
   }
   ctx.pending_flat_lgkm = true;
   ctx.pending_flat_vm = true;
}

void
insert_wait_entry(wait_ctx& ctx, PhysReg reg, RegClass rc, wait_event event, bool wait_on_read,
                  uint8_t vmem_types = 0, unsigned cycles = 0, bool force_linear = false)
{
   uint16_t counters = ctx.info->get_counters_for_event(event);
   wait_imm imm;
   u_foreach_bit (i, counters & wait_counters)
      imm[i] = 0;

   alu_delay_info delay;
   if (event == event_valu) {
      delay.valu_instrs = 0;
      delay.valu_cycles = cycles;
   } else if (event == event_trans) {
      delay.trans_instrs = 0;
      delay.trans_cycles = cycles;
   } else if (event == event_salu) {
      delay.salu_cycles = cycles;
   }

   wait_entry new_entry(event, imm, delay, counters, !rc.is_linear() && !force_linear,
                        wait_on_read);
   if (counters & counter_vm)
      new_entry.vmem_types |= vmem_types;

   for (unsigned i = 0; i < rc.size(); i++) {
      auto it = ctx.gpr_map.emplace(PhysReg{reg.reg() + i}, new_entry);
      if (!it.second)
         it.first->second.join(new_entry);
   }
}

void
insert_wait_entry(wait_ctx& ctx, Operand op, wait_event event, uint8_t vmem_types = 0)
{
   if (!op.isConstant() && !op.isUndefined())
      insert_wait_entry(ctx, op.physReg(), op.regClass(), event, false, vmem_types, 0);
}

void
insert_wait_entry(wait_ctx& ctx, Definition def, wait_event event, uint8_t vmem_types = 0,
                  unsigned cycles = 0)
{
   /* We can't safely write to unwritten destination VGPR lanes with DS/VMEM on GFX11 without
    * waiting for the load to finish.
    * Also, follow linear control flow for ALU because it's unlikely that the hardware does per-lane
    * dependency checks.
    */
   uint32_t ds_vmem_events = event_lds | event_gds | event_vmem | event_flat;
   uint32_t alu_events = event_trans | event_valu | event_salu;
   bool force_linear = ctx.gfx_level >= GFX11 && (event & (ds_vmem_events | alu_events));

   insert_wait_entry(ctx, def.physReg(), def.regClass(), event, true, vmem_types, cycles,
                     force_linear);
}

void
gen_alu(Instruction* instr, wait_ctx& ctx)
{
   Instruction_cycle_info cycle_info = get_cycle_info(*ctx.program, *instr);
   bool is_valu = instr->isVALU();
   bool is_trans = instr->isTrans();
   bool clear = instr->isEXP() || instr->isDS() || instr->isMIMG() || instr->isFlatLike() ||
                instr->isMUBUF() || instr->isMTBUF();

   wait_event event = (wait_event)0;
   if (is_trans)
      event = event_trans;
   else if (is_valu)
      event = event_valu;
   else if (instr->isSALU())
      event = event_salu;

   if (event != (wait_event)0) {
      for (const Definition& def : instr->definitions)
         insert_wait_entry(ctx, def, event, 0, cycle_info.latency);
   }
   update_alu(ctx, is_valu && instr_info.classes[(int)instr->opcode] != instr_class::wmma, is_trans,
              clear, cycle_info.issue_cycles);
}

void
gen(Instruction* instr, wait_ctx& ctx)
{
   switch (instr->format) {
   case Format::EXP: {
      Export_instruction& exp_instr = instr->exp();

      wait_event ev;
      if (exp_instr.dest <= 9)
         ev = event_exp_mrt_null;
      else if (exp_instr.dest <= 15)
         ev = event_exp_pos;
      else
         ev = event_exp_param;
      update_counters(ctx, ev);

      /* insert new entries for exported vgprs */
      for (unsigned i = 0; i < 4; i++) {
         if (exp_instr.enabled_mask & (1 << i)) {
            unsigned idx = exp_instr.compressed ? i >> 1 : i;
            assert(idx < exp_instr.operands.size());
            insert_wait_entry(ctx, exp_instr.operands[idx], ev);
         }
      }
      insert_wait_entry(ctx, exec, s2, ev, false);
      break;
   }
   case Format::FLAT: {
      FLAT_instruction& flat = instr->flat();
      if (ctx.gfx_level < GFX10 && !instr->definitions.empty())
         update_counters_for_flat_load(ctx, flat.sync);
      else
         update_counters(ctx, event_flat, flat.sync);

      if (!instr->definitions.empty())
         insert_wait_entry(ctx, instr->definitions[0], event_flat);
      break;
   }
   case Format::SMEM: {
      SMEM_instruction& smem = instr->smem();
      update_counters(ctx, event_smem, smem.sync);

      if (!instr->definitions.empty())
         insert_wait_entry(ctx, instr->definitions[0], event_smem);
      else if (ctx.gfx_level >= GFX10 && !smem.sync.can_reorder())
         ctx.pending_s_buffer_store = true;

      break;
   }
   case Format::DS: {
      DS_instruction& ds = instr->ds();
      update_counters(ctx, ds.gds ? event_gds : event_lds, ds.sync);
      if (ds.gds)
         update_counters(ctx, event_gds_gpr_lock);

      if (!instr->definitions.empty())
         insert_wait_entry(ctx, instr->definitions[0], ds.gds ? event_gds : event_lds);

      if (ds.gds) {
         for (const Operand& op : instr->operands)
            insert_wait_entry(ctx, op, event_gds_gpr_lock);
         insert_wait_entry(ctx, exec, s2, event_gds_gpr_lock, false);
      }
      break;
   }
   case Format::LDSDIR: {
      LDSDIR_instruction& ldsdir = instr->ldsdir();
      update_counters(ctx, event_ldsdir, ldsdir.sync);
      insert_wait_entry(ctx, instr->definitions[0], event_ldsdir);
      break;
   }
   case Format::MUBUF:
   case Format::MTBUF:
   case Format::MIMG:
   case Format::GLOBAL:
   case Format::SCRATCH: {
      uint8_t type = get_vmem_type(ctx.gfx_level, instr);
      wait_event ev = get_vmem_event(ctx, instr, type);

      update_counters(ctx, ev, get_sync_info(instr));

      if (!instr->definitions.empty())
         insert_wait_entry(ctx, instr->definitions[0], ev, type);

      if (ctx.gfx_level == GFX6 && instr->format != Format::MIMG && instr->operands.size() == 4) {
         update_counters(ctx, event_vmem_gpr_lock);
         insert_wait_entry(ctx, instr->operands[3], event_vmem_gpr_lock);
      } else if (ctx.gfx_level == GFX6 && instr->isMIMG() && !instr->operands[2].isUndefined()) {
         update_counters(ctx, event_vmem_gpr_lock);
         insert_wait_entry(ctx, instr->operands[2], event_vmem_gpr_lock);
      }

      break;
   }
   case Format::SOPP: {
      if (instr->opcode == aco_opcode::s_sendmsg || instr->opcode == aco_opcode::s_sendmsghalt)
         update_counters(ctx, event_sendmsg);
      break;
   }
   case Format::SOP1: {
      if (instr->opcode == aco_opcode::s_sendmsg_rtn_b32 ||
          instr->opcode == aco_opcode::s_sendmsg_rtn_b64) {
         update_counters(ctx, event_sendmsg);
         insert_wait_entry(ctx, instr->definitions[0], event_sendmsg);
      }
      break;
   }
   default: break;
   }
}

void
emit_waitcnt(wait_ctx& ctx, std::vector<aco_ptr<Instruction>>& instructions, wait_imm& imm)
{
   Builder bld(ctx.program, &instructions);

   if (ctx.gfx_level >= GFX12) {
      if (imm.vm != wait_imm::unset_counter && imm.lgkm != wait_imm::unset_counter) {
         bld.sopp(aco_opcode::s_wait_loadcnt_dscnt, (imm.vm << 8) | imm.lgkm);
         imm.vm = wait_imm::unset_counter;
         imm.lgkm = wait_imm::unset_counter;
      }

      if (imm.vs != wait_imm::unset_counter && imm.lgkm != wait_imm::unset_counter) {
         bld.sopp(aco_opcode::s_wait_storecnt_dscnt, (imm.vs << 8) | imm.lgkm);
         imm.vs = wait_imm::unset_counter;
         imm.lgkm = wait_imm::unset_counter;
      }

      aco_opcode op[wait_type_num];
      op[wait_type_exp] = aco_opcode::s_wait_expcnt;
      op[wait_type_lgkm] = aco_opcode::s_wait_dscnt;
      op[wait_type_vm] = aco_opcode::s_wait_loadcnt;
      op[wait_type_vs] = aco_opcode::s_wait_storecnt;
      op[wait_type_sample] = aco_opcode::s_wait_samplecnt;
      op[wait_type_bvh] = aco_opcode::s_wait_bvhcnt;
      op[wait_type_km] = aco_opcode::s_wait_kmcnt;

      for (unsigned i = 0; i < wait_type_num; i++) {
         if (imm[i] != wait_imm::unset_counter)
            bld.sopp(op[i], imm[i]);
      }
   } else {
      if (imm.vs != wait_imm::unset_counter) {
         assert(ctx.gfx_level >= GFX10);
         bld.sopk(aco_opcode::s_waitcnt_vscnt, Operand(sgpr_null, s1), imm.vs);
         imm.vs = wait_imm::unset_counter;
      }
      if (!imm.empty())
         bld.sopp(aco_opcode::s_waitcnt, imm.pack(ctx.gfx_level));
   }
   imm = wait_imm();
}

void
emit_delay_alu(wait_ctx& ctx, std::vector<aco_ptr<Instruction>>& instructions,
               alu_delay_info& delay)
{
   uint32_t imm = 0;
   if (delay.trans_instrs != delay.trans_nop) {
      imm |= (uint32_t)alu_delay_wait::TRANS32_DEP_1 + delay.trans_instrs - 1;
   }

   if (delay.valu_instrs != delay.valu_nop) {
      imm |= ((uint32_t)alu_delay_wait::VALU_DEP_1 + delay.valu_instrs - 1) << (imm ? 7 : 0);
   }

   /* Note that we can only put 2 wait conditions in the instruction, so if we have all 3 we just
    * drop the SALU one. Here we use that this doesn't really affect correctness so occasionally
    * getting this wrong isn't an issue. */
   if (delay.salu_cycles && imm <= 0xf) {
      unsigned cycles = std::min<uint8_t>(3, delay.salu_cycles);
      imm |= ((uint32_t)alu_delay_wait::SALU_CYCLE_1 + cycles - 1) << (imm ? 7 : 0);
   }

   Instruction* inst = create_instruction(aco_opcode::s_delay_alu, Format::SOPP, 0, 0);
   inst->salu().imm = imm;
   inst->pass_flags = (delay.valu_cycles | (delay.trans_cycles << 16));
   instructions.emplace_back(inst);
   delay = alu_delay_info();
}

bool
check_clause_raw(std::bitset<512>& regs_written, Instruction* instr)
{
   for (Operand op : instr->operands) {
      if (op.isConstant())
         continue;
      for (unsigned i = 0; i < op.size(); i++) {
         if (regs_written[op.physReg().reg() + i])
            return false;
      }
   }

   for (Definition def : instr->definitions) {
      for (unsigned i = 0; i < def.size(); i++)
         regs_written[def.physReg().reg() + i] = 1;
   }

   return true;
}

void
handle_block(Program* program, Block& block, wait_ctx& ctx)
{
   std::vector<aco_ptr<Instruction>> new_instructions;

   wait_imm queued_imm;
   alu_delay_info queued_delay;

   size_t clause_end = 0;
   for (size_t i = 0; i < block.instructions.size(); i++) {
      aco_ptr<Instruction>& instr = block.instructions[i];

      bool is_wait = queued_imm.unpack(ctx.gfx_level, instr.get());
      bool is_delay_alu = parse_delay_alu(ctx, queued_delay, instr.get());

      memory_sync_info sync_info = get_sync_info(instr.get());
      kill(queued_imm, queued_delay, instr.get(), ctx, sync_info);

      /* At the start of a possible clause, also emit waitcnts for each instruction to avoid
       * splitting the clause.
       */
      if (i >= clause_end || !queued_imm.empty()) {
         std::optional<std::bitset<512>> regs_written;
         for (clause_end = i + 1; clause_end < block.instructions.size(); clause_end++) {
            Instruction* next = block.instructions[clause_end].get();
            if (!should_form_clause(instr.get(), next))
               break;

            if (!regs_written) {
               regs_written.emplace();
               check_clause_raw(*regs_written, instr.get());
            }

            if (!check_clause_raw(*regs_written, next))
               break;

            kill(queued_imm, queued_delay, next, ctx, get_sync_info(next));
         }
      }

      if (program->gfx_level >= GFX11)
         gen_alu(instr.get(), ctx);
      gen(instr.get(), ctx);

      if (instr->format != Format::PSEUDO_BARRIER && !is_wait && !is_delay_alu) {
         if (instr->isVINTERP_INREG() && queued_imm.exp != wait_imm::unset_counter) {
            instr->vinterp_inreg().wait_exp = MIN2(instr->vinterp_inreg().wait_exp, queued_imm.exp);
            queued_imm.exp = wait_imm::unset_counter;
         }

         if (!queued_imm.empty())
            emit_waitcnt(ctx, new_instructions, queued_imm);
         if (!queued_delay.empty())
            emit_delay_alu(ctx, new_instructions, queued_delay);

         bool is_ordered_count_acquire =
            instr->opcode == aco_opcode::ds_ordered_count &&
            !((instr->ds().offset1 | (instr->ds().offset0 >> 8)) & 0x1);

         new_instructions.emplace_back(std::move(instr));
         perform_barrier(ctx, queued_imm, sync_info, semantic_acquire);

         if (is_ordered_count_acquire)
            queued_imm.combine(ctx.barrier_imm[ffs(storage_gds) - 1]);
      }
   }

   /* For last block of a program which has succeed shader part, wait all memory ops done
    * before go to next shader part.
    */
   if (block.kind & block_kind_end_with_regs)
      force_waitcnt(ctx, queued_imm);

   if (!queued_imm.empty())
      emit_waitcnt(ctx, new_instructions, queued_imm);
   if (!queued_delay.empty())
      emit_delay_alu(ctx, new_instructions, queued_delay);

   block.instructions.swap(new_instructions);
}

} /* end namespace */

void
insert_wait_states(Program* program)
{
   target_info info(program->gfx_level);

   /* per BB ctx */
   std::vector<bool> done(program->blocks.size());
   std::vector<wait_ctx> in_ctx(program->blocks.size(), wait_ctx(program, &info));
   std::vector<wait_ctx> out_ctx(program->blocks.size(), wait_ctx(program, &info));

   std::stack<unsigned, std::vector<unsigned>> loop_header_indices;
   unsigned loop_progress = 0;

   if (program->pending_lds_access) {
      update_barrier_imm(in_ctx[0], info.get_counters_for_event(event_lds), event_lds,
                         memory_sync_info(storage_shared));
   }

   for (Definition def : program->args_pending_vmem) {
      update_counters(in_ctx[0], event_vmem);
      insert_wait_entry(in_ctx[0], def, event_vmem);
   }

   for (unsigned i = 0; i < program->blocks.size();) {
      Block& current = program->blocks[i++];

      if (current.kind & block_kind_discard_early_exit) {
         /* Because the jump to the discard early exit block may happen anywhere in a block, it's
          * not possible to join it with its predecessors this way.
          * We emit all required waits when emitting the discard block.
          */
         continue;
      }

      wait_ctx ctx = in_ctx[current.index];

      if (current.kind & block_kind_loop_header) {
         loop_header_indices.push(current.index);
      } else if (current.kind & block_kind_loop_exit) {
         bool repeat = false;
         if (loop_progress == loop_header_indices.size()) {
            i = loop_header_indices.top();
            repeat = true;
         }
         loop_header_indices.pop();
         loop_progress = std::min<unsigned>(loop_progress, loop_header_indices.size());
         if (repeat)
            continue;
      }

      bool changed = false;
      for (unsigned b : current.linear_preds)
         changed |= ctx.join(&out_ctx[b], false);
      for (unsigned b : current.logical_preds)
         changed |= ctx.join(&out_ctx[b], true);

      if (done[current.index] && !changed) {
         in_ctx[current.index] = std::move(ctx);
         continue;
      } else {
         in_ctx[current.index] = ctx;
      }

      loop_progress = std::max<unsigned>(loop_progress, current.loop_nest_depth);
      done[current.index] = true;

      handle_block(program, current, ctx);

      out_ctx[current.index] = std::move(ctx);
   }

   /* Combine s_delay_alu using the skip field. */
   if (program->gfx_level >= GFX11) {
      for (Block& block : program->blocks) {
         int i = 0;
         int prev_delay_alu = -1;
         for (aco_ptr<Instruction>& instr : block.instructions) {
            if (instr->opcode != aco_opcode::s_delay_alu) {
               block.instructions[i++] = std::move(instr);
               continue;
            }

            uint16_t imm = instr->salu().imm;
            int skip = i - prev_delay_alu - 1;
            if (imm >> 7 || prev_delay_alu < 0 || skip >= 6) {
               if (imm >> 7 == 0)
                  prev_delay_alu = i;
               block.instructions[i++] = std::move(instr);
               continue;
            }

            block.instructions[prev_delay_alu]->salu().imm |= (skip << 4) | (imm << 7);
            prev_delay_alu = -1;
         }
         block.instructions.resize(i);
      }
   }
}

} // namespace aco
