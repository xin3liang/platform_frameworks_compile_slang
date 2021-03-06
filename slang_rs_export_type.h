/*
 * Copyright 2010-2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _FRAMEWORKS_COMPILE_SLANG_SLANG_RS_EXPORT_TYPE_H_  // NOLINT
#define _FRAMEWORKS_COMPILE_SLANG_SLANG_RS_EXPORT_TYPE_H_

#include <list>
#include <set>
#include <string>
#include <sstream>

#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include "llvm/Support/ManagedStatic.h"

#include "slang_rs_exportable.h"


inline const clang::Type* GetCanonicalType(const clang::Type* T) {
  if (T == nullptr) {
    return  nullptr;
  }
  return T->getCanonicalTypeInternal().getTypePtr();
}

inline const clang::Type* GetCanonicalType(clang::QualType QT) {
  return GetCanonicalType(QT.getTypePtr());
}

inline const clang::Type* GetExtVectorElementType(const clang::ExtVectorType *T) {
  if (T == nullptr) {
    return nullptr;
  }
  return GetCanonicalType(T->getElementType());
}

inline const clang::Type* GetPointeeType(const clang::PointerType *T) {
  if (T == nullptr) {
    return nullptr;
  }
  return GetCanonicalType(T->getPointeeType());
}

inline const clang::Type* GetConstantArrayElementType(const clang::ConstantArrayType *T) {
  if (T == nullptr) {
    return nullptr;
  }
  return GetCanonicalType(T->getElementType());
}


namespace llvm {
  class Type;
}   // namespace llvm

namespace slang {

class RSContext;

// Broad grouping of the data types
enum DataTypeCategory {
    PrimitiveDataType,
    MatrixDataType,
    ObjectDataType
};

// From graphics/java/android/renderscript/Element.java: Element.DataType
/* NOTE: The values of the enums are found compiled in the bit code (i.e. as
 * values, not symbolic.  When adding new types, you must add them to the end.
 * If removing types, you can't re-use the integer value.
 *
 * TODO: but if you do this, you won't be able to keep using First* & Last*
 * for validation.
 *
 * IMPORTANT: This enum should correspond one-for-one to the entries found in the
 * gReflectionsTypes table (except for the two negative numbers).  Don't edit one without
 * the other.
 */
enum DataType {
    DataTypeIsStruct = -2,
    DataTypeUnknown = -1,

    DataTypeFloat16 = 0,
    DataTypeFloat32 = 1,
    DataTypeFloat64 = 2,
    DataTypeSigned8 = 3,
    DataTypeSigned16 = 4,
    DataTypeSigned32 = 5,
    DataTypeSigned64 = 6,
    DataTypeUnsigned8 = 7,
    DataTypeUnsigned16 = 8,
    DataTypeUnsigned32 = 9,
    DataTypeUnsigned64 = 10,
    DataTypeBoolean = 11,
    DataTypeUnsigned565 = 12,
    DataTypeUnsigned5551 = 13,
    DataTypeUnsigned4444 = 14,

    DataTypeRSMatrix2x2 = 15,
    DataTypeRSMatrix3x3 = 16,
    DataTypeRSMatrix4x4 = 17,

    DataTypeRSElement = 18,
    DataTypeRSType = 19,
    DataTypeRSAllocation = 20,
    DataTypeRSSampler = 21,
    DataTypeRSScript = 22,
    DataTypeRSMesh = 23,
    DataTypeRSPath = 24,
    DataTypeRSProgramFragment = 25,
    DataTypeRSProgramVertex = 26,
    DataTypeRSProgramRaster = 27,
    DataTypeRSProgramStore = 28,
    DataTypeRSFont = 29,

    // This should always be last and correspond to the size of the gReflectionTypes table.
    DataTypeMax
};

typedef struct {
    DataTypeCategory category;
    const char * rs_type;
    const char * rs_short_type;
    uint32_t size_in_bits;
    const char * c_name;
    const char * java_name;
    const char * rs_c_vector_prefix;
    const char * rs_java_vector_prefix;
    bool java_promotion;
} RSReflectionType;


typedef struct RSReflectionTypeData_rec {
    const RSReflectionType *type;
    uint32_t vecSize;
    bool isPointer;
    uint32_t arraySize;

    // Subelements
    //std::vector<const struct RSReflectionTypeData_rec *> fields;
    //std::vector< std::string > fieldNames;
    //std::vector< uint32_t> fieldOffsetBytes;
} RSReflectionTypeData;

// Make a name for types that are too complicated to create the real names.
std::string CreateDummyName(const char *type, const std::string &name);

inline bool IsDummyName(const llvm::StringRef &Name) {
  return Name.startswith("<");
}

