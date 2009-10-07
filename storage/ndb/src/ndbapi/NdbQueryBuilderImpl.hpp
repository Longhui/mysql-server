/*
   Copyright (C) 2009 Sun Microsystems Inc
    All rights reserved. Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/


#ifndef NdbQueryBuilderImpl_H
#define NdbQueryBuilderImpl_H

// Query-related error codes.
#define QRY_REQ_ARG_IS_NULL 4800
#define QRY_TOO_FEW_KEY_VALUES 4801
#define QRY_TOO_MANY_KEY_VALUES 4802
#define QRY_OPERAND_HAS_WRONG_TYPE 4803
#define QRY_CHAR_OPERAND_TRUNCATED 4804
#define QRY_NUM_OPERAND_RANGE 4805
#define QRY_UNKONWN_PARENT 4806
#define QRY_UNKNOWN_COLUMN 4807
#define QRY_UNRELATED_INDEX 4808
#define QRY_WRONG_INDEX_TYPE 4809
#define QRY_OPERAND_ALREADY_BOUND 4810
#define QRY_DEFINITION_TOO_LARGE 4811
#define QRY_DUPLICATE_COLUMN_IN_PROJ 4812
#define QRY_NEED_PARAMETER 4813
#define QRY_RESULT_ROW_ALREADY_DEFINED 4814
#define QRY_HAS_ZERO_OPERATIONS 4815

#ifdef __cplusplus
#include <Vector.hpp>
#include "NdbQueryBuilder.hpp"

// Forward declared
class NdbTableImpl;
class NdbIndexImpl;
class NdbColumnImpl;
class NdbQueryBuilderImpl;
class NdbQueryDefImpl;
class NdbQueryOperationDefImpl;
class NdbParamOperandImpl;
class NdbConstOperandImpl;
class NdbLinkedOperandImpl;

// For debuggging.
//#define TRACE_SERIALIZATION

/** A buffer for holding serialized data.
 *
 *  Data is normaly appended to the end of this buffer by several variants
 *  of ::append(). A chunk of memory may also be allocated (at end of buffer)
 *  with ::alloc(). The buffer has a small local storage likely to be sufficent
 *  for most buffer usage. If required it will allocate a buffer extension to
 *  satisfy larger buffer requests.
 *
 * NOTE: When buffer grows, it contents may be relocated ta another memory area.
 *       Pointers returned to ::alloc'ed objects or ::addr() request are therefore
 *       not valid after another ::append() or ::alloc() has been performed.
 *       If pointer persistency is required, use ::getSize() to store the current
 *       end of buffer before the persistent object is allocated or appended.
 *       You may then later use 'size' as a handle to ::addr() to get the address.
 *
 * NOTE: If memory allocation fails during append / alloc, a 'memoryExhausted' state
 *       is set. Further allocation or append will then fail or be ignored. Before
 *       using the contents in the Uint32Buffer, always check ::isMemoryExhausted()
 *       to validate the contents of your buffer.
 */
class Uint32Buffer{
public:
  STATIC_CONST(initSize = 256); // Initial buffer size, extend on demand but probably sufficent

  explicit Uint32Buffer():
    m_extension(NULL),
    m_array(m_local),
    m_avail(initSize),
    m_size(0),
    m_memoryExhausted(false)
  {}

  ~Uint32Buffer() {
    delete[] m_extension;
  }

  /**
   * Allocate a buffer extension at end of this buffer.
   * NOTE: Return NULL if allocation fails and set 
   *       isMemoryExhausted. This will also cause further
   *       alloc() / append() to be skipped.
   */   
  Uint32* alloc(size_t count) {
    size_t reqSize = m_size+count;
    if(unlikely(reqSize >= m_avail)) {
      if (unlikely(m_memoryExhausted)) {
        return NULL;
      }
//    ndbout << "Uint32Buffer::alloc() Extend buffer from: " << m_avail
//           << ", to: " << newSize << endl;
      Uint32* newBuf = new Uint32[reqSize*2];
      if (likely(newBuf!=NULL)) {
        assert(newBuf);
        memcpy (newBuf, m_array, m_size*sizeof(Uint32));
        delete[] m_extension;
        m_array = m_extension = newBuf;
        m_avail = reqSize*2;
      } else {
        m_size = m_avail;
        m_memoryExhausted = true;
        return NULL;
      }
    }
    Uint32* extend = &m_array[m_size];
    m_size += count;
    return extend;
  }

