/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef incl_HPHP_VM_UNIT_H_
#define incl_HPHP_VM_UNIT_H_

#include "hphp/runtime/base/runtime_option.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/base/complex_types.h"
#include "hphp/runtime/vm/repo-helpers.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/base/hphp_array.h"
#include "hphp/util/range.h"
#include "hphp/util/parser/location.h"
#include "hphp/runtime/base/md5.h"
#include "hphp/util/tiny_vector.h"

namespace HPHP {
// Forward declarations.
namespace Compiler { class Peephole; }
struct ActRec;

class Func;
class FuncEmitter;
class Repo;
class FuncDict;
class Unit;
class PreClassEmitter;

enum class UnitOrigin {
  File = 0,
  Eval = 1
};

enum UnitMergeKind {
  // UnitMergeKindClass is required to be 0 for correctness.
  UnitMergeKindClass = 0,
  UnitMergeKindUniqueDefinedClass = 1,
  // Top level, scalar defines in the unit
  UnitMergeKindDefine = 2,
  // Top level, scalar defines that will be loaded once
  // and preserved from request to request
  UnitMergeKindPersistentDefine = 3,
  UnitMergeKindGlobal = 4,
  // 5 is available
  UnitMergeKindReqDoc = 6,
  UnitMergeKindDone = 7,
  // We cannot add more kinds here; this has to fit in 3 bits.
};

enum UnitMergeState {
  UnitMergeStateUnmerged = 0,
  UnitMergeStateMerging = 1,
  UnitMergeStateMerged = 2,
  UnitMergeStateUniqueFuncs = 4,
  UnitMergeStateNeedsCompact = 8,
  UnitMergeStateEmpty = 32
};

inline bool ALWAYS_INLINE isMergeKindReq(UnitMergeKind k) {
  return k == UnitMergeKindReqDoc;
}

struct UnitMergeInfo {
  typedef IterRange<Func* const*> FuncRange;
  typedef IterRange<Func**> MutableFuncRange;

  unsigned m_firstHoistableFunc;
  unsigned m_firstHoistablePreClass;
  unsigned m_firstMergeablePreClass;
  unsigned m_mergeablesSize;
  void*    m_mergeables[1];

  static UnitMergeInfo* alloc(size_t num);

  Func** funcBegin() const {
    return (Func**)m_mergeables;
  }
  Func** funcEnd() const {
    return funcBegin() + m_firstHoistablePreClass;
  }
  Func** funcHoistableBegin() const {
    return funcBegin() + m_firstHoistableFunc;
  }
  MutableFuncRange nonMainFuncs() const {
    return MutableFuncRange(funcBegin() + 1, funcEnd());
  }
  MutableFuncRange hoistableFuncs() const {
    return MutableFuncRange(funcHoistableBegin(), funcEnd());
  }
  FuncRange funcs() const {
    return FuncRange(funcBegin(), funcEnd());
  }
  MutableFuncRange mutableFuncs() {
    return MutableFuncRange(funcBegin(), funcEnd());
  }
  void*& mergeableObj(int ix) { return ((void**)m_mergeables)[ix]; }
  void* mergeableData(int ix) { return (char*)m_mergeables + ix*sizeof(void*); }
};

// Exception handler table entry.
class EHEnt {
 public:
  enum class Type {
    Catch,
    Fault
  };
  Type m_type;
  Offset m_base;
  Offset m_past;
  int m_iterId;
  bool m_itRef;
  int m_parentIndex;
  Offset m_fault;
  typedef std::vector<std::pair<Id, Offset> > CatchVec;
  CatchVec m_catches;

  template<class SerDe> void serde(SerDe& sd) {
    sd(m_type)
      (m_base)
      (m_past)
      (m_iterId)
      (m_fault)
      (m_itRef)
      // eh.m_parentIndex is re-computed in sortEHTab, not serialized.
      ;
    if (m_type == Type::Catch) {
      sd(m_catches);
    }
  }
};

// Function paramater info region table entry.
class FPIEnt {
 public:
  Offset m_fpushOff;
  Offset m_fcallOff;
  Offset m_fpOff; // evaluation stack depth to current frame pointer
  int m_parentIndex;
  int m_fpiDepth;

  template<class SerDe> void serde(SerDe& sd) {
    sd(m_fpushOff)(m_fcallOff)(m_fpOff);
    // These fields are recomputed by sortFPITab:
    // m_parentIndex;
    // m_fpiDepth;
  }
};

class FPIEntComp {
 public:
  bool operator() (const FPIEnt &fpi1, const FPIEnt &fpi2) {
    return fpi1.m_fpushOff < fpi2.m_fpushOff;
  }
};

class SourceLoc {
 public:
  SourceLoc() : line0(1), char0(1), line1(1), char1(1) {}
  explicit SourceLoc(const Location& l) { setLoc(&l); }

