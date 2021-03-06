#include <binaryninjaapi.h>
#include <capstone/capstone.h>

using namespace BinaryNinja;

// #define AARCH64_TRACE_INSTR

// Returns 1s expanded to count, e.g.: Count<uint8_t>(7) == 0b01111111
template <typename T> inline T Ones(size_t count) {
  if (count == sizeof(T) * 8) {
    return static_cast<T>(~static_cast<T>(0));
  } else {
    return ~(static_cast<T>(~static_cast<T>(0)) << count);
  }
}

class Disassembler {
private:
  csh mCapstone {};
  bool mIsOK = true;
public:
  Disassembler() noexcept {
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &mCapstone) != CS_ERR_OK) {
      mIsOK = false;
      return;
    }

    cs_option(mCapstone, CS_OPT_DETAIL, CS_OPT_ON);
  }

  ~Disassembler() {
    cs_close(&mCapstone);
  }

  bool IsOK() const {
    return mIsOK;
  }

  csh Get() const {
    return mCapstone;
  }
};

// Disassembler _must_ be thread_local because on multi-threaded analysis GetInstructionLowLevelIL may be called
// from multiple threads, thus causing Capstone to malfunction. Note that on thread exit the destructor will
// be called and the associated Capstone resources will be released
static thread_local Disassembler disassembler;

class AArch64ArchitectureExtension : public ArchitectureHook {
private:
  /**
   * Convert a Capstone condition code to BNIL condition code
   *
   * @param condition AArch64 condition code
   * @return BNIL condition, or -1
   */
  static BNLowLevelILFlagCondition LiftCondition(arm64_cc condition) {
    switch (condition) {
    case ARM64_CC_EQ:
      return LLFC_E;
    case ARM64_CC_NE:
      return LLFC_NE;
    case ARM64_CC_HS:
      return LLFC_UGE;
    case ARM64_CC_LO:
      return LLFC_ULE;
    case ARM64_CC_MI:
      return LLFC_NEG;
    case ARM64_CC_PL:
      return LLFC_POS;
    case ARM64_CC_VS:
      return LLFC_O;
    case ARM64_CC_VC:
      return LLFC_NO;
    case ARM64_CC_HI:
      return LLFC_UGE;
    case ARM64_CC_LS:
      return LLFC_ULE;
    case ARM64_CC_GE:
      return LLFC_SGE;
    case ARM64_CC_LT:
      return LLFC_SLT;
    case ARM64_CC_GT:
      return LLFC_SGT;
    case ARM64_CC_LE:
      return LLFC_SLE;
    case ARM64_CC_INVALID:
      return (BNLowLevelILFlagCondition) ARM64_CC_INVALID;
    case ARM64_CC_AL:
      return (BNLowLevelILFlagCondition) ARM64_CC_AL;
    case ARM64_CC_NV:
      return (BNLowLevelILFlagCondition) ARM64_CC_NV;
    }

    return (BNLowLevelILFlagCondition) -1;
  }

public:
  explicit AArch64ArchitectureExtension(Architecture* aarch64)
      : ArchitectureHook(aarch64) {
  }