  /** Put the idx'th element already allocated.
   */
  void put(Uint32 idx, Uint32 value) {
    assert(idx < m_size);
    m_array[idx] = value;
  }

  /** append 'src' word to end of this buffer
   */
  void append(const Uint32 src) {
    if (likely(m_size < m_avail)) {
      m_array[m_size++] = src;
    } else {
      Uint32* dst = alloc(1);
      if (likely(dst!=NULL))
        *dst = src;
    }
  }

  /** append 'src' buffer to end of this buffer
   */
  void append(const Uint32Buffer& src) {
    assert (!src.isMemoryExhausted());
    size_t len = src.getSize();
    if (likely(len > 0)) {
      Uint32* dst = alloc(len);
      if (likely(dst!=NULL)) {
        memcpy(dst, src.addr(), len*sizeof(Uint32));
      }
    }
  }

  /** append 'src' *bytes* to end of this buffer
   *  Zero pad possibly odd bytes in last Uint32 word
   */
  void append(const void* src, size_t len) {
    if (likely(len > 0)) {
      size_t wordCount = (len + sizeof(Uint32)-1) / sizeof(Uint32);
      Uint32* dst = alloc(wordCount);
      if (likely(dst!=NULL)) {
        // Make sure that any trailing bytes in the last word are zero.
        dst[wordCount-1] = 0;
        // Copy src 
        memcpy(dst, src, len);
      }
    }
  }

  Uint32* addr(Uint32 idx=0) {
    return (likely(!m_memoryExhausted && m_size>idx)) ?&m_array[idx] :NULL;
  }
  const Uint32* addr(Uint32 idx=0) const {
    return (likely(!m_memoryExhausted && m_size>idx)) ?&m_array[idx] :NULL;
  }

  /** Get the idx'th element. Make sure there is space for 'count' elements.*/
  Uint32 get(Uint32 idx) const {
    assert(idx < m_size);
    return m_array[idx];
  }

  /** Check for possible memory alloc failure during build. */
  bool isMemoryExhausted() const {
    return m_memoryExhausted;
  }

  size_t getSize() const {
    return m_size;
  }

private:
  /** Should not be copied, nor assigned.*/
  Uint32Buffer(Uint32Buffer&);
  Uint32Buffer& operator=(Uint32Buffer&);

private:
  /* TODO: replace array with something that can grow and allocate from a 
   * pool as needed..*/
  Uint32  m_local[initSize];
  Uint32* m_extension;

  Uint32* m_array;  // Refers m_local,  or m_extension if large buffer
  Uint32  m_avail;
  Uint32  m_size;  // <= m_avail
  bool m_memoryExhausted;
};


////////////////////////////////////////////////
// Implementation of NdbQueryOperation interface
////////////////////////////////////////////////

/** For making complex declarations more readable.*/
typedef const void* constVoidPtr;

class NdbQueryOperationDefImpl
{
  friend class NdbQueryOperationImpl;
  friend class NdbQueryImpl;

public:
  /**
   * Different access / query operation types
   */
  enum Type {
    PrimaryKeyAccess,     ///< Read using pk
    UniqueIndexAccess,    ///< Read using unique index
    TableScan,            ///< Full table scan
    OrderedIndexScan      ///< Ordered index scan, optionaly w/ bounds
  };

  Uint32 getNoOfParentOperations() const
  { return m_parents.size(); }

  const NdbQueryOperationDefImpl& getParentOperation(Uint32 i) const
  { return *m_parents[i]; }

  Uint32 getNoOfChildOperations() const
  { return m_children.size(); }

  const NdbQueryOperationDefImpl& getChildOperation(Uint32 i) const
  { return *m_children[i]; }

  const NdbTableImpl& getTable() const
  { return m_table; }

  const char* getName() const
  { return m_ident; }

  Uint32 assignQueryOperationId(Uint32& nodeId)
  { if (getType()==UniqueIndexAccess) nodeId++;
    m_id = nodeId++;
    return m_id;
  }

  // Register a operation as parent of this operation
  void addParent(NdbQueryOperationDefImpl*);

  // Register a linked child refering this operation
  void addChild(NdbQueryOperationDefImpl*);