  int line0;
  int char0;
  int line1;
  int char1;

  // {1, 1, 1, 1} is a special "invalid" value.
  void reset() {
    line0 = char0 = line1 = char1 = 1;
  }

  bool valid() {
    return line0 != 1 || char0 != 1 || line1 != 1 || char1 != 1;
  }

  void setLoc(const Location *l) {
    line0 = l->line0;
    char0 = l->char0;
    line1 = l->line1;
    char1 = l->char1;
  }

  bool same(const SourceLoc *l) const {
    return (this == l) ||
           (line0 == l->line0 && char0 == l->char0 &&
            line1 == l->line1 && char1 == l->char1);
  }

  bool operator==(const SourceLoc &l) const {
    return same(&l);
  }
};

class OffsetRange {
 public:
  OffsetRange() : m_base(0), m_past(0) {}
  OffsetRange(Offset base, Offset past) : m_base(base), m_past(past) {}
  Offset m_base;
  Offset m_past;
};
typedef std::vector<OffsetRange> OffsetRangeVec;

template<typename T>
class TableEntry {
 public:
  TableEntry() : m_pastOffset(0) {}

  TableEntry(Offset pastOffset, T val)
    : m_pastOffset(pastOffset), m_val(val) {}
  Offset pastOffset() const { return m_pastOffset; }
  T val() const { return m_val; }
  bool operator <(const TableEntry& other) const {
    return m_pastOffset < other.m_pastOffset;
  }

  template<class SerDe> void serde(SerDe& sd) { sd(m_pastOffset)(m_val); }

 private:
  Offset m_pastOffset;
  T m_val;
};

typedef TableEntry<int> LineEntry;
typedef std::vector<LineEntry> LineTable;
typedef TableEntry<const Func*> FuncEntry;
typedef std::vector<FuncEntry> FuncTable;

/*
 * This is the runtime representation of a typedef.  Typedefs are only
 * allowed when hip hop extensions are enabled.
 *
 * The m_kind field is KindOfObject whenever the typedef is basically
 * just a name.  At runtime we still might resolve this name to
 * another typedef, becoming a typedef for KindOfArray or something in
 * that request.
 *
 * For the per-request struct, see TypedefReq below.
 */
struct Typedef {
  const StringData* name;
  const StringData* value;
  DataType          kind;
  bool              nullable; // Null is allowed; for ?Foo typedefs

  template<class SerDe> void serde(SerDe& sd) {
    sd(name)
      (value)
      (kind)
      (nullable)
      ;
  }
};

/*
 * In a given request, a defined typedef is turned into a TypedefReq
 * struct.  This contains the information needed to validate parameter
 * type hints for a typedef at runtime.
 */
struct TypedefReq {
  DataType kind;          // may be KindOfAny for "mixed"
  bool nullable;          // for option types, like ?Foo
  Class* klass;           // nullptr if kind != KindOfObject
  const StringData* name; // needed for error messages; nullptr if not defined
};

//==============================================================================
// (const StringData*) versus (StringData*)
//
// All (const StringData*) values are static strings that came from e.g.
// StringData::GetStaticString().  Therefore no reference counting is required.
//
//==============================================================================

/*
 * Metadata about a compilation unit.
 *
 * Contains the list of PreClasses and global functions, along with a
 * special function called the 'pseudo-main', which is logically
 * invoked (modulo optimizations that avoid it) during execution when
 * the unit is included/required.
 */
struct Unit {
  friend class UnitEmitter;
  friend class UnitRepoProxy;
  friend class FuncDict;

  typedef UnitMergeInfo::FuncRange FuncRange;
  typedef UnitMergeInfo::MutableFuncRange MutableFuncRange;

  typedef hphp_hash_map<const Class*, Func*,
                        pointer_hash<Class> > PseudoMainCacheMap;

  class MetaInfo {
   public:
    enum class Kind {
      None,
      String,
      Class,
      NopOut,

      /*
       * Marks types that are proven to be a particular type by static
       * analysis.  Guards are not needed in these cases.
       */
      DataTypeInferred,

      /*
       * Marks types that are predicted by static analysis.  Guards
       * will still be needed in case the prediction is wrong.
       */
      DataTypePredicted,