class RSExportType : public RSExportable {
  friend class RSExportElement;
 public:
  typedef enum {
    ExportClassPrimitive,
    ExportClassPointer,
    ExportClassVector,
    ExportClassMatrix,
    ExportClassConstantArray,
    ExportClassRecord
  } ExportClass;

  void convertToRTD(RSReflectionTypeData *rtd) const;

 private:
  ExportClass mClass;
  std::string mName;

  // Cache the result after calling convertToLLVMType() at the first time
  mutable llvm::Type *mLLVMType;

 protected:
  RSExportType(RSContext *Context,
               ExportClass Class,
               const llvm::StringRef &Name);

  // Let's make it private since there're some prerequisites to call this
  // function.
  //
  // @T was normalized by calling RSExportType::NormalizeType().
  // @TypeName was retrieve from RSExportType::GetTypeName() before calling
  //           this.
  //
  static RSExportType *Create(RSContext *Context,
                              const clang::Type *T,
                              const llvm::StringRef &TypeName);

  static llvm::StringRef GetTypeName(const clang::Type *T);

  // This function convert the RSExportType to LLVM type. Actually, it should be
  // "convert Clang type to LLVM type." However, clang doesn't make this API
  // (lib/CodeGen/CodeGenTypes.h) public, we need to do by ourselves.
  //
  // Once we can get LLVM type, we can use LLVM to get alignment information,
  // allocation size of a given type and structure layout that LLVM used
  // (all of these information are target dependent) without dealing with these
  // by ourselves.
  virtual llvm::Type *convertToLLVMType() const = 0;
  // Record type may recursively reference its type definition. We need a
  // temporary type setup before the type construction gets done.
  inline void setAbstractLLVMType(llvm::Type *LLVMType) const {
    mLLVMType = LLVMType;
  }

  virtual ~RSExportType();

 public:
  // This function additionally verifies that the Type T is exportable.
  // If it is not, this function returns false. Otherwise it returns true.
  static bool NormalizeType(const clang::Type *&T,
                            llvm::StringRef &TypeName,
                            RSContext *Context,
                            const clang::VarDecl *VD);

  // This function checks whether the specified type can be handled by RS/FS.
  // If it cannot, this function returns false. Otherwise it returns true.
  // Filterscript has additional restrictions on supported types.
  static bool ValidateType(slang::RSContext *Context, clang::ASTContext &C,
                           clang::QualType QT, clang::NamedDecl *ND,
                           clang::SourceLocation Loc, unsigned int TargetAPI,
                           bool IsFilterscript);

  // This function ensures that the VarDecl can be properly handled by RS.
  // If it cannot, this function returns false. Otherwise it returns true.
  // Filterscript has additional restrictions on supported types.
  static bool ValidateVarDecl(slang::RSContext *Context, clang::VarDecl *VD,
                              unsigned int TargetAPI, bool IsFilterscript);

  // @T may not be normalized
  static RSExportType *Create(RSContext *Context, const clang::Type *T);
  static RSExportType *CreateFromDecl(RSContext *Context,
                                      const clang::VarDecl *VD);

  static const clang::Type *GetTypeOfDecl(const clang::DeclaratorDecl *DD);

  inline ExportClass getClass() const { return mClass; }

  virtual unsigned getSize() const { return 1; }

  inline llvm::Type *getLLVMType() const {
    if (mLLVMType == nullptr)
      mLLVMType = convertToLLVMType();
    return mLLVMType;
  }

  // Return the maximum number of bytes that may be written when this type is stored.
  virtual size_t getStoreSize() const;

  // Return the distance in bytes between successive elements of this type; it includes padding.
  virtual size_t getAllocSize() const;

  inline const std::string &getName() const { return mName; }

  virtual std::string getElementName() const {
    // Base case is actually an invalid C/Java identifier.
    return "@@INVALID@@";
  }

  virtual bool keep();
  virtual bool equals(const RSExportable *E) const;
};  // RSExportType

// Primitive types
class RSExportPrimitiveType : public RSExportType {
  friend class RSExportType;
  friend class RSExportElement;
 private:
  DataType mType;
  bool mNormalized;

  typedef llvm::StringMap<DataType> RSSpecificTypeMapTy;
  static llvm::ManagedStatic<RSSpecificTypeMapTy> RSSpecificTypeMap;

  static const size_t SizeOfDataTypeInBits[];
  // @T was normalized by calling RSExportType::NormalizeType() before calling
  // this.
  // @TypeName was retrieved from RSExportType::GetTypeName() before calling
  // this
  static RSExportPrimitiveType *Create(RSContext *Context,
                                       const clang::Type *T,
                                       const llvm::StringRef &TypeName,
                                       bool Normalized = false);