  // Register a linked reference to a column from operation
  // Return position in list of refered columns available from
  // this (parent) operation. Child ops later refer linked 
  // columns by its position in this list
  Uint32 addColumnRef(const NdbColumnImpl*);

  // Register a param operand which is refered by this operation.
  // Param values are supplied pr. operation when code is serialized.
  void addParamRef(const NdbParamOperandImpl* param)
  { m_params.push_back(param); }

  Uint32 getNoOfParameters() const
  { return m_params.size(); }

  const NdbParamOperandImpl& getParameter(Uint32 ix) const
  { return *m_params[ix]; }

  virtual const NdbIndexImpl* getIndex() const
  { return NULL; }

  // Return 'true' is query type is a multi-row scan
  virtual bool isScanOperation() const = 0;

  virtual const NdbQueryOperationDef& getInterface() const = 0; 

  /** Make a serialized representation of this operation, corresponding to
   * the struct QueryNode type.
   * @return Possible error code.
   */
  virtual int serializeOperation(Uint32Buffer& serializedTree) const = 0;

  /** Find the projection that should be sent to the SPJ block. This should
   * contain the attributes needed to instantiate all child operations.
   */
  const Vector<const NdbColumnImpl*>& getSPJProjection() const{
    return m_spjProjection;
  }

  /**
   * Expand keys and bounds for the root operation into the KEYINFO section.
   * @param keyInfo Actuall KEYINFO section the key / bounds are 
   *                put into
   * @param actualParam Instance values for NdbParamOperands.
   * @param isPruned 'true' for a scan of pruned to single partition.
   * @param hashValue Valid only if 'isPruned'.
   * Returns: 0 if OK, or possible an errorcode.
   */
  virtual int prepareKeyInfo(Uint32Buffer& keyInfo,
                             const constVoidPtr actualParam[]) const = 0;

  virtual int checkPrunable(const Uint32Buffer& keyInfo,
                             bool&   isPruned,
                             Uint32& hashValue) const {
    isPruned = false;
    return 0;
  }

  virtual ~NdbQueryOperationDefImpl() = 0;

protected:
  explicit NdbQueryOperationDefImpl (
                                     const NdbTableImpl& table,
                                     const char* ident,
                                     Uint32      ix)
   : m_table(table), m_ident(ident), m_ix(ix), m_id(ix),
     m_parents(), m_children(), m_params(),
     m_spjProjection()
  {}

  // Get the ordinal position of this operation within the query def.
  Uint32 getQueryOperationIx() const
  { return m_ix; }

  // Get id of node as known inside queryTree
  Uint32 getQueryOperationId() const
  { return m_id; }

  // Get type of query operation
  virtual Type getType() const = 0;

  // QueryTree building:
  // Append list of parent nodes to serialized code
  void appendParentList(Uint32Buffer& serializedDef) const;

private:
  const NdbTableImpl& m_table;
  const char* const m_ident; // Optional name specified by aplication
  const Uint32 m_ix;         // Index of this operation within operation array
  Uint32       m_id;         // Operation id when materialized into queryTree.
                             // If op has index, index id is 'm_id-1'.

  // parent / child vectors contains dependencies as defined
  // with linkedValues
  Vector<NdbQueryOperationDefImpl*> m_parents;
  Vector<NdbQueryOperationDefImpl*> m_children;

  // Params required by this operation
  Vector<const NdbParamOperandImpl*> m_params;

  // Column from this operation required by its child operations
  Vector<const NdbColumnImpl*> m_spjProjection;
}; // class NdbQueryOperationDefImpl


class NdbQueryDefImpl
{
  friend class NdbQueryDef;

public:
  explicit NdbQueryDefImpl(const NdbQueryBuilderImpl& builder, 
                           const Vector<NdbQueryOperationDefImpl*>& operations,
                           int& error);
  ~NdbQueryDefImpl();

  // Entire query is a scan iff root operation is scan. 
  // May change in the future as we implement more complicated SPJ operations.
  bool isScanQuery() const
  { return m_operations[0]->isScanOperation(); }

  Uint32 getNoOfOperations() const
  { return m_operations.size(); }

  // Get a specific NdbQueryOperationDef by ident specified
  // when the NdbQueryOperationDef was created.
  const NdbQueryOperationDefImpl& getQueryOperation(Uint32 index) const
  { return *m_operations[index]; } 