      GuardedThis,
      GuardedCls,
      NoSurprise,
      ArrayCapacity,

      /*
       * Information about the known class of a property base in the
       * middle of a vector instruction.
       *
       * In this case, m_arg is the index of the member code for the
       * relevant property dim.  (Unlike other cases, m_arg is not an
       * index into the instruction inputs in NormalizedInstruction.)
       *
       * Whatever the base is when processing that member code will be
       * an object of the supplied class type (or a null).
       */
      MVecPropClass,

      /*
       * At a Ret{C,V} site, indicates which locals are known not to
       * be reference counted.  m_data is the id of the local variable
       * that cannot be reference counted at this point.
       */
      NonRefCounted,
    };

    /*
     * This flag is used to mark that m_arg is an index into an
     * MVector input list.  (We need to know this so we can bump the
     * indexes different amounts depending on the instruction type;
     * see applyInputMetaData.)
     */
    static const int VectorArg = 1 << 7;

    MetaInfo(Kind k, int a, Id d) : m_kind(k), m_arg(a), m_data(d) {
      assert((int)m_arg == a);
    }
    MetaInfo() : m_kind(Kind::None), m_arg(-1), m_data(0) {}

    /*
     * m_arg indicates which input the MetaInfo applies to.
     *
     * For instructions taking vector immediates, it is an index into
     * the immediate elements, excluding any MW members (and including
     * the base).  (This is currently even if the instruction takes
     * other stack arguments.)
     */
    Kind  m_kind;
    uint8_t m_arg;
    Id    m_data;
  };

  class MetaHandle {
    /*
      The meta-data in Unit::m_bc_meta is stored as:

      Offset     <num entries>
      Offset     byte-code-offset-1
      Offset     byte-code-offset-2
      ...
      Offset     byte-code_offset-n
      Offset     INT_MAX # sentinel
      Offset     data-offset-1
      Offset     data-offset-2
      ...
      Offset     data-offset-n
      Offset     m_bc_meta_len # sentinel
      uint8      m_kind1
      uint8      m_arg1
      VSI        m_data1
      ...
      uint8      m_kind-n
      uint8      m_arg-n
      VSI        m_data-n
    */
   public:
    MetaHandle() : index(nullptr), cur(0) {}
    bool findMeta(const Unit* unit, Offset offset);
    bool nextArg(MetaInfo& info);
   private:
    const Offset* index;
    unsigned cur;
    const uint8_t *ptr;
  };

  Unit();
  ~Unit();
  void* operator new(size_t sz);
  void operator delete(void* p, size_t sz);

  int repoId() const { return m_repoId; }
  int64_t sn() const { return m_sn; }

  PC entry() const { return m_bc; }
  Offset bclen() const { return m_bclen; }

  PC at(const Offset off) const {
    assert(off >= 0 && off <= Offset(m_bclen));
    return m_bc + off;
  }

  Offset offsetOf(const Opcode* op) const {
    assert(op >= m_bc && op <= (m_bc + m_bclen));
    return op - m_bc;
  }
  Offset offsetOf(const Op* op) const {
    return offsetOf(reinterpret_cast<const Opcode*>(op));
  }

  const StringData* filepath() const {
    assert(m_filepath);
    return m_filepath;
  }
  CStrRef filepathRef() const {
    assert(m_filepath);
    return *(String*)(&m_filepath);
  }
  const StringData* dirpath() const {
    assert(m_dirpath);
    return m_dirpath;
  }

  MD5 md5() const { return m_md5; }

  static NamedEntity* GetNamedEntity(const StringData *)
    __attribute__((__flatten__));
  static size_t GetNamedEntityTableSize();
  static Array getUserFunctions();
  static Array getClassesInfo();
  static Array getInterfacesInfo();
  static Array getTraitsInfo();

  size_t numLitstrs() const {
    return m_namedInfo.size();
  }
  StringData* lookupLitstrId(Id id) const {
    assert(id >= 0 && id < Id(m_namedInfo.size()));
    return const_cast<StringData*>(m_namedInfo[id].first);
  }

  const NamedEntity* lookupNamedEntityId(Id id) const {
    return lookupNamedEntityPairId(id).second;
  }

  const NamedEntityPair& lookupNamedEntityPairId(Id id) const {
    assert(id < Id(m_namedInfo.size()));
    const NamedEntityPair &ne = m_namedInfo[id];
    assert(ne.first);
    if (UNLIKELY(!ne.second)) {
      const_cast<const NamedEntity*&>(ne.second) = GetNamedEntity(ne.first);
    }
    return ne;
  }