 protected:
  RSExportPrimitiveType(RSContext *Context,
                        // for derived class to set their type class
                        ExportClass Class,
                        const llvm::StringRef &Name,
                        DataType DT,
                        bool Normalized)
      : RSExportType(Context, Class, Name),
        mType(DT),
        mNormalized(Normalized) {
  }

  virtual llvm::Type *convertToLLVMType() const;

  static DataType GetDataType(RSContext *Context, const clang::Type *T);

 public:
  // T is normalized by calling RSExportType::NormalizeType() before
  // calling this
  static bool IsPrimitiveType(const clang::Type *T);

  // @T may not be normalized
  static RSExportPrimitiveType *Create(RSContext *Context,
                                       const clang::Type *T);

  static DataType GetRSSpecificType(const llvm::StringRef &TypeName);
  static DataType GetRSSpecificType(const clang::Type *T);

  static bool IsRSMatrixType(DataType DT);
  static bool IsRSObjectType(DataType DT);
  static bool IsRSObjectType(const clang::Type *T) {
    return IsRSObjectType(GetRSSpecificType(T));
  }

  // Determines whether T is [an array of] struct that contains at least one
  // RS object type within it.
  static bool IsStructureTypeWithRSObject(const clang::Type *T);

  static size_t GetSizeInBits(const RSExportPrimitiveType *EPT);

  inline DataType getType() const { return mType; }
  inline bool isRSObjectType() const {
      return IsRSObjectType(mType);
  }

  virtual bool equals(const RSExportable *E) const;

  static RSReflectionType *getRSReflectionType(DataType DT);
  static RSReflectionType *getRSReflectionType(
      const RSExportPrimitiveType *EPT) {
    return getRSReflectionType(EPT->getType());
  }

  virtual unsigned getSize() const { return (GetSizeInBits(this) >> 3); }

  std::string getElementName() const {
    return getRSReflectionType(this)->rs_short_type;
  }
};  // RSExportPrimitiveType


class RSExportPointerType : public RSExportType {
  friend class RSExportType;
  friend class RSExportFunc;
 private:
  const RSExportType *mPointeeType;

  RSExportPointerType(RSContext *Context,
                      const llvm::StringRef &Name,
                      const RSExportType *PointeeType)
      : RSExportType(Context, ExportClassPointer, Name),
        mPointeeType(PointeeType) {
  }

  // @PT was normalized by calling RSExportType::NormalizeType() before calling
  // this.
  static RSExportPointerType *Create(RSContext *Context,
                                     const clang::PointerType *PT,
                                     const llvm::StringRef &TypeName);

  virtual llvm::Type *convertToLLVMType() const;

 public:
  virtual bool keep();

  inline const RSExportType *getPointeeType() const { return mPointeeType; }

  virtual bool equals(const RSExportable *E) const;
};  // RSExportPointerType


class RSExportVectorType : public RSExportPrimitiveType {
  friend class RSExportType;
  friend class RSExportElement;
 private:
  unsigned mNumElement;   // number of element

  RSExportVectorType(RSContext *Context,
                     const llvm::StringRef &Name,
                     DataType DT,
                     bool Normalized,
                     unsigned NumElement)
      : RSExportPrimitiveType(Context, ExportClassVector, Name,
                              DT, Normalized),
        mNumElement(NumElement) {
  }

  // @EVT was normalized by calling RSExportType::NormalizeType() before
  // calling this.
  static RSExportVectorType *Create(RSContext *Context,
                                    const clang::ExtVectorType *EVT,
                                    const llvm::StringRef &TypeName,
                                    bool Normalized = false);

  virtual llvm::Type *convertToLLVMType() const;

 public:
  static llvm::StringRef GetTypeName(const clang::ExtVectorType *EVT);

  inline unsigned getNumElement() const { return mNumElement; }

  std::string getElementName() const {
    std::stringstream Name;
    Name << RSExportPrimitiveType::getRSReflectionType(this)->rs_short_type
         << "_" << getNumElement();
    return Name.str();
  }

  virtual bool equals(const RSExportable *E) const;
};

// Only *square* *float* matrix is supported by now.
//
// struct rs_matrix{2x2,3x3,4x4, ..., NxN} should be defined as the following
// form *exactly*:
//  typedef struct {
//    float m[{NxN}];
//  } rs_matrixNxN;
//
//  where mDim will be N.
class RSExportMatrixType : public RSExportType {
  friend class RSExportType;
 private:
  unsigned mDim;  // dimension