  const NdbQueryOperationDefImpl* getQueryOperation(const char* ident) const;

  const NdbQueryDef& getInterface() const
  { return m_interface; }

  /** Get serialized representation of query definition.*/
  Uint32Buffer& getSerialized()
  { return m_serializedDef; }

  /** Get serialized representation of query definition.*/
  const Uint32Buffer& getSerialized() const
  { return m_serializedDef; }

private:
  NdbQueryDef m_interface;

  Vector<NdbQueryOperationDefImpl*> m_operations;
  Vector<NdbQueryOperandImpl*> m_operands;
  Uint32Buffer m_serializedDef; 
}; // class NdbQueryDefImpl


class NdbQueryBuilderImpl
{
  friend class NdbQueryBuilder;

public:
  ~NdbQueryBuilderImpl();
  explicit NdbQueryBuilderImpl(Ndb& ndb);

  const NdbQueryDefImpl* prepare();

  const NdbError& getNdbError() const;

  void setErrorCode(int aErrorCode)
  { if (!m_error.code)
      m_error.code = aErrorCode;
  }

private:
  bool hasError() const
  { return (m_error.code!=0); }

  bool contains(const NdbQueryOperationDefImpl*);

  Ndb& m_ndb;
  NdbError m_error;

  Vector<NdbQueryOperationDefImpl*> m_operations;
  Vector<NdbQueryOperandImpl*> m_operands;
  Uint32 m_paramCnt;
}; // class NdbQueryBuilderImpl


//////////////////////////////////////////////
// Implementation of NdbQueryOperand interface
//////////////////////////////////////////////

// Baseclass for the QueryOperand implementation
class NdbQueryOperandImpl
{
public:

  /** The type of an operand. This corresponds to the set of subclasses
   * of NdbQueryOperandImpl.
   */
  enum Kind {
    Linked,
    Param,
    Const
  };

  const NdbColumnImpl* getColumn() const
  { return m_column; }

  virtual int bindOperand(const NdbColumnImpl& column,
                          NdbQueryOperationDefImpl& operation)
  { if (m_column  && m_column != &column)
      // Already bounded to a different column
      return QRY_OPERAND_ALREADY_BOUND;
    m_column = &column;
    return 0;
  }
  
  Kind getKind() const
  { return m_kind; }

protected:
  friend NdbQueryBuilderImpl::~NdbQueryBuilderImpl();
  friend NdbQueryDefImpl::~NdbQueryDefImpl();

  virtual ~NdbQueryOperandImpl()=0;

  NdbQueryOperandImpl(Kind kind)
    : m_column(0),
      m_kind(kind)
  {}

protected:
  const NdbColumnImpl* m_column;       // Initial NULL, assigned w/ bindOperand()
  /** This is used to tell the type of an NdbQueryOperand. This allow safe
   * downcasting to a subclass.
   */
  const Kind m_kind;
}; // class NdbQueryOperandImpl


class NdbLinkedOperandImpl : public NdbQueryOperandImpl
{
  friend class NdbQueryBuilder;  // Allow privat access from builder interface

public:
  const NdbQueryOperationDefImpl& getParentOperation() const
  { return m_parentOperation; }

  // 'LinkedSrc' is index into parent op's spj-projection list where
  // the refered column value is available
  Uint32 getLinkedColumnIx() const
  { return m_parentColumnIx; }

  const NdbColumnImpl& getParentColumn() const
  { return *m_parentOperation.getSPJProjection()[m_parentColumnIx]; }

  virtual const NdbLinkedOperand& getInterface() const
  { return m_interface; }

  virtual int bindOperand(const NdbColumnImpl& column,
                          NdbQueryOperationDefImpl& operation);

private:
  virtual ~NdbLinkedOperandImpl() {}

  NdbLinkedOperandImpl (NdbQueryOperationDefImpl& parent, 
                        Uint32 columnIx)
   : NdbQueryOperandImpl(Linked),
     m_interface(*this), 
     m_parentOperation(parent),
     m_parentColumnIx(columnIx)
  {}

  NdbLinkedOperand m_interface;
  NdbQueryOperationDefImpl& m_parentOperation;
  const Uint32 m_parentColumnIx;
}; // class NdbLinkedOperandImpl


class NdbParamOperandImpl : public NdbQueryOperandImpl
{
  friend class NdbQueryBuilder;  // Allow privat access from builder interface

public:
  const char* getName() const
  { return m_name; }