  size_t numArrays() const {
    return m_arrays.size();
  }
  ArrayData* lookupArrayId(Id id) const {
    return const_cast<ArrayData*>(m_arrays.at(id));
  }

  static Func *lookupFunc(const NamedEntity *ne);
  static Func *lookupFunc(const StringData *funcName);
  static Func *loadFunc(const NamedEntity *ne, const StringData* name);
  static Func *loadFunc(const StringData* name);

  static Class* defClass(const HPHP::PreClass* preClass,
                         bool failIsFatal = true);
  static bool aliasClass(Class* original, const StringData* alias);
  void defTypedef(Id id);

  static Cell* lookupCns(const StringData* cnsName);
  static Cell* lookupPersistentCns(const StringData* cnsName);
  static Cell* loadCns(const StringData* cnsName);
  static bool defCns(const StringData* cnsName, const TypedValue* value,
                     bool persistent = false);
  static uint64_t defCnsHelper(uint64_t ch,
                               const TypedValue* value,
                               const StringData* cnsName);
  static void defDynamicSystemConstant(const StringData* cnsName,
                                       const void* data);
  static bool defCnsDynamic(const StringData* cnsName, TypedValue* value);

  /*
   * Find the Class* for a defined class corresponding to the name
   * `clsName'.
   *
   * Returns: nullptr if the class of the given name is not yet
   * defined in this request.
   */
  static Class *lookupClass(const StringData *clsName) {
    return lookupClass(GetNamedEntity(clsName));
  }

  /*
   * Find the Class* for a defined class with name mapped to the
   * supplied NamedEntity.
   *
   * Returns: nullptr if the class is not yet defined in this request.
   */
  static Class *lookupClass(const NamedEntity *ne) {
    Class* cls;
    if (LIKELY((cls = ne->getCachedClass()) != nullptr)) {
      return cls;
    }
    return nullptr;
  }

  /*
   * Same as lookupClass, except if it's not defined *and* is unique,
   * return the Class* anyway.
   *
   * The point of this is that when jitting code before a unique class
   * is defined, we can often still burn the Class* into the TC, since
   * it will be defined by the time the code that needs the Class*
   * runs (via autoload or whatnot).
   */
  static Class *lookupUniqueClass(const NamedEntity *ne) {
    Class* cls = ne->clsList();
    if (LIKELY(cls != nullptr)) {
      if (cls->attrs() & AttrUnique && RuntimeOption::RepoAuthoritative) {
        return cls;
      }
      return cls->getCached();
    }
    return nullptr;
  }

  static Class *lookupUniqueClass(const StringData *clsName) {
    return lookupUniqueClass(GetNamedEntity(clsName));
  }

  static Class *loadClass(const NamedEntity *ne,
                          const StringData *name);

  static Class *loadClass(const StringData *name) {
    return loadClass(GetNamedEntity(name), name);
  }

  static Class *loadMissingClass(const NamedEntity *ne,
                                 const StringData *name);

  static Class* getClass(const StringData* name, bool tryAutoload) {
    return getClass(GetNamedEntity(name), name, tryAutoload);
  }

  static Class* getClass(const NamedEntity *ne, const StringData *name,
                         bool tryAutoload);
  static bool classExists(const StringData* name, bool autoload,
                          Attr typeAttrs);

  bool compileTimeFatal(const StringData*& msg, int& line) const;
  const TypedValue *getMainReturn() const {
    return &m_mainReturn;
  }
private:
  template <bool debugger>
  void mergeImpl(void* tcbase, UnitMergeInfo* mi);
public:
  Func* firstHoistable() const {
    return *m_mergeInfo->funcHoistableBegin();
  }
  Func* getMain(Class* cls = nullptr) const;
  // Ranges for iterating over functions.
  MutableFuncRange nonMainFuncs() const {
    return m_mergeInfo->nonMainFuncs();
  }
  MutableFuncRange hoistableFuncs() const {
    return m_mergeInfo->hoistableFuncs();
  }
  void renameFunc(const StringData* oldName, const StringData* newName);
  static void loadFunc(const Func *func);
  FuncRange funcs() const {
    return m_mergeInfo->funcs();
  }
  MutableFuncRange mutableFuncs() {
    return m_mergeInfo->mutableFuncs();
  }
  Func* lookupFuncId(Id id) const {
    assert(id < Id(m_mergeInfo->m_firstHoistablePreClass));
    return m_mergeInfo->funcBegin()[id];
  }
  size_t numPreClasses() const {
    return (size_t)m_preClasses.size();
  }
  PreClass* lookupPreClassId(Id id) const {
    assert(id < Id(m_preClasses.size()));
    return m_preClasses[id].get();
  }
  typedef std::vector<PreClassPtr> PreClassPtrVec;
  typedef Range<PreClassPtrVec> PreClassRange;
  void initialMerge();
  void merge();
  PreClassRange preclasses() const {
    return PreClassRange(m_preClasses);
  }
  bool mergeClasses() const;