  bool LiftCSINC(cs_insn* instr, LowLevelILFunction& il) {
    cs_arm64* detail = &(instr->detail->arm64);

    if (detail->op_count != 3) {
      return false;
    }

    uint32_t Rd = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[0].reg));
    uint32_t Rn = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[1].reg));
    uint32_t Rm = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[2].reg));
    size_t Rd_size = this->m_base->GetRegisterInfo(Rd).size;
    size_t Rn_size = this->m_base->GetRegisterInfo(Rn).size;
    size_t Rm_size = this->m_base->GetRegisterInfo(Rm).size;

    if (detail->cc == ARM64_CC_INVALID ||
        (Rd_size != Rn_size && Rn_size != Rm_size)) {
      return false;
    }

    // Never is actually _always_, Capstone internal
    if (detail->cc == ARM64_CC_AL || detail->cc == ARM64_CC_NV) {
      il.AddInstruction(il.SetRegister(Rd_size, il.Register(Rd_size, Rd),
                                       il.Register(Rn_size, Rn)));
      return true;
    }

    LowLevelILLabel assignmentLabel, incrementLabel, afterLabel;

    il.AddInstruction(il.If(il.FlagCondition(LiftCondition(detail->cc)),
                            assignmentLabel, incrementLabel));

    // Rd = Rn
    il.MarkLabel(assignmentLabel);
    il.AddInstruction(il.SetRegister(Rd_size, Rd, il.Register(Rn_size, Rn)));
    il.AddInstruction(il.Goto(afterLabel));

    // Rd = Rm + 1
    il.MarkLabel(incrementLabel);
    il.AddInstruction(il.SetRegister(
        Rd_size, Rd,
        il.Add(Rd_size, il.Register(Rm_size, Rm), il.Const(Rd_size, 1))));

    il.MarkLabel(afterLabel);

    return true;
  }

  bool LiftUMULL(cs_insn* instr, LowLevelILFunction& il) {
    cs_arm64* detail = &(instr->detail->arm64);

    if (detail->op_count != 3) {
      return false;
    }

    uint32_t Xd = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[0].reg));
    uint32_t Wn = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[1].reg));
    uint32_t Wm = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[2].reg));

    il.AddInstruction(il.SetRegister(
        8, Xd, il.Mult(8, il.Register(4, Wn), il.Register(4, Wm))));

    return true;
  }

  bool LiftCINC(cs_insn* instr, LowLevelILFunction& il) {
    cs_arm64* detail = &(instr->detail->arm64);

    if (detail->op_count != 2) {
      return false;
    }

    uint32_t Rd = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[0].reg));
    uint32_t Rn = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[1].reg));
    size_t Rd_size = this->m_base->GetRegisterInfo(Rd).size;
    size_t Rn_size = this->m_base->GetRegisterInfo(Rn).size;

    if (detail->cc == ARM64_CC_INVALID) {
      return false;
    }

    if (detail->cc == ARM64_CC_AL || detail->cc == ARM64_CC_NV) {
      // Rd = Rn + 1
      il.AddInstruction(il.SetRegister(
          Rd_size, Rd,
          il.Add(Rd_size, il.Register(Rn_size, Rn), il.Const(Rd_size, 1))));
      return true;
    }

    LowLevelILLabel incrementLabel, assignmentLabel, afterLabel;

    il.AddInstruction(il.If(il.FlagCondition(LiftCondition(detail->cc)),
                            incrementLabel, assignmentLabel));

    // Rd = Rn + 1
    il.MarkLabel(incrementLabel);
    il.AddInstruction(il.SetRegister(
        Rd_size, Rd,
        il.Add(Rd_size, il.Register(Rn_size, Rn), il.Const(Rd_size, 1))));
    il.AddInstruction(il.Goto(afterLabel));

    // Rd = Rn
    il.MarkLabel(assignmentLabel);
    il.AddInstruction(il.SetRegister(Rd_size, Rd, il.Register(Rn_size, Rn)));

    il.MarkLabel(afterLabel);

    return true;
  }

  bool LiftBFI(cs_insn* instr, LowLevelILFunction& il) {
    cs_arm64* detail = &(instr->detail->arm64);

    if (detail->op_count != 4) {
      return false;
    }

    uint32_t Rd = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[0].reg));
    uint32_t Rn = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[1].reg));
    size_t Rd_size = this->m_base->GetRegisterInfo(Rd).size;
    size_t Rn_size = this->m_base->GetRegisterInfo(Rd).size;
    int64_t lsb = detail->operands[2].imm;
    int64_t width = detail->operands[3].imm;

    // Continue if the both are same size and either 32-bit or 64-bit
    if (Rd_size != Rn_size && !(Rd_size == 4 ^ Rd_size == 8)) {
      return false;
    }

    uint64_t inclusion_mask;
    if (Rd_size == 8) {
      inclusion_mask = Ones<uint64_t>(width) << lsb;
    } else {
      inclusion_mask = Ones<uint32_t>(width) << lsb;
    }

    ExprId left = il.And(Rd_size, il.Register(Rd_size, Rd),
                         il.Const(Rd_size, ~inclusion_mask));
    ExprId right = il.And(
        Rd_size,
        il.ShiftLeft(Rd_size, il.Register(Rd_size, Rn), il.Const(1, lsb)),
        il.Const(Rd_size, inclusion_mask));
    il.AddInstruction(il.SetRegister(Rd_size, Rd, il.Or(Rd_size, left, right)));

    return true;
  }

  bool LiftROR(cs_insn* instr, LowLevelILFunction& il) {
    cs_arm64* detail = &(instr->detail->arm64);

    if (detail->op_count != 3) {
      return false;
    }

    uint32_t Rd = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[0].reg));
    uint32_t Rn = this->m_base->GetRegisterByName(
        cs_reg_name(disassembler.Get(), detail->operands[1].reg));

    size_t Rd_size = this->m_base->GetRegisterInfo(Rd).size;
    size_t Rn_size = this->m_base->GetRegisterInfo(Rd).size;

    if (Rd_size != Rn_size) {
      return false;
    }

    if (detail->operands[2].type == ARM64_OP_REG) {
      uint32_t Rm = this->m_base->GetRegisterByName(
          cs_reg_name(disassembler.Get(), detail->operands[2].reg));

      il.AddInstruction(
          il.SetRegister(Rd_size, Rd,
                         il.RotateRight(Rd_size, il.Register(Rd_size, Rn),
                                        il.Register(Rd_size, Rm))));
      return true;
    } else if (detail->operands[2].type == ARM64_OP_IMM) {
      uint32_t shift = detail->operands[2].imm;

      il.AddInstruction(
          il.SetRegister(Rd_size, Rd,
                         il.RotateRight(Rd_size, il.Register(Rd_size, Rn),
                                        il.Const(Rd_size, shift))));
      return true;
    }

    return false;
  }

  bool GetInstructionLowLevelIL(const uint8_t* data, uint64_t addr, size_t& len,
                                LowLevelILFunction& il) override {
    cs_insn* instr;
    size_t count = cs_disasm(disassembler.Get(), data, len, addr, 0, &instr);

    bool supported = false;
    if (count > 0) {
      switch (instr->id) {
      case ARM64_INS_CSINC:
#ifdef AARCH64_TRACE_INSTR
        LogInfo("CSINC @ 0x%lx", instr->address);
#endif
        supported = LiftCSINC(instr, il);
        break;
      case ARM64_INS_UMULL:
#ifdef AARCH64_TRACE_INSTR
        LogInfo("UMULL @ 0x%lx", instr->address);
#endif
        supported = LiftUMULL(instr, il);
        break;
      case ARM64_INS_CINC:
#ifdef AARCH64_TRACE_INSTR
        LogInfo("CINC @ 0x%lx", instr->address);
#endif
        supported = LiftCINC(instr, il);
        break;
      case ARM64_INS_BFI:
#ifdef AARCH64_TRACE_INSTR
        LogInfo("BFI @ 0x%lx", instr->address);
#endif
        supported = LiftBFI(instr, il);
        break;
      case ARM64_INS_ROR:
#ifdef AARCH64_TRACE_INSTR
        LogInfo("ROR @ 0x%lx", instr->address);
#endif
        supported = LiftROR(instr, il);
        break;
      }
    }

    len = instr->size;
    if (count > 0) {
      cs_free(instr, count);
    }

    if (!supported) {
      return ArchitectureHook::GetInstructionLowLevelIL(data, addr, len, il);
    }

    return true;
  }
};

extern "C" {
BINARYNINJAPLUGIN void CorePluginDependencies() {
  AddRequiredPluginDependency("arch_arm64");
}

BINARYNINJAPLUGIN bool CorePluginInit() {
  if (!disassembler.IsOK()) {
    LogError("Failed to create AArch64 disassembler engine");
    return false;
  }

  Architecture* aarch64Ext =
      new AArch64ArchitectureExtension(Architecture::GetByName("aarch64"));
  Architecture::Register(aarch64Ext);

  LogInfo("Registered AArch64 extensions plugin");

  return true;
}
}