  Uint32 getParamIx() const
  { return m_paramIx; }

  size_t getSizeInBytes(const constVoidPtr paramValue) const;

  virtual const NdbParamOperand& getInterface() const
  { return m_interface; }

  virtual int bindOperand(const NdbColumnImpl& column,
                          NdbQueryOperationDefImpl& operation);

private:
  virtual ~NdbParamOperandImpl() {}
  NdbParamOperandImpl (const char* name, Uint32 paramIx)
   : NdbQueryOperandImpl(Param),
     m_interface(*this), 
     m_name(name),
     m_paramIx(paramIx)
  {}

  NdbParamOperand m_interface;
  const char* const m_name;     // Optional parameter name or NULL
  const Uint32 m_paramIx;
}; // class NdbParamOperandImpl


class NdbConstOperandImpl : public NdbQueryOperandImpl
{
  friend class NdbQueryBuilder;  // Allow privat access from builder interface
public:
  size_t getSizeInBytes()    const
  { return m_converted.len; }
  const void* getAddr() const
  { return likely(m_converted.buffer==NULL) ? &m_converted.val : m_converted.buffer; }

  virtual const NdbConstOperand& getInterface() const
  { return m_interface; }

  virtual int bindOperand(const NdbColumnImpl& column,
                          NdbQueryOperationDefImpl& operation);

protected:
  virtual ~NdbConstOperandImpl() {}
  NdbConstOperandImpl ()
    : NdbQueryOperandImpl(Const),
      m_converted(),
      m_interface(*this)
  {}

  #define UNDEFINED_CONVERSION	\
  { return QRY_OPERAND_HAS_WRONG_TYPE; }

  virtual int convertUint8()  UNDEFINED_CONVERSION;
  virtual int convertInt8()   UNDEFINED_CONVERSION;
  virtual int convertUint16() UNDEFINED_CONVERSION;
  virtual int convertInt16()  UNDEFINED_CONVERSION;
  virtual int convertUint24() UNDEFINED_CONVERSION;
  virtual int convertInt24()  UNDEFINED_CONVERSION;
  virtual int convertUint32() UNDEFINED_CONVERSION;
  virtual int convertInt32()  UNDEFINED_CONVERSION;
  virtual int convertUint64() UNDEFINED_CONVERSION;
  virtual int convertInt64()  UNDEFINED_CONVERSION;
  virtual int convertFloat()  UNDEFINED_CONVERSION;
  virtual int convertDouble() UNDEFINED_CONVERSION

  virtual int convertUDec()   UNDEFINED_CONVERSION;
  virtual int convertDec()    UNDEFINED_CONVERSION;

  virtual int convertBit()    UNDEFINED_CONVERSION;
  virtual int convertChar()   UNDEFINED_CONVERSION;
  virtual int convertVChar()  UNDEFINED_CONVERSION;
  virtual int convertLVChar() UNDEFINED_CONVERSION;
  virtual int convertBin()    UNDEFINED_CONVERSION;
  virtual int convertVBin()   UNDEFINED_CONVERSION;
  virtual int convertLVBin()  UNDEFINED_CONVERSION;

  virtual int convertDate()   UNDEFINED_CONVERSION;
  virtual int convertDatetime() UNDEFINED_CONVERSION;
  virtual int convertTime()   UNDEFINED_CONVERSION;
  virtual int convertYear()   UNDEFINED_CONVERSION;
  virtual int convertTimestamp() UNDEFINED_CONVERSION;


  virtual int convert2ColumnType();


  STATIC_CONST(maxShortChar = 32);

  /** Values converted to datatype format as expected by bound column 
    * (available through ::getColumn())
    */
  class ConvertedValue {
  public:
    ConvertedValue()  : len(0), buffer(NULL) {};
    ~ConvertedValue() {
      if (buffer) delete[] ((char*)buffer);
    };

    union
    {
      Uint32    uint32;
      Int32     int32;
      Uint64    uint64;
      Int64     int64;

      char      shortChar[maxShortChar];
    } val;

    size_t len;
    void*  buffer;  // Optional; storage for converted value
  } m_converted;

private:
  NdbConstOperand m_interface;
}; // class NdbConstOperandImpl


#endif // __cplusplus
#endif