  int getLineNumber(Offset pc) const;
  bool getSourceLoc(Offset pc, SourceLoc& sLoc) const;
  bool getOffsetRanges(int line, OffsetRangeVec& offsets) const;
  bool getOffsetRange(Offset pc, OffsetRange& range) const;

  Op getOpcode(size_t instrOffset) const {
    assert(instrOffset < m_bclen);
    return toOp(m_bc[instrOffset]);
  }

  /*
   * Return the Func* for the code at offset off.
   *
   * Returns nullptr if the offset is not in a func body (but this
   * should be impossible).
   */
  const Func* getFunc(Offset pc) const;

  void setCacheId(unsigned id) {
    m_cacheOffset = id >> 3;
    m_cacheMask = 1 << (id & 7);
  }
  bool isMergeOnly() const { return m_mergeOnly; }
  void clearMergeOnly() { m_mergeOnly = false; }
  bool isEmpty() const { return m_mergeState & UnitMergeStateEmpty; }
  void* replaceUnit() const;
public:
  static Mutex s_classesMutex;

  struct PrintOpts {
    PrintOpts()
      : startOffset(kInvalidOffset)
      , stopOffset(kInvalidOffset)
      , showLines(true)
      , showFuncs(true)
      , indentSize(1)
    {}

    PrintOpts& range(Offset start, Offset stop) {
      startOffset = start;
      stopOffset = stop;
      return *this;
    }

    PrintOpts& noLineNumbers() {
      showLines = false;
      return *this;
    }

    PrintOpts& noFuncs() {
      showFuncs = false;
      return *this;
    }

    PrintOpts& indent(int i) {
      indentSize = i;
      return *this;
    }

    Offset startOffset;
    Offset stopOffset;
    bool showLines;
    bool showFuncs;
    int indentSize;
  };

  void prettyPrint(std::ostream&, PrintOpts = PrintOpts()) const;
  std::string toString() const;

public: // Translator field access
  static size_t bcOff() { return offsetof(Unit, m_bc); }

private:
  // pseudoMain's return value, or KindOfUninit if its not known.
  TypedValue m_mainReturn;
  int64_t m_sn;
  uchar const* m_bc;
  size_t m_bclen;
  uchar const* m_bc_meta;
  size_t m_bc_meta_len;
  const StringData* m_filepath;
  const StringData* m_dirpath;
  MD5 m_md5;
  std::vector<NamedEntityPair> m_namedInfo;
  std::vector<const ArrayData*> m_arrays;
  PreClassPtrVec m_preClasses;
  FixedVector<Typedef> m_typedefs;
  UnitMergeInfo* m_mergeInfo;
  unsigned m_cacheOffset;
  int8_t m_repoId;
  uint8_t m_mergeState;
  uint8_t m_cacheMask;
  bool m_mergeOnly;
  LineTable m_lineTable;
  FuncTable m_funcTable;
  mutable PseudoMainCacheMap *m_pseudoMainCache;
};

class UnitEmitter {
  friend class UnitRepoProxy;
  friend class ::HPHP::Compiler::Peephole;
 public:
  explicit UnitEmitter(const MD5& md5);
  ~UnitEmitter();

  void addTrivialPseudoMain();
  int repoId() const { return m_repoId; }
  void setRepoId(int repoId) { m_repoId = repoId; }
  int64_t sn() const { return m_sn; }
  void setSn(int64_t sn) { m_sn = sn; }
  Offset bcPos() const { return (Offset)m_bclen; }
  void setBc(const uchar* bc, size_t bclen);
  void setBcMeta(const uchar* bc_meta, size_t bc_meta_len);
  const StringData* getFilepath() { return m_filepath; }
  void setFilepath(const StringData* filepath) { m_filepath = filepath; }
  void setMainReturn(const TypedValue* v) { m_mainReturn = *v; }
  void setMergeOnly(bool b) { m_mergeOnly = b; }
  const MD5& md5() const { return m_md5; }
  Id addTypedef(const Typedef& td);
  Id mergeLitstr(const StringData* litstr);
  Id mergeArray(ArrayData* a, const StringData* key=nullptr);
  FuncEmitter* getMain();
  void initMain(int line1, int line2);
  FuncEmitter* newFuncEmitter(const StringData* n);
  void appendTopEmitter(FuncEmitter* func);
  FuncEmitter* newMethodEmitter(const StringData* n, PreClassEmitter* pce);
  PreClassEmitter* newPreClassEmitter(const StringData* n,
                                      PreClass::Hoistable hoistable);
  PreClassEmitter* pce(Id preClassId) { return m_pceVec[preClassId]; }