  RSExportMatrixType(RSContext *Context,
                     const llvm::StringRef &Name,
                     unsigned Dim)
    : RSExportType(Context, ExportClassMatrix, Name),
      mDim(Dim) {
  }

  virtual llvm::Type *convertToLLVMType() const;

 public:
  // @RT was normalized by calling RSExportType::NormalizeType() before
  // calling this.
  static RSExportMatrixType *Create(RSContext *Context,
                                    const clang::RecordType *RT,
                                    const llvm::StringRef &TypeName,
                                    unsigned Dim);

  inline unsigned getDim() const { return mDim; }

  virtual bool equals(const RSExportable *E) const;
};

class RSExportConstantArrayType : public RSExportType {
  friend class RSExportType;
 private:
  const RSExportType *mElementType;  // Array element type
  unsigned mSize;  // Array size

  RSExportConstantArrayType(RSContext *Context,
                            const RSExportType *ElementType,
                            unsigned Size)
    : RSExportType(Context, ExportClassConstantArray, "<ConstantArray>"),
      mElementType(ElementType),
      mSize(Size) {
  }

  // @CAT was normalized by calling RSExportType::NormalizeType() before
  // calling this.
  static RSExportConstantArrayType *Create(RSContext *Context,
                                           const clang::ConstantArrayType *CAT);

  virtual llvm::Type *convertToLLVMType() const;

 public:
  virtual unsigned getSize() const { return mSize; }
  inline const RSExportType *getElementType() const { return mElementType; }

  std::string getElementName() const {
    return mElementType->getElementName();
  }

  virtual bool keep();
  virtual bool equals(const RSExportable *E) const;
};

class RSExportRecordType : public RSExportType {
  friend class RSExportType;
 public:
  class Field {
   private:
    const RSExportType *mType;
    // Field name
    std::string mName;
    // Link to the struct that contain this field
    const RSExportRecordType *mParent;
    // Offset in the container
    size_t mOffset;

   public:
    Field(const RSExportType *T,
          const llvm::StringRef &Name,
          const RSExportRecordType *Parent,
          size_t Offset)
        : mType(T),
          mName(Name.data(), Name.size()),
          mParent(Parent),
          mOffset(Offset) {
    }

    inline const RSExportRecordType *getParent() const { return mParent; }
    inline const RSExportType *getType() const { return mType; }
    inline const std::string &getName() const { return mName; }
    inline size_t getOffsetInParent() const { return mOffset; }
  };

  typedef std::list<const Field*>::const_iterator const_field_iterator;

  inline const_field_iterator fields_begin() const {
    return this->mFields.begin();
  }
  inline const_field_iterator fields_end() const {
    return this->mFields.end();
  }

 private:
  std::list<const Field*> mFields;
  bool mIsPacked;
  // Artificial export struct type is not exported by user (and thus it won't
  // get reflected)
  bool mIsArtificial;
  size_t mStoreSize;
  size_t mAllocSize;

  RSExportRecordType(RSContext *Context,
                     const llvm::StringRef &Name,
                     bool IsPacked,
                     bool IsArtificial,
                     size_t StoreSize,
                     size_t AllocSize)
      : RSExportType(Context, ExportClassRecord, Name),
        mIsPacked(IsPacked),
        mIsArtificial(IsArtificial),
        mStoreSize(StoreSize),
        mAllocSize(AllocSize) {
  }

  // @RT was normalized by calling RSExportType::NormalizeType() before calling
  // this.
  // @TypeName was retrieved from RSExportType::GetTypeName() before calling
  // this.
  static RSExportRecordType *Create(RSContext *Context,
                                    const clang::RecordType *RT,
                                    const llvm::StringRef &TypeName,
                                    bool mIsArtificial = false);

  virtual llvm::Type *convertToLLVMType() const;

 public:
  inline const std::list<const Field*>& getFields() const { return mFields; }
  inline bool isPacked() const { return mIsPacked; }
  inline bool isArtificial() const { return mIsArtificial; }
  virtual size_t getStoreSize() const { return mStoreSize; }
  virtual size_t getAllocSize() const { return mAllocSize; }

  virtual std::string getElementName() const {
    return "ScriptField_" + getName();
  }

  virtual bool keep();
  virtual bool equals(const RSExportable *E) const;

  ~RSExportRecordType() {
    for (std::list<const Field*>::iterator I = mFields.begin(),
             E = mFields.end();
         I != E;
         I++)
      if (*I != nullptr)
        delete *I;
  }
};  // RSExportRecordType

}   // namespace slang

#endif  // _FRAMEWORKS_COMPILE_SLANG_SLANG_RS_EXPORT_TYPE_H_  NOLINT