  /*
   * Record source location information for the last chunk of bytecode
   * added to this UnitEmitter.  Adjacent regions associated with the
   * same source line will be collapsed as this is created.
   */
  void recordSourceLocation(const Location *sLoc, Offset start);

  /*
   * Adds a new FuncEmitter to the unit.  You can only do this once
   * for the FuncEmitter (after you are done setting it up).  Also,
   * all FuncEmitter's added to the unit must not overlap.
   *
   * Takes ownership of `fe'.
   */
  void recordFunction(FuncEmitter *fe);

 private:
  template<class T>
  void emitImpl(T n, int64_t pos) {
    uchar *c = (uchar*)&n;
    if (pos == -1) {
      // Make sure m_bc is large enough.
      while (m_bclen + sizeof(T) > m_bcmax) {
        m_bc = (uchar*)realloc(m_bc, m_bcmax << 1);
        m_bcmax <<= 1;
      }
      memcpy(&m_bc[m_bclen], c, sizeof(T));
      m_bclen += sizeof(T);
    } else {
      assert(pos + sizeof(T) <= m_bclen);
      for (uint i = 0; i < sizeof(T); ++i) {
        m_bc[pos + i] = c[i];
      }
    }
  }
 public:
  void emitOp(Op op, int64_t pos = -1) {
    emitByte((uchar)op, pos);
  }
  void emitByte(uchar n, int64_t pos = -1) { emitImpl(n, pos); }
  void emitInt32(int n, int64_t pos = -1) { emitImpl(n, pos); }
  template<typename T> void emitIVA(T n) {
    if (LIKELY((n & 0x7f) == n)) {
      emitByte((unsigned char)n << 1);
    } else {
      assert((n & 0x7fffffff) == n);
      emitInt32((n << 1) | 0x1);
    }
  }
  void emitInt64(int64_t n, int64_t pos = -1) { emitImpl(n, pos); }
  void emitDouble(double n, int64_t pos = -1) { emitImpl(n, pos); }
  bool insert(UnitOrigin unitOrigin, RepoTxn& txn);
  void commit(UnitOrigin unitOrigin);
  Func* newFunc(const FuncEmitter* fe, Unit& unit, Id id, int line1, int line2,
                Offset base, Offset past,
                const StringData* name, Attr attrs, bool top,
                const StringData* docComment, int numParams,
                bool isClosureBody, bool isGenerator);
  Func* newFunc(const FuncEmitter* fe, Unit& unit, PreClass* preClass,
                int line1, int line2, Offset base, Offset past,
                const StringData* name, Attr attrs, bool top,
                const StringData* docComment, int numParams,
                bool isClosureBody, bool isGenerator);
  Unit* create();
  void returnSeen() { m_returnSeen = true; }
  void pushMergeableClass(PreClassEmitter* e);
  void pushMergeableInclude(UnitMergeKind kind, const StringData* unitName);
  void insertMergeableInclude(int ix, UnitMergeKind kind, Id id);
  void pushMergeableDef(UnitMergeKind kind,
                        const StringData* name, const TypedValue& tv);
  void insertMergeableDef(int ix, UnitMergeKind kind,
                          Id id, const TypedValue& tv);
 private:
  void setLines(const LineTable& lines);

 private:
  int m_repoId;
  int64_t m_sn;
  static const size_t BCMaxInit = 4096; // Initial bytecode size.
  size_t m_bcmax;
  uchar* m_bc;
  size_t m_bclen;
  uchar* m_bc_meta;
  size_t m_bc_meta_len;
  TypedValue m_mainReturn;
  const StringData* m_filepath;
  MD5 m_md5;
  typedef hphp_hash_map<const StringData*, Id,
                        string_data_hash, string_data_same> LitstrMap;
  LitstrMap m_litstr2id;
  std::vector<const StringData*> m_litstrs;
  typedef hphp_hash_map<const StringData*, Id,
                        string_data_hash, string_data_same> ArrayIdMap;
  ArrayIdMap m_array2id;
  typedef struct {
    const StringData* serialized;
    const ArrayData* array;
  } ArrayVecElm;
  typedef std::vector<ArrayVecElm> ArrayVec;
  ArrayVec m_arrays;
  int m_nextFuncSn;
  bool m_mergeOnly;
  typedef std::vector<FuncEmitter*> FeVec;
  FeVec m_fes;
  typedef hphp_hash_map<const FuncEmitter*, const Func*,
                        pointer_hash<FuncEmitter> > FMap;
  FMap m_fMap;
  typedef std::vector<PreClassEmitter*> PceVec;
  typedef std::vector<Id> IdVec;
  PceVec m_pceVec;
  typedef hphp_hash_set<const StringData*, string_data_hash,
                        string_data_isame> HoistedPreClassSet;
  HoistedPreClassSet m_hoistablePreClassSet;
  IdVec m_hoistablePceIdVec;
  typedef std::vector<std::pair<UnitMergeKind, Id> > MergeableStmtVec;
  MergeableStmtVec m_mergeableStmts;
  std::vector<std::pair<Id,TypedValue> > m_mergeableValues;
  bool m_allClassesHoistable;
  bool m_returnSeen;
  /*
   * m_sourceLocTab and m_feTab are interval maps.  Each entry encodes
   * an open-closed range of bytecode offsets.
   *
   * The m_sourceLocTab is keyed by the start of each half-open range.
   * This is to allow appending new bytecode offsets that are part of
   * the same range to coalesce.
   *
   * The m_feTab is keyed by the past-the-end offset.  This is the
   * format we'll want it in when we go to create a Unit.
   */
  std::vector<std::pair<Offset,SourceLoc> > m_sourceLocTab;
  std::vector<std::pair<Offset,const FuncEmitter*> > m_feTab;
  std::vector<Typedef> m_typedefs;
};

class UnitRepoProxy : public RepoProxy {
  friend class Unit;
  friend class UnitEmitter;
 public:
  explicit UnitRepoProxy(Repo& repo);
  ~UnitRepoProxy();
  void createSchema(int repoId, RepoTxn& txn);
  Unit* load(const std::string& name, const MD5& md5);

#define URP_IOP(o) URP_OP(Insert##o, insert##o)
#define URP_GOP(o) URP_OP(Get##o, get##o)
#define URP_OPS \
  URP_IOP(Unit) \
  URP_GOP(Unit) \
  URP_IOP(UnitLitstr) \
  URP_GOP(UnitLitstrs) \
  URP_IOP(UnitArray) \
  URP_GOP(UnitArrays) \
  URP_IOP(UnitMergeable) \
  URP_GOP(UnitMergeables) \
  URP_IOP(UnitSourceLoc) \
  URP_GOP(SourceLoc) \
  URP_GOP(SourceLocPastOffsets) \
  URP_GOP(SourceLocBaseOffset) \
  URP_GOP(BaseOffsetAtPCLoc) \
  URP_GOP(BaseOffsetAfterPCLoc)
  class InsertUnitStmt : public RepoProxy::Stmt {
   public:
    InsertUnitStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    void insert(RepoTxn& txn, int64_t& unitSn, const MD5& md5, const uchar* bc,
                size_t bclen, const uchar* bc_meta, size_t bc_meta_len,
                const TypedValue* mainReturn, bool mergeOnly,
                const LineTable& lines,
                const std::vector<Typedef>&);
  };
  class GetUnitStmt : public RepoProxy::Stmt {
   public:
    GetUnitStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    bool get(UnitEmitter& ue, const MD5& md5);
  };
  class InsertUnitLitstrStmt : public RepoProxy::Stmt {
   public:
    InsertUnitLitstrStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    void insert(RepoTxn& txn, int64_t unitSn, Id litstrId,
                const StringData* litstr);
  };
  class GetUnitLitstrsStmt : public RepoProxy::Stmt {
   public:
    GetUnitLitstrsStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    void get(UnitEmitter& ue);
  };
  class InsertUnitArrayStmt : public RepoProxy::Stmt {
   public:
    InsertUnitArrayStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    void insert(RepoTxn& txn, int64_t unitSn, Id arrayId,
                const StringData* array);
  };
  class GetUnitArraysStmt : public RepoProxy::Stmt {
   public:
    GetUnitArraysStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    void get(UnitEmitter& ue);
  };
  class InsertUnitMergeableStmt : public RepoProxy::Stmt {
   public:
    InsertUnitMergeableStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    void insert(RepoTxn& txn, int64_t unitSn,
                int ix, UnitMergeKind kind,
                Id id, TypedValue *value);
  };
  class GetUnitMergeablesStmt : public RepoProxy::Stmt {
   public:
    GetUnitMergeablesStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    void get(UnitEmitter& ue);
  };
  class InsertUnitSourceLocStmt : public RepoProxy::Stmt {
   public:
    InsertUnitSourceLocStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    void insert(RepoTxn& txn, int64_t unitSn, Offset pastOffset, int line0,
                int char0, int line1, int char1);
  };
  class GetSourceLocStmt : public RepoProxy::Stmt {
   public:
    GetSourceLocStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    bool get(int64_t unitSn, Offset pc, SourceLoc& sLoc);
  };
  class GetSourceLocPastOffsetsStmt : public RepoProxy::Stmt {
   public:
    GetSourceLocPastOffsetsStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    bool get(int64_t unitSn, int line, OffsetRangeVec& ranges);
  };
  class GetSourceLocBaseOffsetStmt : public RepoProxy::Stmt {
   public:
    GetSourceLocBaseOffsetStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    bool get(int64_t unitSn, OffsetRange& range);
  };
  class GetBaseOffsetAtPCLocStmt : public RepoProxy::Stmt {
   public:
    GetBaseOffsetAtPCLocStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    bool get(int64_t unitSn, Offset pc, Offset& offset);
  };
  class GetBaseOffsetAfterPCLocStmt : public RepoProxy::Stmt {
   public:
    GetBaseOffsetAfterPCLocStmt(Repo& repo, int repoId) : Stmt(repo, repoId) {}
    bool get(int64_t unitSn, Offset pc, Offset& offset);
  };
#define URP_OP(c, o) \
 public: \
  c##Stmt& o(int repoId) { return *m_##o[repoId]; } \
 private: \
  c##Stmt m_##o##Local; \
  c##Stmt m_##o##Central; \
  c##Stmt* m_##o[RepoIdCount];
  URP_OPS
#undef URP_OP
};

/**
 * AllFuncs
 * MutableAllFuncs
 *
 * Range over all Func's in a single unit.
 */

struct ConstPreClassMethodRanger {
  typedef Func* const* Iter;
  typedef const Func* Value;
  static Iter get(PreClassPtr pc) {
    return pc->methods();
  }
};

struct MutablePreClassMethodRanger {
  typedef Func** Iter;
  typedef Func* Value;
  static Func** get(PreClassPtr pc) {
    return pc->mutableMethods();
  }
};
template<typename FuncRange,
         typename GetMethods>
class AllFuncsImpl {
 public:
  explicit AllFuncsImpl(const Unit* unit)
    : fr(unit->funcs())
    , mr(0, 0)
    , cr(unit->preclasses())
  {
    if (fr.empty()) skip();
  }
  bool empty() const { return fr.empty() && mr.empty() && cr.empty(); }
  typedef typename GetMethods::Value FuncPtr;
  FuncPtr front() const {
    assert(!empty());
    if (!fr.empty()) return fr.front();
    assert(!mr.empty());
    return mr.front();
  }
  FuncPtr popFront() {
    FuncPtr f = !fr.empty() ? fr.popFront() :
      !mr.empty() ? mr.popFront() : 0;
    assert(f);
    if (fr.empty() && mr.empty()) skip();
    return f;
  }
 private:
  void skip() {
    assert(fr.empty());
    while (!cr.empty() && mr.empty()) {
      PreClassPtr c = cr.popFront();
      mr = Unit::FuncRange(GetMethods::get(c),
                           GetMethods::get(c) + c->numMethods());
    }
  }

  Unit::FuncRange fr;
  Unit::FuncRange mr;
  Unit::PreClassRange cr;
};

typedef AllFuncsImpl<Unit::FuncRange, ConstPreClassMethodRanger> AllFuncs;
typedef AllFuncsImpl<Unit::MutableFuncRange, MutablePreClassMethodRanger> MutableAllFuncs;

/**
 *
 * Range over all defined classes.
 */
class AllClasses {
protected:
  NamedEntityMap::iterator m_next, m_end;
  Class* m_current;
  void next();
  void skip();
public:
  AllClasses();
  bool empty() const;
  Class* front() const;
  Class* popFront();
};

/**
 * If name starts with '\\', returns a new String with the leading
 * slash stripped. Otherwise returns a null string.
 */
inline const String normalizeNS(const StringData* name) {
  assert(name->data()[name->size()] == 0);
  if (name->data()[0] == '\\') {
    return String(name->data() + 1);
  }
  return null_string;
}

}
#endif
