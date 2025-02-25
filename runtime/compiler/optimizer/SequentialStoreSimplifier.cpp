/*******************************************************************************
 * Copyright IBM Corp. and others 2000
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "optimizer/SequentialStoreSimplifier.hpp"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "codegen/CodeGenerator.hpp"
#include "env/FrontEnd.hpp"
#include "codegen/StorageInfo.hpp"
#include "compile/Compilation.hpp"
#include "compile/SymbolReferenceTable.hpp"
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "cs2/sparsrbit.h"
#include "env/CompilerEnv.hpp"
#include "env/StackMemoryRegion.hpp"
#include "env/TRMemory.hpp"
#include "il/AliasSetInterface.hpp"
#include "il/Block.hpp"
#include "il/DataTypes.hpp"
#include "il/ILOpCodes.hpp"
#include "il/ILOps.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "il/ResolvedMethodSymbol.hpp"
#include "il/Symbol.hpp"
#include "il/SymbolReference.hpp"
#include "il/TreeTop.hpp"
#include "il/TreeTop_inlines.hpp"
#include "infra/Array.hpp"
#include "infra/Assert.hpp"
#include "infra/Cfg.hpp"
#include "infra/Link.hpp"
#include "infra/List.hpp"
#include "infra/TRCfgEdge.hpp"
#include "infra/TreeServices.hpp"
#include "optimizer/Optimization.hpp"
#include "optimizer/OptimizationManager.hpp"
#include "optimizer/Optimizations.hpp"
#include "optimizer/Optimizer.hpp"
#include "optimizer/Structure.hpp"
#include "ras/Debug.hpp"


#define OPT_DETAILS "O^O SEQUENTIAL STORE TRANSFORMATION: "
#define OPT_DETAILS_SCSS "O^O SCSS: "

TR::TreeTop* seqLoadSearchAndCombine(TR::Compilation* comp, bool trace, TR_BitVector* visitedNodes, TR::TreeTop* currentTree, TR::Node* currentNode, NodeForwardList* combineNodeList);
bool isValidSeqLoadCombine(TR::Compilation* comp, bool trace, TR::Node* combineNode, NodeForwardList* combineNodeList, int32_t &combineNodeCount);
bool isValidSeqLoadMulOrShl(TR::Compilation* comp, bool trace, TR::Node* mulOrShlNode);
bool isValidSeqLoadAnd(TR::Compilation* comp, bool trace, TR::Node* andNode);
bool isValidSeqLoadByteConversion(TR::Compilation* comp, bool trace, TR::Node* conversionNode);
TR::TreeTop* generateArraycopyFromSequentialLoads(TR::Compilation* comp, bool trace, TR::TreeTop* currentTreeTop, TR::Node* rootNode, NodeForwardList* combineNodeList);

class TR_ShiftedValueTree
   {
   public:
      TR_ALLOC(TR_Memory::LoopTransformer)

      TR_ShiftedValueTree(TR::Compilation * c) : _comp(c), _rootNode(NULL), _valNode(NULL), _shiftValue(0), _valueSize(-1) {}

      TR::Compilation * comp() { return _comp; }

      int32_t getShiftValue() { return _shiftValue; }
      int32_t getValueSize() { return _valueSize; }
      bool process(TR::Node* loadNode);
      TR::Node* getRootNode() { return _rootNode; }
      TR::Node* getValNode() { return _valNode; }
      bool isConst() { return _isConst; }
   private:
      TR::Compilation * _comp;
      TR::Node* _rootNode;
      TR::Node* _valNode;
      int64_t _shiftValue;
      int32_t _valueSize;
      bool    _isConst;
   };

class TR_SequentialStores
   {
   public:
      TR_ALLOC(TR_Memory::LoopTransformer)
      TR_SequentialStores(TR::Compilation *comp, TR_AllocationKind heapOrStack = heapAlloc);
      bool checkIStore(TR::Node* node);
      TR::SymbolReference* getALoadRef();
      TR::Node * getALoad();
      TR::ILOpCodes getLoadOpCode();
      virtual int32_t getNumBytes()=0;
   protected:
      //TR::SymbolReference* _activeALoadRef;
      TR::Node *_activeALoad;
      TR::ILOpCodes _loadOpCode;
   private:
      TR::Compilation *_comp;
   };

class TR_arraycopySequentialStores : public TR_SequentialStores
   {
   public:

      TR_arraycopySequentialStores(TR::Compilation* comp);

      TR::Compilation * comp() { return _comp; }

      TR_Memory *    trMemory()      { return comp()->trMemory(); }
      TR_StackMemory trStackMemory() { return trMemory(); }

      TR::SymbolReference* getLoadVarRef();
      bool checkALoadValue(TR::Node* loadNode);
      bool checkAiadd(TR::TreeTop * currentTree, TR::Node* aiaddNode);
      bool checkTrees();
      TR::TreeTop *getTreeTop() { return _treeTops[0]; }
      TR_AddressTree* getAddrTree() { return _addrTree[0]; }
      bool checkIStore(TR::Node* node);
      TR_ShiftedValueTree* getVal() { return _val[0]; }
      virtual int32_t getNumBytes();
      bool hasConstValue() { return _val[0]->isConst(); }
      int numTrees();
      int maxNumTrees() { return _maxAddressTrees-1; }
      TR::Node* constValNode();
      bool alternateDir() { return _alternateDir; }
      void removeTrees(TR::SymbolReference * symRef);
   private:
      int numValidTrees(int maxEntries=_maxAddressTrees);
      int64_t constVal();
      void insertTree(int entry);
      bool insertConsistentTree();

      TR::SymbolReference* _activeLoadVarRef;
      TR_AddressTree* _activeAddrTree;
      TR::TreeTop * _activeTreeTop;
      TR_ShiftedValueTree* _activeValueTree;

      // up to 8 trees in a row for a long being stored - add one extra to test for overflow
      enum { _maxAddressTrees=9 };

      TR_AddressTree* _addrTree[_maxAddressTrees];
      TR_ShiftedValueTree* _val[_maxAddressTrees];
      TR::TreeTop * _treeTops[_maxAddressTrees];
      bool _bigEndian;
      bool _alternateDir;
      TR::Compilation* _comp;
      int32_t _numBytes;
   };

class TR_arraysetSequentialStores : public TR_SequentialStores
   {
   public:
      TR_ALLOC(TR_Memory::LoopTransformer)
      TR_arraysetSequentialStores(TR::Compilation *comp, TR_AllocationKind heapOrStack = heapAlloc) :
         TR_SequentialStores(comp), _comp(comp), _processedRefs(false), _activeOffset(-1), _baseOffset(-1), _lastOffset(-1), _indexBase(0), _storeNodeSize(-1) {}
      bool checkConstant(TR::Node* constExpr);
      bool checkArrayStoreConstant(TR::Node *constExpr);
      bool checkALoad(TR::Node* constExpr);
      bool checkStore(TR::Node* node);
      TR::Node *checkArrayStore(TR::Node* storeNode, bool supportsArraySet);
      int64_t getConstant();
      bool getProcessedRefs();
      void setProcessedRefs();
      int32_t getActiveOffset();
      int32_t getBaseOffset();
      void setLastOffset(int32_t offset);
      int32_t getLastOffset() { return _lastOffset; }
      virtual int32_t getNumBytes();
   private:

      TR_AddressTree* _activeAddrTree;
      TR::Compilation *_comp;

      int64_t _initValue;
      bool _processedRefs;
      int32_t _activeOffset;
      int32_t _baseOffset;
      int32_t _lastOffset;
      TR::Node *_indexBase;
      int32_t _storeNodeSize;
   };

bool TR_SequentialStores::checkIStore(TR::Node* istoreNode)
   {
   if (istoreNode->getSize() != istoreNode->getOpCode().getSize())
      return false;

   TR::ILOpCode op = istoreNode->getOpCode();
   // We cannot make the transformation if symbol is unresolved, since offset is not determined yet.
   if (op.isStore() && op.isIndirect() && !op.isWrtBar() && !istoreNode->getSymbolReference()->isUnresolved())
      {
      return true;
      }
   else
      {
      return false;
      }
   }

bool TR_arraycopySequentialStores::checkIStore(TR::Node* istoreNode)
   {
   if (istoreNode->getSize() != istoreNode->getOpCode().getSize())
      return false;

   return (TR_SequentialStores::checkIStore(istoreNode) && istoreNode->getSize() == 1);
   }

bool TR_arraysetSequentialStores::checkStore(TR::Node* storeNode)
   {
   if (storeNode->getSize() != storeNode->getOpCode().getSize())
      return false;

   if (getProcessedRefs())
      {
      if (storeNode->getSize() != _storeNodeSize)
         {
         return false;
         }
      if (storeNode->getSymbolReference()->getOffset() != _activeOffset)
         {
         return false;
         }
      _activeOffset += storeNode->getSize();
      }
   else
      {
      _baseOffset = storeNode->getSymbolReference()->getOffset();
      _lastOffset = _baseOffset;
      _activeOffset = _baseOffset + storeNode->getSize();
      _storeNodeSize = storeNode->getSize();
      }

   return true;
   }


TR::Node *TR_arraysetSequentialStores::checkArrayStore(TR::Node* storeNode, bool supportsArraySet)
   {
   if (!storeNode->getSymbolReference()->getSymbol()->isArrayShadowSymbol())
      return NULL;

   TR::Node *firstChild = storeNode->getFirstChild();
   TR::Node *arrayLoad = NULL;
   TR::Node *offset = NULL;
   if (firstChild->getOpCode().isArrayRef())
      {
      arrayLoad = firstChild->getFirstChild();
      offset = firstChild->getSecondChild();
      }
   else
      arrayLoad = firstChild;

   int32_t offsetValue = storeNode->getSymbolReference()->getOffset();
   if (offset)
      {
       if (offset->getOpCode().isLoadConst())
          {
          if (firstChild->getOpCodeValue() == TR::aiadd)
             offsetValue += offset->getInt();
          else
             {
             if ((offsetValue + offset->getLongInt()) > (int64_t) INT_MAX)
                return NULL;

             offsetValue += (int32_t) offset->getLongInt();
             }
          }
       else
          {
          if ((offset->getOpCodeValue() == TR::iadd) ||
              (offset->getOpCodeValue() == TR::isub) ||
              (offset->getOpCodeValue() == TR::ladd) ||
              (offset->getOpCodeValue() == TR::lsub))
             {
             TR::Node *first = offset->getFirstChild();
             TR::Node *second = offset->getSecondChild();
             if (second->getOpCode().isLoadConst())
                {
                if (offset->getOpCodeValue() == TR::iadd)
                   offsetValue += second->getInt();
                else if (offset->getOpCodeValue() == TR::isub)
                   offsetValue -= second->getInt();
                else
                   {
                   int64_t longValue = 0;
                   if (offset->getOpCodeValue() == TR::ladd)
                      longValue = second->getLongInt();
                   else if (offset->getOpCodeValue() == TR::lsub)
                      longValue = -1*second->getLongInt();

                   if ((offsetValue + longValue) > (int64_t) INT_MAX)
                      return NULL;

                   offsetValue += (int32_t) longValue;
                   }
                }
             else
                {
                //traceMsg(_comp, "1 Returning null at store %p \n", storeNode);
                return NULL;
                }

             if (getProcessedRefs())
                {
                if (_indexBase != first)
                   {
                   //traceMsg(_comp, "2 Returning null at store %p \n", storeNode);
                   return NULL;
                   }
                }
             else
                _indexBase = first;
             }
          else
             {
             //traceMsg(_comp, "3 Returning null at store %p \n", storeNode);
             if (getProcessedRefs())
                {
                if (_indexBase != offset)
                   {
                   //traceMsg(_comp, "2 Returning null at store %p \n", storeNode);
                   return NULL;
                   }
                }
             else
                _indexBase = offset;
             }
          }
      }
   //else
   //   offsetValue = 0;
    if (getProcessedRefs())
      {
      if (storeNode->getSize() != _storeNodeSize)
         {
         return NULL;
         }
      if (offsetValue != _activeOffset)
         {
         //traceMsg(_comp, "4 Returning null at store %p offset %d active %d \n", storeNode, offsetValue, _activeOffset);
         return NULL;
         }

      if (!supportsArraySet)
         {
         if (((_activeOffset + storeNode->getSize()) - _baseOffset) > 8)
            {
            //traceMsg(_comp, "5 Returning null at store %p \n", storeNode);
            return NULL;
            }
         }

      _activeOffset += storeNode->getSize();
      }
   else
      {
      _baseOffset = offsetValue;
      _lastOffset = _baseOffset;
      _activeOffset = _baseOffset + storeNode->getSize();
      _storeNodeSize = storeNode->getSize();
      }

   return arrayLoad;
   }


bool TR_arraysetSequentialStores::checkALoad(TR::Node* aLoadNode)
   {
   if (aLoadNode->getOpCodeValue() != TR::aload)
      {
      return false;
      }

   _loadOpCode = aLoadNode->getOpCodeValue();

   //if (getProcessedRefs() && aLoadNode->getSymbolReference() != _activeALoadRef)
   if (getProcessedRefs() && aLoadNode != _activeALoad)
      {
      return false;
      }
   else
      {
      //_activeALoadRef = aLoadNode->getSymbolReference();
      _activeALoad = aLoadNode;
      }

   return true;
   }

int32_t TR_arraysetSequentialStores::getNumBytes()
   {
   return _lastOffset - _baseOffset;
   }

int32_t TR_arraycopySequentialStores::getNumBytes()
   {
   return _numBytes;
   }

int32_t TR_arraysetSequentialStores::getBaseOffset()
   {
   return _baseOffset;
   }

void TR_arraysetSequentialStores::setLastOffset(int offset)
   {
   _lastOffset = offset;
   }

int64_t TR_arraysetSequentialStores::getConstant()
   {
   return _initValue;
   }

TR::Node* TR_SequentialStores::getALoad()
   {
   return _activeALoad;
   }

TR::SymbolReference* TR_SequentialStores::getALoadRef()
   {
   if (_activeALoad)
      return _activeALoad->getSymbolReference();
   return NULL;
   }

TR::ILOpCodes TR_SequentialStores::getLoadOpCode()
   {
   return _loadOpCode;
   }

TR_SequentialStores::TR_SequentialStores(TR::Compilation* comp, TR_AllocationKind heapOrStack) : _comp(comp)
   {}

bool TR_arraysetSequentialStores::getProcessedRefs()
   {
   return _processedRefs;
   }

void TR_arraysetSequentialStores::setProcessedRefs()
   {
   _processedRefs = true;
   }

int32_t TR_arraysetSequentialStores::getActiveOffset()
   {
   return _activeOffset;
   }

TR_arraycopySequentialStores::TR_arraycopySequentialStores(TR::Compilation* comp) : TR_SequentialStores(comp), _comp(comp), _alternateDir(false)
   {
   memset(_addrTree, 0, sizeof(_addrTree));
   memset(_val, 0, sizeof(_val));
   _bigEndian = comp->target().cpu.isBigEndian();
   }

//   i2b [node cannot overflow]
//       iushr [node >= 0] [node cannot overflow] <iushr disappears when <element> == sizeof(value) - 1>
//         iload <value>
//         iconst (sizeof(<value>) - <element> + 1)*8 (sizeof(<value>) == 1,2,4,8) [Big Endian]
//          -or-
//         iconst (<element> + 1)*8 (sizeof(<value>) == 1,2,4,8)                   [Little Endian]
bool TR_arraycopySequentialStores::checkALoadValue(TR::Node* loadNode)
   {
   // just check general format right now and that shift values are in the right -range-.
   // need to wait until sorted trees list is presented to see if shifts match up with offsets

   _activeValueTree = new (trStackMemory()) TR_ShiftedValueTree(comp());
   return _activeValueTree->process(loadNode);
   }


bool isValidSeqLoadB2i(TR::Compilation * comp, TR::Node* b2iNode)
   {
   TR::Node* firstChild = b2iNode;
   TR::Node* secondChild;
   if (firstChild->getNumChildren() == 0)
      return false;
   firstChild = firstChild->getFirstChild();
   if ( !(firstChild->getOpCodeValue()==TR::bloadi) )
      return false;
   if (firstChild->getNumChildren() == 0)
      return false;
   firstChild = firstChild->getFirstChild();

   if(comp->target().is64Bit())
      {
      if ( !(firstChild->getOpCodeValue()==TR::aladd) )
         return false;
      if (firstChild->getNumChildren() < 2)
         return false;
      secondChild = firstChild->getSecondChild();
      firstChild = firstChild->getFirstChild();
      if  ( !((firstChild->getOpCodeValue()==TR::aload) && (secondChild->getOpCodeValue()==TR::lsub)) )
         return false;
      if (secondChild->getNumChildren() < 2)
         return false;
      firstChild = secondChild->getSecondChild();
      if ( !(firstChild->getOpCodeValue()==TR::lconst) )
         return false;
      }
   else
      {
      if ( !(firstChild->getOpCodeValue()==TR::aiadd) )
         return false;
      if (firstChild->getNumChildren() < 2)
         return false;
      secondChild = firstChild->getSecondChild();
      firstChild = firstChild->getFirstChild();
      if  ( !((firstChild->getOpCodeValue()==TR::aload) && (secondChild->getOpCodeValue()==TR::isub)) )
         return false;
      if (secondChild->getNumChildren() < 2)
         return false;
      firstChild = secondChild->getSecondChild();
      if ( !(firstChild->getOpCodeValue()==TR::iconst) )
         return false;
      }
   return true;
   }


bool isValidSeqLoadIMul(TR::Compilation * comp, TR::Node* imulNode)
   {
   if (imulNode->getOpCodeValue()!=TR::imul)
      return false;
   TR::Node* firstChild = imulNode->getFirstChild();
   TR::Node* secondChild = imulNode->getSecondChild();
   if  ( !((firstChild->getOpCodeValue()==TR::bu2i) && (secondChild->getOpCodeValue()==TR::iconst)) )
      return false;
   return isValidSeqLoadB2i(comp, firstChild);
   }

int32_t getOffsetForSeqLoadDEPRECATED(TR::Compilation * comp, TR::Node* rootNode, int32_t totalBytes, int32_t byteNumber)
   {
   TR::Node* dummyNode = rootNode;
   if (byteNumber==1)
      {
      for (int32_t i = 0; i<totalBytes; i++)
         {
         dummyNode = dummyNode->getFirstChild();
         }
      if(comp->target().is64Bit())
         {
         return dummyNode->getFirstChild()->getFirstChild()->getFirstChild()->getSecondChild()->getSecondChild()->getLongInt() * -1;
         }
      else
         {
         return dummyNode->getFirstChild()->getFirstChild()->getFirstChild()->getSecondChild()->getSecondChild()->getInt()*-1;
         }
      }
   else
      {
      for (int32_t i = 0; i<totalBytes-byteNumber+1; i++)
         {
         dummyNode = dummyNode->getFirstChild();
         }
      if (dummyNode->getSecondChild()->getOpCodeValue()==TR::imul)
         {
         if(comp->target().is64Bit())
            {
            return dummyNode->getSecondChild()->getFirstChild()->getFirstChild()->getFirstChild()->getSecondChild()->getSecondChild()->getLongInt() * -1;
            }
         else
            {
            return dummyNode->getSecondChild()->getFirstChild()->getFirstChild()->getFirstChild()->getSecondChild()->getSecondChild()->getInt() * -1;
            }
         }
      else
         {
         if(comp->target().is64Bit())
            {
            return dummyNode->getSecondChild()->getFirstChild()->getFirstChild()->getSecondChild()->getSecondChild()->getLongInt()*-1;
            }
         else
            {
            return dummyNode->getSecondChild()->getFirstChild()->getFirstChild()->getSecondChild()->getSecondChild()->getInt()*-1;
            }
         }
      }
   }

TR::Node* getBasePointerReferenceForSeqLoadDEPRECATED(TR::Compilation * comp, TR::Node* rootNode, int32_t totalBytes, int32_t byteNumber)
   {
   TR::Node* dummyNode = rootNode;
   if (byteNumber==1)
      {
      for (int32_t i = 0; i<totalBytes; i++)
         {
         dummyNode = dummyNode->getFirstChild();
         }
      return dummyNode->getFirstChild()->getFirstChild()->getFirstChild()->getSecondChild()->getFirstChild()->skipConversions();
      }
   else
      {
      for (int32_t i = 0; i<totalBytes-byteNumber+1; i++)
         {
         dummyNode = dummyNode->getFirstChild();
         }
      if (dummyNode->getSecondChild()->getOpCodeValue()==TR::imul)
         {
         return dummyNode->getSecondChild()->getFirstChild()->getFirstChild()->getFirstChild()->getSecondChild()->getFirstChild()->skipConversions();
         }
      else
         {
         return dummyNode->getSecondChild()->getFirstChild()->getFirstChild()->getSecondChild()->getFirstChild()->skipConversions();
         }
      }
   }

int32_t getMultValueForSeqLoadDEPRECATED(TR::Compilation *comp, TR::Node* rootNode, int32_t totalBytes, int32_t byteNumber)
   {
   TR::Node* dummyNode = rootNode;
   if (byteNumber==1)
      {
      for (int32_t i = 0; i<totalBytes; i++)
         {
         dummyNode = dummyNode->getFirstChild();
         }
      return dummyNode->getSecondChild()->getInt();
      }
   else
      {

      for (int32_t i = 0; i<totalBytes-byteNumber+1; i++)
         {
         dummyNode = dummyNode->getFirstChild();
         }
      if (dummyNode->getSecondChild()->getOpCodeValue()==TR::imul)
         {
         return dummyNode->getSecondChild()->getSecondChild()->getInt();
         }
      else
         {
         return 1;
         }
      }
   }

TR::Node* getALoadReferenceForSeqLoadDEPRECATED(TR::Node* rootNode, int32_t totalBytes, int32_t byteNumber)
   {
   TR::Node* dummyNode = rootNode;
   if (byteNumber==1)
      {
      for (int32_t i = 0; i<totalBytes; i++)
         {
         dummyNode = dummyNode->getFirstChild();
         }
      return dummyNode->getFirstChild()->getFirstChild()->getFirstChild()->getFirstChild();
      }
   else
      {
      for (int32_t i = 0; i<totalBytes-byteNumber+1; i++)
         {
         dummyNode = dummyNode->getFirstChild();
         }
      if (dummyNode->getSecondChild()->getOpCodeValue()==TR::imul)
         {
         return dummyNode->getSecondChild()->getFirstChild()->getFirstChild()->getFirstChild()->getFirstChild();
         }
      else
         {
         return dummyNode->getSecondChild()->getFirstChild()->getFirstChild()->getFirstChild();
         }
      }
   }

TR::TreeTop* seqLoadSearchAndCombine(TR::Compilation* comp, bool trace, TR_BitVector* visitedNodes, TR::TreeTop* currentTree, TR::Node* currentNode, NodeForwardList* combineNodeList)
   {
   /*
    * seqLoadSearchAndCombine recursively searches down the tree looking for a set of nodes that potentially matches
    * the pattern of byte loads to sequential memory locations that are used to construct a larger value.
    * This search looks at opcodes, const values and node reference counts to check for a match.
    * If these checks pass, generateArraycopyFromSequentialLoads is called which does a few more detailed checks and
    * potentially performs the transformation to combine the byte loads together.
    */
   if (visitedNodes->isSet(currentNode->getGlobalIndex()))
      return currentTree;

   visitedNodes->set(currentNode->getGlobalIndex());

   /*
    * Combine nodes are the "add" and "or" nodes that construct the longer value out of several loaded bytes.
    * combineNodeList keeps tracks of the nodes used to construct one particular value. It is populated by isValidSeqLoadCombine.
    * combineNodeList needs to be cleared each time before it is repopulated.
    * isValidSeqLoadCombine also performs a series of initial checks to see if the tree structure contains the pattern
    * of byte loads that can be combined into wider loads.
    * combineNodeCount tracks the number of combine nodes.
    */
   combineNodeList->clear();
   int32_t combineNodeCount = 0;
   if (isValidSeqLoadCombine(comp, trace, currentNode, combineNodeList, combineNodeCount))
      {
      currentTree = generateArraycopyFromSequentialLoads(comp, trace, currentTree, currentNode, combineNodeList);
      }
   else
      {
      /* If the checks in isValidSeqLoadCombine fail, try again on the children. */
      for (int i = 0; i < currentNode->getNumChildren(); i++)
         {
         currentTree = seqLoadSearchAndCombine(comp, trace, visitedNodes, currentTree, currentNode->getChild(i), combineNodeList);
         }
      }

   return currentTree;
   }

bool isValidSeqLoadCombine(TR::Compilation* comp, bool trace, TR::Node* combineNode, NodeForwardList* combineNodeList, int32_t &combineNodeCount)
   {
   /*
    * Combine nodes are the "add" and "or" nodes that construct the longer value out of several loaded bytes.
    * This layer of checks is focused on the "add" and "or" nodes that construct the final value out of the
    * loaded value. It calls other functions the confirm that those loaded values are properly processed byte values.
    */

   /* Check if the candidate combine node is: iadd, ior, ladd, lor */
   if ((combineNode->getOpCodeValue() != TR::iadd) && (combineNode->getOpCodeValue() != TR::ior) &&
       (combineNode->getOpCodeValue() != TR::ladd) && (combineNode->getOpCodeValue() != TR::lor))
      {
      return false;
      }

   /*
    * Only the topmost combine node (the first one to be looked at) can have a reference count greater than 1.
    * If not, it means the intermediate values when constructing the larger value are used in other places and still
    * needs to be generated.
    */
   if ((combineNodeCount > 0) && (combineNode->getReferenceCount() != 1))
      {
      return false;
      }

   /* The node being looked at is a potential combine node so it gets added to the list. */
   combineNodeList->push_front(combineNode);
   combineNodeCount++;

   /*
    * If int width combine nodes are being used, no more than 4 bytes can be combined which means no more than 3 combine nodes.
    * If long width combine nodes are being used, no more than 8 bytes can be combined which means no more than 7 combine nodes.
    */
   if ((combineNodeCount > 3) && ((combineNode->getOpCodeValue() == TR::iadd) || (combineNode->getOpCodeValue() == TR::ior)))
      {
      return false;
      }
   else if ((combineNodeCount > 7) && ((combineNode->getOpCodeValue() == TR::ladd) || (combineNode->getOpCodeValue() == TR::lor)))
      {
      return false;
      }

   TR::Node* childrenArray[2];
   childrenArray[0] = combineNode->getFirstChild();
   childrenArray[1] = combineNode->getSecondChild();

   /*
    * Check if the children match the pattern for combining byte loads.
    * There is an optional mul/shl layer.
    * There is an optional and layer.
    * The byte conversion nodes are the ones on the path to the byte loads.
    * The child can also be another combine node (add/or).
    */
   for (int i = 0; i < 2; i++)
      {
      TR::ILOpCodes childOpCode = childrenArray[i]->getOpCodeValue();

      switch (childOpCode)
         {
         case TR::imul:
         case TR::ishl:
         case TR::lmul:
         case TR::lshl:
            if (!isValidSeqLoadMulOrShl(comp, trace, childrenArray[i]))
               return false;
            break;
         case TR::iand:
         case TR::land:
            if (!isValidSeqLoadAnd(comp, trace, childrenArray[i]))
               return false;
            break;
         case TR::b2i:
         case TR::b2l:
         case TR::bu2i:
         case TR::bu2l:
            if (!isValidSeqLoadByteConversion(comp, trace, childrenArray[i]))
               return false;
            break;
         case TR::iadd:
         case TR::ior:
         case TR::ladd:
         case TR::lor:
            if (!isValidSeqLoadCombine(comp, trace, childrenArray[i], combineNodeList, combineNodeCount))
               return false;
            break;
         default:
            return false;
         }
      }

   return true;
   }

bool isValidSeqLoadMulOrShl(TR::Compilation* comp, bool trace, TR::Node* mulOrShlNode)
   {
   /*
    * A mul or shl node is used to move the loaded byte into the proper position such that they can be combined
    * together with "add" or "or" nodes later on to construct the final value.
    * isValidSeqLoadMulOrShl checks the ref counts on the mul/shl node and that the const child have one of the
    * possible expected values.
    * The validity of the non-const child is handled by calling out to another function.
    */

   /* Confirm that the input mulOrShlNode is one of: imul, ishl, lmul, lshl */
   if ((mulOrShlNode->getOpCodeValue() != TR::imul) && (mulOrShlNode->getOpCodeValue() != TR::ishl) &&
       (mulOrShlNode->getOpCodeValue() != TR::lmul) && (mulOrShlNode->getOpCodeValue() != TR::lshl))
      {
      return false;
      }

   /*
    * mulOrShlNode must have a refCount of 1. After the transformation the intermediate value generated by this node
    * won't be calculated. If the value is needed for something else, the transformation needs to bail out here.
    */
   if (mulOrShlNode->getReferenceCount() != 1)
      {
      return false;
      }

   TR::Node* firstChild = mulOrShlNode->getFirstChild();
   TR::Node* secondChild = mulOrShlNode->getSecondChild();

   /*
    * Check if the first child matches the pattern.
    * There is an optional and layer.
    * The byte conversion nodes are the ones on the path to the byte loads.
    */
   switch (firstChild->getOpCodeValue())
      {
      case TR::iand:
      case TR::land:
         if (!isValidSeqLoadAnd(comp, trace, firstChild))
            return false;
         break;
      case TR::b2i:
      case TR::b2l:
      case TR::bu2i:
      case TR::bu2l:
         if (!isValidSeqLoadByteConversion(comp, trace, firstChild))
            return false;
         break;
      default:
         return false;
      }

   /*
    * The second child must a const node and only certain values are valid.
    * These values correspond with the constants needed to shift a loaded byte left by multiples of 8 bits.
    */
   if ((secondChild->getOpCodeValue() != TR::iconst) && (secondChild->getOpCodeValue() != TR::lconst))
      {
      return false;
      }

   if (mulOrShlNode->getOpCodeValue() == TR::imul)
      {
      switch (secondChild->getInt())
         {
         case 0x100:     /* 256   */
         case 0x10000:   /* 256^2 */
         case 0x1000000: /* 256^3 */
            break;
         default:
            return false;
         }
      }
   else if (mulOrShlNode->getOpCodeValue() == TR::ishl)
      {
      switch (secondChild->getInt())
         {
         case 8:
         case 16:
         case 24:
            break;
         default:
            return false;
         }
      }
   else if (mulOrShlNode->getOpCodeValue() == TR::lmul)
      {
      switch (secondChild->getLongInt())
         {
         case 0x100L:             /* 256   */
         case 0x10000L:           /* 256^2 */
         case 0x1000000L:         /* 256^3 */
         case 0x100000000L:       /* 256^4 */
         case 0x10000000000L:     /* 256^5 */
         case 0x1000000000000L:   /* 256^6 */
         case 0x100000000000000L: /* 256^7 */
            break;
         default:
            return false;
         }
      }
   else /* mulOrShlNode->getOpCodeValue() == TR::lshl */
      {
      switch (secondChild->getInt())
         {
         case 8:
         case 16:
         case 24:
         case 32:
         case 40:
         case 48:
         case 56:
            break;
         default:
            return false;
         }
      }

   return true;
   }

bool isValidSeqLoadAnd(TR::Compilation* comp, bool trace, TR::Node* andNode)
   {
   /*
    * Sometimes the loaded byte is and'd with 0xFF so that bits set due to sign extension are cleared.
    * This is done so those bits do not interfere when the bytes are combined via add/or nodes.
    * The non const child must be a byte to int conversion node which is passed to another function to be
    * verified.
    */

   /* Confirm that the input andNode is one of: iand, land */
   if ((andNode->getOpCodeValue() != TR::iand) && (andNode->getOpCodeValue() != TR::land))
      {
      return false;
      }

   /*
    * andNode must have a refCount of 1. After the transformation the intermediate value generated by this node
    * won't be calculated. If the value is needed for something else, the transformation needs to bail out here.
    */
   if (andNode->getReferenceCount() != 1)
      {
      return false;
      }

   TR::Node* firstChild = andNode->getFirstChild();
   TR::Node* secondChild = andNode->getSecondChild();

   /*
    * Check if the first child matches the pattern.
    * The byte conversion nodes are the ones on the path to the byte loads.
    */
   switch (firstChild->getOpCodeValue())
      {
      case TR::b2i:
      case TR::b2l:
      case TR::bu2i:
      case TR::bu2l:
         if (!isValidSeqLoadByteConversion(comp, trace, firstChild))
            return false;
         break;
      default:
         return false;
      }

   /*
    * The second child must a const node and only 0xFF is valid.
    * This node is supposed to act as a mask so that only 8 bits are affected when this is combined via add/or with the other
    * loaded bytes.
    */
   if (secondChild->getOpCodeValue() == TR::iconst)
      {
      if (secondChild->getInt() != 0xFF)
         {
         return false;
         }
      }
   else if (secondChild->getOpCodeValue() == TR::lconst)
      {
      if (secondChild->getLongInt() != 0xFF)
         {
         return false;
         }
      }
   else
      {
      return false;
      }

   return true;
   }

bool isValidSeqLoadByteConversion(TR::Compilation* comp, bool trace, TR::Node* conversionNode)
   {
   /*
    * This function checks the opcode and reference counts related to nodes that perform the byte load and
    * convert to an int or a long.
    *
    * Example tree:
    * b2i        <--- conversionNode
    *   bloadi
    *     aladd
    *       aload/aloadi
    *       lsub/ladd
    *         **baseOffsetNode**
    *         lconst
    */

   /* Confirm that the input conversionNode is one of: b2i, b2l, bu2i, bu2l */
   if ((conversionNode->getOpCodeValue() != TR::b2i) && (conversionNode->getOpCodeValue() != TR::bu2i) &&
       (conversionNode->getOpCodeValue() != TR::b2l) && (conversionNode->getOpCodeValue() != TR::bu2l))
      {
      return false;
      }

   /*
    * conversionNode must have a refCount of 1. After the transformation the intermediate value generated by this node
    * won't be calculated. If the value is needed for something else, the transformation needs to bail out here.
    */
   if (conversionNode->getReferenceCount() != 1)
      {
      return false;
      }

   TR::Node* firstChild = conversionNode->getFirstChild();
   TR::Node* secondChild = NULL;

   /*
    * conversionNode's child must be a bloadi with a refcount of 1. After the transformation the intermediate value generated by this node
    * won't be calculated. If the value is needed for something else, the transformation needs to bail out here.
    */
   if ((firstChild->getOpCodeValue() != TR::bloadi) || (firstChild->getReferenceCount() != 1))
      {
      return false;
      }

   firstChild = firstChild->getFirstChild();

   if (comp->target().is64Bit())
      {
      /*
       * Under 64bit, bloadi's child must be an aladd node. Some of the aladd nodes go away after the transformation.
       * So, if the refCount is not 1, bail out here.
       */
      if ((firstChild->getOpCodeValue() != TR::aladd) || (firstChild->getReferenceCount() != 1))
         {
         return false;
         }

      secondChild = firstChild->getSecondChild();
      firstChild = firstChild->getFirstChild();

      /* aladd's first child can be aload or aloadi. This load is kept so there is no refcount restriction. */
      if ((firstChild->getOpCodeValue() != TR::aload) && (firstChild->getOpCodeValue() != TR::aloadi))
         {
         return false;
         }

      /*
       * aladd's second child can be an lconst, ladd or lsub. The ladd or lsub might go away after transformation
       * so they must have a refCount of 1.
       */
      if (secondChild->getReferenceCount() != 1)
         {
         if (secondChild->getOpCodeValue() != TR::lconst)
            {
            return false;
            }
         }
      else
         {
         if ((secondChild->getOpCodeValue() != TR::lconst) &&
             (secondChild->getOpCodeValue() != TR::ladd)   &&
             (secondChild->getOpCodeValue() != TR::lsub))
            {
            return false;
            }
         }

      /* If the second child is lsub or ladd, make sure that node's second child is an lconst. */
      if (secondChild->getOpCodeValue() != TR::lconst)
         {
         secondChild = secondChild->getSecondChild();

         if (secondChild->getOpCodeValue() != TR::lconst)
            {
            return false;
            }
         }
      }
   else
      {
      /*
       * Under 32bit, bloadi's child must be an aiadd node. Some of the aiadd nodes go away after the transformation.
       * So, if the refCount is not 1, bail out here.
       */
      if ((firstChild->getOpCodeValue() != TR::aiadd) || (firstChild->getReferenceCount() != 1))
         {
         return false;
         }

      secondChild = firstChild->getSecondChild();
      firstChild = firstChild->getFirstChild();

      /* aiadd's first child can be aload or aloadi. This load is kept so there is no refcount restriction. */
      if ((firstChild->getOpCodeValue() != TR::aload) && (firstChild->getOpCodeValue() != TR::aloadi))
         {
         return false;
         }

      /*
       * aiadd's second child can be an iconst, iadd or isub. The iadd or isub might go away after transformation
       * so they must have a refCount of 1.
       */
      if (secondChild->getReferenceCount() != 1)
         {
         if (secondChild->getOpCodeValue() != TR::iconst)
            {
            return false;
            }
         }
      else
         {
         if ((secondChild->getOpCodeValue() != TR::iconst) &&
             (secondChild->getOpCodeValue() != TR::iadd)   &&
             (secondChild->getOpCodeValue() != TR::isub))
            {
            return false;
            }
         }

      /* If the second child is isub or iadd, make sure that node's second child is an iconst. */
      if (secondChild->getOpCodeValue() != TR::iconst)
         {
         secondChild = secondChild->getSecondChild();

         if (secondChild->getOpCodeValue() != TR::iconst)
            {
            return false;
            }
         }
      }

   return true;
   }

int64_t getOffsetForSeqLoad(TR::Compilation* comp, TR::Node* byteConversionNode)
   {
   /*
    * Returns the constant offset from baseOffsetNode. Offset from aload/aloadi if there is no baseOffsetNode.
    * byteConversionNode must be: b2i, b2l, bu2i, bu2l
    *
    * Example tree: (sub/add version)
    * b2i        <--- byteConversionNode
    *   bloadi
    *     aladd
    *       aload/aloadi
    *       lsub/ladd     <<--- displacementNode
    *         **baseOffsetNode**
    *         lconst      <<--- return value
    *
    *
    * Example tree: (const version)
    * b2i        <--- byteConversionNode
    *   bloadi
    *     aladd
    *       aload/aloadi
    *       lconst        <<--- displacementNode, return value
    */


   if ((byteConversionNode->getOpCodeValue() != TR::b2i) && (byteConversionNode->getOpCodeValue() != TR::bu2i) &&
       (byteConversionNode->getOpCodeValue() != TR::b2l) && (byteConversionNode->getOpCodeValue() != TR::bu2l))
      {
      TR_ASSERT_FATAL_WITH_NODE(byteConversionNode, 0, "Unsupported opCode. This should have been caught earlier. byteConversionNode: %p.", byteConversionNode);
      }

   TR::Node* displacementNode = byteConversionNode->getFirstChild()->getFirstChild()->getSecondChild();

   if (comp->target().is64Bit())
      {
      if (displacementNode->getOpCodeValue() == TR::lconst)
         {
         return displacementNode->getLongInt();
         }
      else if (displacementNode->getOpCodeValue() == TR::lsub)
         {
         return displacementNode->getSecondChild()->getLongInt() * -1;
         }
      else //displacementNode->getOpCodeValue() == TR::ladd
         {
         return displacementNode->getSecondChild()->getLongInt();
         }
      }
   else
      {
      if (displacementNode->getOpCodeValue() == TR::iconst)
         {
         return displacementNode->getInt();
         }
      else if (displacementNode->getOpCodeValue() == TR::isub)
         {
         return displacementNode->getSecondChild()->getInt() * -1;
         }
      else //displacementNode->getOpCodeValue() == TR::iadd
         {
         return displacementNode->getSecondChild()->getInt();
         }
      }
   }

TR::Node* getBaseOffsetForSeqLoad(TR::Node* inputNode)
   {
   /*
    * Returns the baseOffsetNode. Returns NULL if there is no baseOffsetNode.
    * inputNode is a child of a combine node that is not also a combine node.
    * Combine nodes are the add/or nodes that construct the longer value out of seperate loaded bytes.
    *
    * Example tree: (sub/add version)
    * b2i        <--- inputNode
    *   bloadi
    *     aladd
    *       aload/aloadi
    *       lsub/ladd     <<--- displacementNode
    *         **baseOffsetNode**  <<--- return baseOffsetNode (search down past any conversion nodes, if any)
    *         lconst
    *
    *
    * Example tree: (const version)
    * b2i        <--- inputNode
    *   bloadi
    *     aladd
    *       aload/aloadi
    *       lconst        <<--- displacementNode, return NULL
    */


   TR::Node* baseOffsetNode = NULL;
   TR::Node* displacementNode = NULL;

   switch (inputNode->getOpCodeValue())
      {
      case TR::iand:
      case TR::imul:
      case TR::ishl:
      case TR::land:
      case TR::lmul:
      case TR::lshl:
         /* Keep searching down for the b2i node. */
         baseOffsetNode = getBaseOffsetForSeqLoad(inputNode->getFirstChild());
         break;
      case TR::b2i:
      case TR::b2l:
      case TR::bu2i:
      case TR::bu2l:
         displacementNode = inputNode->getFirstChild()->getFirstChild()->getSecondChild();
         if ((displacementNode->getOpCodeValue() == TR::iconst) || (displacementNode->getOpCodeValue() == TR::lconst))
            {
            baseOffsetNode = NULL;
            break;
            }
         baseOffsetNode = displacementNode->getFirstChild()->skipConversions();
         break;
      default:
         TR_ASSERT_FATAL_WITH_NODE(inputNode, 0, "Unsupported opCode. This should have been caught earlier. inputNode: %p.", inputNode);
         break;
      }

   return baseOffsetNode;
   }

int32_t convertMultValueToShiftValue(int64_t multValue)
   {
   /*
    * Converts the constant used for multiplication into the equivalent number of shifted bits.
    * Prior checks should have already been done to ensure that the multiplication value is one of the listed cases.
    */
   switch (multValue)
      {
      case 0x100L:             /* 256   */
         return 8;
      case 0x10000L:           /* 256^2 */
         return 16;
      case 0x1000000L:         /* 256^3 */
         return 24;
      case 0x100000000L:       /* 256^4 */
         return 32;
      case 0x10000000000L:     /* 256^5 */
         return 40;
      case 0x1000000000000L:   /* 256^6 */
         return 48;
      case 0x100000000000000L: /* 256^7 */
         return 56;
      default:
         TR_ASSERT_FATAL(0, "Unknown multValue. This should have been caught earlier. multValue: %ld.", multValue);
         return -1;
      }
   }

int32_t getShiftValueForSeqLoad(TR::Node* inputNode)
   {
   /*
    * Returns the number of bits a byte is shifted by before being combined with the other bytes.
    * The purpose of this is to shift the byte into the right place.
    * If inputNode is a mul node, the mult value is converted to a shift value before being returned.
    * If it is a shl node instead, return the const on the second child.
    * If it is not mul or shift, the shiftValue is 0.
    *
    * inputNode is a child of a combine node that is not also a combine node.
    * Combine nodes are the add/or nodes that construct the longer value out of seperate loaded bytes.
    */

   int32_t shiftValue = 0;

   switch (inputNode->getOpCodeValue())
      {
      case TR::imul:
         shiftValue = convertMultValueToShiftValue(inputNode->getSecondChild()->getInt());
         break;
      case TR::lmul:
         shiftValue = convertMultValueToShiftValue(inputNode->getSecondChild()->getLongInt());
         break;
      case TR::ishl:
      case TR::lshl:
         shiftValue = inputNode->getSecondChild()->getInt();
         break;
      case TR::b2i:
      case TR::b2l:
      case TR::bu2i:
      case TR::bu2l:
      case TR::iand:
      case TR::land:
         shiftValue = 0;
         break;
      default:
         TR_ASSERT_FATAL_WITH_NODE(inputNode, 0, "Unsupported opCode. This should have been caught earlier. inputNode: %p.", inputNode);
         break;
      }

   return shiftValue;
   }

TR::Node* getALoadReferenceForSeqLoad(TR::Node* inputNode)
   {
   /*
    * Returns the node that loads the address of the byte array.
    * inputNode is a child of a combine node that is not also a combine node.
    * Combine nodes are the add/or nodes that construct the longer value out of seperate loaded bytes.
    *
    * Example tree:
    * b2i        <--- inputNode
    *   bloadi
    *     aladd
    *       aload/aloadi  <<--- aloadNode
    *       lsub/ladd
    *         **baseOffsetNode**
    *         lconst
    */


   TR::Node* aloadNode = NULL;

   switch (inputNode->getOpCodeValue())
      {
      case TR::iand:
      case TR::imul:
      case TR::ishl:
      case TR::land:
      case TR::lmul:
      case TR::lshl:
         /* Keep searching down for the b2i node. */
         aloadNode = getALoadReferenceForSeqLoad(inputNode->getFirstChild());
         break;
      case TR::b2i:
      case TR::b2l:
      case TR::bu2i:
      case TR::bu2l:
         aloadNode = inputNode->getFirstChild()->getFirstChild()->getFirstChild();
         break;
      default:
         TR_ASSERT_FATAL_WITH_NODE(inputNode, 0, "Unsupported opCode. This should have been caught earlier. inputNode: %p.", inputNode);
         break;
      }

   return aloadNode;
   }

TR::Node* getByteConversionNodeForSeqLoad(TR::Node* inputNode)
   {
   /*
    * Returns the byte conversion node.
    * inputNode is a child of a combine node that is not also a combine node.
    * Combine nodes are the add/or nodes that construct the longer value out of seperate loaded bytes.
    *
    * Example tree:
    * b2i        <--- inputNode, byteConversionNode
    *   bloadi
    *     aladd
    *       aload/aloadi
    *       lsub/ladd
    *         **baseOffsetNode**
    *         lconst
    */


   TR::Node* byteConversionNode = NULL;

   switch (inputNode->getOpCodeValue())
      {
      case TR::iand:
      case TR::imul:
      case TR::ishl:
      case TR::land:
      case TR::lmul:
      case TR::lshl:
         /* Keep searching down for the b2i node. */
         byteConversionNode = getByteConversionNodeForSeqLoad(inputNode->getFirstChild());
         break;
      case TR::b2i:
      case TR::b2l:
      case TR::bu2i:
      case TR::bu2l:
         byteConversionNode = inputNode;
         break;
      default:
         TR_ASSERT_FATAL_WITH_NODE(inputNode, 0, "Unsupported opCode. This should have been caught earlier. inputNode: %p.", inputNode);
         break;
      }

   return byteConversionNode;
   }

bool checkForSeqLoadSignExtendedByte(TR::Node* inputNode)
   {
   /*
    * Checks if the loaded byte needs to be sign extended or not.
    * If bu2i/bu2l nodes are used, the loaded byte is unsigned.
    * If iand/land nodes are used, the loaded byte was masked with 0xFF and is treated as unsigned.
    * If b2i/b2l nodes are used, the loaded byte is signed. If it is the most significant byte, it will need to
    * be sign extended later. If it isn't the most significant byte, the transformation will bail out since the
    * sign bits may interfere with the other loaded bytes.
    *
    * inputNode is a child of a combine node that is not also a combine node.
    * Combine nodes are the add/or nodes that construct the longer value out of seperate loaded bytes.
    *
    * Example tree:
    * iand     <--- inputNode
    *   b2i
    *     bloadi
    *       aladd
    *         aload/aloadi
    *         lsub/ladd
    *           **baseOffsetNode**
    *           lconst
    *   iconst 0xFF
    */


   bool signExtendedByte = false;

   switch (inputNode->getOpCodeValue())
      {
      case TR::imul:
      case TR::ishl:
      case TR::lmul:
      case TR::lshl:
         /* Keep searching down for the b2i/and node. */
         signExtendedByte = checkForSeqLoadSignExtendedByte(inputNode->getFirstChild());
         break;
      case TR::bu2i:
      case TR::bu2l:
      case TR::iand:
      case TR::land:
         signExtendedByte = false;
         break;
      case TR::b2i:
      case TR::b2l:
         signExtendedByte = true;
         break;
      default:
         TR_ASSERT_FATAL_WITH_NODE(inputNode, 0, "Unsupported opCode. This should have been caught earlier. inputNode: %p.", inputNode);
         break;
      }

   return signExtendedByte;
   }

bool matchLittleEndianSeqLoadPattern(int64_t byteOffsets[], int32_t byteCount)
   {
   /*
    * byteOffsets array contains relative locations that each byte is loaded from.
    * byteCount is the total number of bytes.
    * byteOffsets[0] is the offset of the least significant byte.
    * byteOffsets[byteCount-1] is the offset of the most significant byte.
    * To be an LE load, the offset needs to go *UP* by exactly 1 when going from 0 to (byteCount-1).
    */
   int64_t currentByteOffset = byteOffsets[0];

   for (int i = 1; i < byteCount; i++)
      {
      if (byteOffsets[i] != (currentByteOffset + 1))
         {
         return false;
         }
      currentByteOffset = byteOffsets[i];
      }

   return true;
   }

bool matchBigEndianSeqLoadPattern(int64_t byteOffsets[], int32_t byteCount)
   {
   /*
    * byteOffsets array contains relative locations that each byte is loaded from.
    * byteCount is the total number of bytes.
    * byteOffsets[0] is the offset of the least significant byte.
    * byteOffsets[byteCount-1] is the offset of the most significant byte.
    * To be an BE load, the offset needs to go *DOWN* by exactly 1 when going from 0 to (byteCount-1).
    */
   int64_t currentByteOffset = byteOffsets[0];

   for (int i = 1; i < byteCount; i++)
      {
      if (byteOffsets[i] != (currentByteOffset - 1))
         {
         return false;
         }
      currentByteOffset = byteOffsets[i];
      }

   return true;
   }

bool TR_ShiftedValueTree::process(TR::Node* loadNode)
   {

   bool shift = true;
   TR::ILOpCodes convCode = loadNode->getOpCodeValue();
   TR::ILOpCodes shiftCode, altShiftCode, constCode;
   int32_t size;

   _isConst = false;
   switch(convCode)
      {
      case TR::s2b: shift = true;  _valueSize = 2; shiftCode = TR::sushr; altShiftCode = TR::sshr; constCode = TR::iconst; break;
      case TR::i2b: shift = true;  _valueSize = 4; shiftCode = TR::iushr; altShiftCode = TR::ishr; constCode = TR::iconst; break;
      case TR::l2b: shift = true;  _valueSize = 8; shiftCode = TR::lushr; altShiftCode = TR::lshr; constCode = TR::iconst; break;
      case TR::bload:
      case TR::bconst:
      case TR::sconst:
      case TR::iconst:
      case TR::lconst: shift = false; _valueSize = 1; _shiftValue = 0; break;
      default: return false;
      }

//   dumpOptDetails(comp(), "arraycopy sequential store - check load node %p\n", loadNode);

   if (shift)
      {
      if (loadNode->getFirstChild()->getOpCodeValue() != shiftCode && loadNode->getFirstChild()->getOpCodeValue() != altShiftCode)
         {
         _valNode = loadNode->getFirstChild();
         _shiftValue = 0;
         }
      else
         {
         TR::Node* shiftVal = loadNode->getFirstChild()->getSecondChild();
         if (shiftVal->getOpCodeValue() != constCode)
            {
            dumpOptDetails(comp(), " Shifted Value: did not encounter correct const code\n");
            return false;
            }
         _valNode = loadNode->getFirstChild()->getFirstChild();
         switch(constCode)
            {
            case TR::sconst: _shiftValue = shiftVal->getShortInt(); break;
            case TR::iconst: _shiftValue = shiftVal->getInt(); break;
            case TR::lconst: _shiftValue = shiftVal->getLongInt(); break;
            default: break;
            }
         }
      }
   else
      {
      _valNode = loadNode;
      if (convCode != TR::bload)
         {
         _isConst = true;
         }
      }

   if ((_shiftValue % 8) != 0 || (_shiftValue < 0) || (_shiftValue >= _valueSize*8))
      {
      dumpOptDetails(comp(), "Shifted Value: did not encounter valid shift value %d for var size %d\n", _shiftValue, _valueSize);
      return false;
      }

   _rootNode = loadNode;

   return true;
   }

bool TR_arraycopySequentialStores::checkAiadd(TR::TreeTop * currentTree, TR::Node* aiaddNode)
   {
//   dumpOptDetails(comp(), "arraycopy sequential store - check aiadd node %p\n", aiaddNode);
   _activeAddrTree = new (trStackMemory()) TR_AddressTree(stackAlloc, comp());
   _activeTreeTop = currentTree;
   if (_activeAddrTree->process(aiaddNode, _comp->cg()->getSupportsAlignedAccessOnly()))
      {
      return insertConsistentTree();
      }
   return false;
   }

void TR_arraycopySequentialStores::insertTree(int entry)
   {
//   dumpOptDetails(comp(), " insert nodes %p,%p into entry %d\n",
//      _activeAddrTree->getRootNode(), _activeValueTree->getRootNode(), entry);
   if ((_addrTree[entry] != NULL) && (entry < _maxAddressTrees))
      {
      memmove(&_addrTree[entry+1], &_addrTree[entry], (_maxAddressTrees-entry-1)*sizeof(TR_AddressTree*));
      memmove(&_val[entry+1], &_val[entry],  (_maxAddressTrees-entry-1)*sizeof(TR_ShiftedValueTree*));
      memmove(&_treeTops[entry+1], &_treeTops[entry], (_maxAddressTrees-entry-1)*sizeof(TR::TreeTop*));
      }
   _addrTree[entry] = _activeAddrTree;
   _val[entry] = _activeValueTree;
   _treeTops[entry] = _activeTreeTop;
   }

bool TR_arraycopySequentialStores::insertConsistentTree()
   {

   // these are byte arrays we are working with - the multiplier must be 1
   if (_activeAddrTree->getMultiplier() != 1)
      {
      dumpOptDetails(comp(), " insertTree: multiplier must be 1 in aiadd tree\n");
      return false;
      }

   TR::Node* activeBaseNode = _activeAddrTree->getBaseVarNode()->isNull() ? NULL : _activeAddrTree->getBaseVarNode()->getChild()->skipConversions();
   if (activeBaseNode == NULL)
      {
      dumpOptDetails(comp(), " insertTree: no base variable in aiadd tree\n");
      return false;
      }

   if (_addrTree[0] == NULL)
      {
//      dumpOptDetails(comp(), " insertTree: first tree inserted successfully with offset: %d\n", _activeAddrTree->getOffset());
      insertTree(0);
      return true;
      }

   // make sure the index variable and base variable is consistent with the first tree
   TR::Node* baseNode = _addrTree[0]->getBaseVarNode()->isNull() ? NULL : _addrTree[0]->getBaseVarNode()->getChild()->skipConversions();
   TR::SymbolReference* baseRef = _addrTree[0]->getBaseVarNode()->isNull() ? NULL : _addrTree[0]->getBaseVarNode()->getChild()->skipConversions()->getSymbolReference();
   if (baseNode != activeBaseNode)
      {
      dumpOptDetails(comp(), " insertTree: base variable is different than previous tree\n");
      return false;
      }

   TR::SymbolReference* indRef = _addrTree[0]->getIndVarNode()->isNull() ? NULL : _addrTree[0]->getIndVarNode()->getChild()->skipConversions()->getSymbolReference();
   TR::SymbolReference* activeIndRef = _activeAddrTree->getIndVarNode()->isNull() ? NULL : _activeAddrTree->getIndVarNode()->getChild()->skipConversions()->getSymbolReference();

   TR::Node* indBase = NULL;
   TR::Node* activeIndBase = NULL;
   if (!_addrTree[0]->getIndexBase()->isNull())
      indBase = _addrTree[0]->getIndexBase()->getParent();
   if (!_activeAddrTree->getIndexBase()->isNull())
      activeIndBase = _activeAddrTree->getIndexBase()->getParent();

   if (indRef != activeIndRef || indBase != activeIndBase)
      {
      dumpOptDetails(comp(), " insertTree: index variable is different than previous tree\n");
      return false;
      }

   // make sure the value being loaded is consistent with the first tree
   if (_val[0]->isConst() !=  _activeValueTree->isConst())
      {
      dumpOptDetails(comp(), " insertTree: const value attributes inconsistent\n");
      return false;
      }

   if (!_val[0]->isConst() && (_val[0]->getValNode() != _activeValueTree->getValNode()))
      {
      dumpOptDetails(comp(), " insertTree: value variable is different than previous tree\n");
      return false;
      }

   // make sure the value tree doesn't reference the address tree base var
   if (_activeValueTree->getRootNode()->referencesSymbolInSubTree(baseRef, _comp->incOrResetVisitCount()))
      {
      dumpOptDetails(comp(), " insertTree: value tree %p references address tree base var\n", _activeValueTree->getRootNode());
      return false;
      }

//   dumpOptDetails(comp(), " insertTree: tree inserted successfully. offset:%d\n", _activeAddrTree->getOffset());
   int entry;
   for (entry=0; entry < _maxAddressTrees && _addrTree[entry] != NULL; ++entry)
      {
      if (_activeAddrTree->getOffset() < _addrTree[entry]->getOffset())
         {
         insertTree(entry);
         return true;
         }
      }

   insertTree(entry);
   return true;
   }


int TR_arraycopySequentialStores::numTrees()
   {
   int i;
   for (i=0; i<_maxAddressTrees; ++i)
      {
      if (_addrTree[i] == NULL)
         return i;
      }
   return _maxAddressTrees;
   }

int TR_arraycopySequentialStores::numValidTrees(int maxEntries)
   {
   int dir = (_bigEndian) ? -1 : 1;
   int entry;

//   dumpOptDetails(comp(), "...check if valid entries\n");
   for (entry=1; (_addrTree[entry] != NULL) && (entry < maxEntries); ++entry)
      {
      if (_addrTree[entry]->getOffset() != _addrTree[0]->getOffset() + entry)
         {
         break;
         }
      if (!hasConstValue() && (_val[entry]->getShiftValue() != _val[0]->getShiftValue() + dir*8*entry))
         {
         break;
         }
      }
   if ((entry == 1) && !hasConstValue() && _comp->cg()->supportsByteswap())
      {
//      dumpOptDetails(comp(), "...no valid entries forward\n...see if reverse store finds any entries\n");
      _alternateDir = true;
      // repeat process for reversed store
      dir = (_bigEndian) ? 1 : -1;
      for (entry=1; (_addrTree[entry] != NULL) && (entry < maxEntries); ++entry)
         {
         if (_addrTree[entry]->getOffset() != _addrTree[0]->getOffset() + entry)
            {
            break;
            }
         if (_val[entry]->getShiftValue() != _val[0]->getShiftValue() + dir*8*entry)
            {
            break;
            }
         }
      }
   if (entry > 1)
      {
      dumpOptDetails(comp(), "Found %d sequential valid trees\n", entry);
      }
   return entry;
   }

bool TR_arraycopySequentialStores::checkTrees()
   {
   if (_val[0] == NULL)
      {
      return false;
      }

   if (hasConstValue())
      {
      _numBytes = numValidTrees();
      }
   else
      {
      _numBytes = numValidTrees(_val[0]->getValueSize());
      }

   return (_numBytes == 2 || _numBytes == 4 || _numBytes == 8);
   }

void TR_arraycopySequentialStores::removeTrees(TR::SymbolReference * symRef)
   {
   TR::SparseBitVector fromAliases(_comp->allocator("SCSS bitvector"));

   for (int32_t i = 0; i < _numBytes; ++i)
      {
      dumpOptDetails(_comp, " Remove trees %p to %p\n", _treeTops[i]->getNode(), _treeTops[i]->getNextTreeTop()->getNode());
      TR::TreeTop::removeDeadTrees(_comp, _treeTops[i], _treeTops[i]->getNextTreeTop());
      }

   }

int64_t TR_arraycopySequentialStores::constVal()
   {
   int dir;
   int baseShift;
   uint64_t val = 0;

   if (_bigEndian)
      {
      dir = -1;
      baseShift = (_numBytes-1)*8;
      }
   else
      {
      dir = 1;
      baseShift = 0;
      }

   for (int entry=0; entry < _numBytes; ++entry)
      {
      uint64_t curVal = 0;
      switch (_val[entry]->getValNode()->getOpCodeValue())
         {
         case TR::bconst: curVal = (uint64_t) (_val[entry]->getValNode()->getByte() & 0xFF); break;
         case TR::sconst: curVal = (uint64_t) (_val[entry]->getValNode()->getShortInt() & 0xFF); break;
         case TR::iconst: curVal = (uint64_t) (_val[entry]->getValNode()->getInt() & 0xFF); break;
         case TR::lconst: curVal = (uint64_t) (_val[entry]->getValNode()->getLongInt() & 0xFF); break;
         default: break;
         }
      curVal = curVal << (baseShift + dir*8*entry);
      val |= curVal;
      }
   return (int64_t)val;
   }

TR::Node* TR_arraycopySequentialStores::constValNode()
   {
   TR::Node* rootNode = getAddrTree()->getRootNode();
   TR::Node* node = NULL;

   switch (_numBytes)
      {
      case 1: node = TR::Node::bconst(rootNode,(int8_t) constVal()); break;
      case 2: node = TR::Node::sconst(rootNode,(int16_t) constVal()); break;
      case 4: node = TR::Node::create(rootNode, TR::iconst); node->setInt((int32_t) constVal()); break;
      case 8: node = TR::Node::create(rootNode, TR::lconst); node->setLongInt(constVal()); break;
      default: TR_ASSERT(false, "const val node has invalid number of bytes"); break;
      }
   return node;
   }

// Transform a set of sequential stores into increasing storage from an integral type into
// an arraycopy call
// Order varies depending if on Big Endian or Little Endian target hardware
// bstorei <array element>
//   aiadd
//     aload <arr>
//     isub
//       iload <var>
//       iconst -<base> - <offset> - <element>
//   i2b [node cannot overflow]
//       iushr [node >= 0] [node cannot overflow] <iushr disappears when <element> == sizeof(value) - 1>
//         iload <value>
//         iconst (sizeof(<value>) - <element> + 1)*8 (sizeof(<value>) == 1,2,4,8) [Big Endian]
//          -or-
//         iconst (<element> + 1)*8 (sizeof(<value>) == 1,2,4,8)                   [Little Endian]
//

static TR::TreeTop* generateArraycopyFromSequentialStores(TR::Compilation* comp, TR::TreeTop* prevTreeTop, TR::TreeTop* istoreTreeTop, TR::Node* istoreNode)
   {
   TR::CodeGenerator* codeGen = comp->cg();

   TR_arraycopySequentialStores arraycopy = TR_arraycopySequentialStores(comp);
   TR::Node* curNode = istoreNode;
   TR::TreeTop* curTreeTop = istoreTreeTop;
   while (
          arraycopy.numTrees() < arraycopy.maxNumTrees() &&
          arraycopy.checkIStore(curNode) &&
          arraycopy.checkALoadValue(curNode->getSecondChild()) &&
          arraycopy.checkAiadd(curTreeTop, curNode->getFirstChild())
         )
      {
      curTreeTop = curTreeTop->getNextTreeTop();
      curNode    = curTreeTop->getNode();
      }

   if (!arraycopy.checkTrees())
      {
      return istoreTreeTop;
      }

   int32_t numBytes = arraycopy.getNumBytes();
   if (numBytes == 1)
      {
      dumpOptDetails(comp, " Sequential Store of 1 byte not worth reducing\n");
      return istoreTreeTop;
      }
   else if (numBytes != 2 && numBytes != 4 && numBytes != 8)
      {
      dumpOptDetails(comp, " Sequential Store of size other than 2/4/8 not reducible\n");
      return istoreTreeTop;
      }

   int32_t loadSize = arraycopy.getVal()->getValueSize();
   if (!arraycopy.hasConstValue() && (loadSize != numBytes))
      {
      dumpOptDetails(comp, " Sequential Store of size different than trees (%d,%d) not supported yet\n", loadSize, numBytes);
      return istoreTreeTop;
      }

   if(codeGen->getSupportsAlignedAccessOnly() &&
      ((arraycopy.getAddrTree()->getOffset() % numBytes) != 0))
      {
      dumpOptDetails(comp, " Sequential Store of %d bytes at offset %d would have caused unaligned memory access\n", numBytes, arraycopy.getAddrTree()->getOffset());
      return istoreTreeTop;
      }

   if (!performTransformation(comp, "%sReducing arraycopy sequential stores\n", OPT_DETAILS))
      return istoreTreeTop;

   dumpOptDetails(comp, " Load Node:%p Number of bytes: %d\n", istoreNode, numBytes);

   TR::SymbolReference *symRef = comp->getSymRefTab()->findOrCreateGenericIntShadowSymbolReference(0);
   // propagate the offset of the symbol reference
   symRef->setOffset(arraycopy.getTreeTop()->getNode()->getSymbolReference()->getOffset());

   //
   // delete the bstorei trees and replace them with a new, improved iXstore tree
   //
   arraycopy.removeTrees(symRef);

   TR::Node* aiaddNode = arraycopy.getAddrTree()->getRootNode(); //->duplicateTree();
   TR::Node* loadNode;

   if (arraycopy.hasConstValue())
      {
      loadNode = arraycopy.constValNode();
      }
   else
      {
      loadNode = arraycopy.getVal()->getValNode(); // ->duplicateTree();
      }

   TR::ILOpCodes opcode;
   TR::ILOpCodes byteswapOpcode;

   // create a load/store of the right size
   switch(numBytes)
      {
      case 2: opcode = TR::sstorei; byteswapOpcode = TR::sbyteswap; break;
      case 4: opcode = TR::istorei; byteswapOpcode = TR::ibyteswap; break;
      case 8: opcode = TR::lstorei; byteswapOpcode = TR::lbyteswap; break;
      default: TR_ASSERT(0, " number of bytes unexpected\n"); break;
      }

   if (arraycopy.alternateDir())
      {
      loadNode = TR::Node::create(byteswapOpcode, 1, loadNode);
      }

   TR::Node* iXstoreNode = TR::Node::createWithSymRef(opcode, 2, 2, aiaddNode, loadNode, symRef);
   TR::TreeTop* treeTop = TR::TreeTop::create(comp, iXstoreNode);
   TR::TreeTop* nextTreeTop = prevTreeTop->getNextTreeTop();

   prevTreeTop->join(treeTop);
   treeTop->join(nextTreeTop);

   return treeTop;
   }

static TR::TreeTop* generateArraycopyFromSequentialLoadsDEPRECATED(TR::Compilation* comp, TR::TreeTop* currentTreeTop, TR::Node* bloadiNode)
   {

   static const char * disableSeqLoadOpt = feGetEnv("TR_DisableSeqLoadOpt");
   if (disableSeqLoadOpt)
      return currentTreeTop;

   TR::CodeGenerator* codeGen = comp->cg();

   if (comp->target().cpu.isLittleEndian())
      return currentTreeTop;

   int32_t numBytes = 0;
   TR::Node* currentNode = currentTreeTop->getNode();
   TR::Node* prevNode = NULL;
   TR::Node* rootNode;

   //checking the number of bytes
   while ( !((currentNode->getFirstChild() == bloadiNode) && (currentNode->getOpCodeValue() == TR::bu2i)) )
      {
      if ( (currentNode->getOpCodeValue() == TR::iadd) || (currentNode->getOpCodeValue() == TR::ior))
         {
         numBytes++;
         if (numBytes==1)
            rootNode=prevNode;
         }
      else if (isValidSeqLoadIMul(comp, currentNode))
         numBytes++;
      else
         numBytes = 0;
      prevNode = currentNode;
      currentNode = currentNode->getFirstChild();
      }

   if (numBytes == 1)
      {
      dumpOptDetails(comp, " Sequential Load of 1 byte not worth reducing\n");
      return currentTreeTop;
      }
   else if ( numBytes != 2 && numBytes != 3 && numBytes != 4 )
      {
      dumpOptDetails(comp, " Sequential Load of size other than 2/3/4 not reducible\n");
      return currentTreeTop;
      }

   /*
    * This version of generateArraycopyFromSequentialLoads does not work when numBytes is equal to 2.
    * This version is deprecated so the opt bails out here rather than fix this particular version.
    * Even if it did not bail out here, a check would likely fail later on anyways.
    */
   if (2 == numBytes)
      {
      return currentTreeTop;
      }

   // Need to make sure these loads are not under spine checks.
   //
   if (comp->requiresSpineChecks() && bloadiNode->getReferenceCount() > 1)
      {
      TR::TreeTop *tt = currentTreeTop;
      TR::TreeTop *lastTreeTop = currentTreeTop->getEnclosingBlock()->startOfExtendedBlock()->getFirstRealTreeTop()->getPrevTreeTop();

      // Search backwards to the top of the EBB and look at all spine checks.
      while (tt != lastTreeTop)
         {
         TR::Node *node = tt->getNode();
         if (node->getOpCodeValue() == TR::BNDCHKwithSpineCHK ||
             node->getOpCodeValue() == TR::SpineCHK)
            {
            node = node->getFirstChild();
            while (node->getOpCode().isConversion())
               node = node->getFirstChild();
            if (node == bloadiNode)
               {
               dumpOptDetails(comp, " Sequential Load to spine checked array not reducible\n");
               return currentTreeTop;
               }
            }
         tt = tt->getPrevTreeTop();
         }
      }

   currentNode = rootNode;
   int32_t baseOffset;
   int32_t shiftVar;
   int32_t multiplier = 0;
   TR::Node* aloadNode;
   TR::Node* newLoadChildNode;
   TR::Node* newConvertChildNode;
   TR::Node* newMultChildNode;
   TR::Node* oldChildNode;
   TR::Node* basePointerNode;

   // see if all the trees are valid
   for (int32_t i = 0; i < numBytes; i++)
      {
      currentNode = currentNode->getFirstChild();
      if (currentNode->getReferenceCount() > 1)
         {
         dumpOptDetails(comp, " Sequential load not possible due to ref count > 1 %p\n", currentNode);
         return currentTreeTop;
         }
      if (i == 0)
         {
         if ( !(isValidSeqLoadB2i(comp, currentNode->getSecondChild())) )
            return currentTreeTop;
         baseOffset = getOffsetForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i);
         aloadNode = getALoadReferenceForSeqLoadDEPRECATED(rootNode, numBytes, numBytes - i);
         basePointerNode = getBasePointerReferenceForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i);
         shiftVar = getMultValueForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i);
         if (shiftVar != 1)
            return currentTreeTop;
         }
      else if (i == numBytes-1)
         {
         if ( !(isValidSeqLoadIMul(comp, currentNode)) )
            return currentTreeTop;
         if ( !(shiftVar*multiplier==getMultValueForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i)) )  //This check is broken for numBytes == 2 since multiplier is 0 instead of 256.
            return currentTreeTop;
         if ( !(baseOffset == getOffsetForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i) + i)
            || !(aloadNode == getALoadReferenceForSeqLoadDEPRECATED(rootNode, numBytes, numBytes - i))
            || !(basePointerNode == getBasePointerReferenceForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i)))
            return currentTreeTop;
         baseOffset = getOffsetForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i);
         newLoadChildNode = currentNode->getFirstChild()->getFirstChild();
         newConvertChildNode = currentNode->getFirstChild();
         newMultChildNode = currentNode;
         }
      else
         {
         if ( !(isValidSeqLoadIMul(comp, currentNode->getSecondChild())) )
            return currentTreeTop;
         if ( !(baseOffset == getOffsetForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i) + i)
            || !(aloadNode == getALoadReferenceForSeqLoadDEPRECATED(rootNode, numBytes, numBytes - i))
            || !(basePointerNode == getBasePointerReferenceForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i)))
            return currentTreeTop;
         if (multiplier == 0)
            {
            multiplier = getMultValueForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i);
            shiftVar = multiplier;
            if (!(multiplier == 256))
               return currentTreeTop;
            }
         else
            {
            shiftVar = multiplier * shiftVar;
            if ( !(shiftVar == getMultValueForSeqLoadDEPRECATED(comp, rootNode, numBytes, numBytes - i)) )
               return currentTreeTop;
            }
         }

      }

   if(numBytes == 4 || numBytes == 3 || numBytes == 2)
      {
      if (!performTransformation(comp, "%sReducing sequential loads\n", OPT_DETAILS))
         {
         return currentTreeTop;
         }
      dumpOptDetails(comp, " Sequential Load reduced at node: %p Number of bytes: %d\n", rootNode, numBytes);
      }

   if (numBytes == 4)
      {
      oldChildNode = rootNode->getFirstChild();
      rootNode->setAndIncChild(0, newLoadChildNode);
      oldChildNode->recursivelyDecReferenceCount();
      TR::Node::recreate(newLoadChildNode, TR::iloadi);
      }

   if (numBytes == 3)
      {
      oldChildNode = rootNode->getFirstChild()->getFirstChild();
      rootNode->getFirstChild()->setAndIncChild(0, newMultChildNode);
      oldChildNode->recursivelyDecReferenceCount();
      TR::Node::recreate(newLoadChildNode, TR::sloadi);
      TR::Node::recreate(newConvertChildNode, TR::su2i);
      newMultChildNode->getSecondChild()->setInt(256);
      }

   if (numBytes == 2)
      {
      oldChildNode = rootNode->getFirstChild();
      rootNode->setAndIncChild(0, newConvertChildNode);
      oldChildNode->recursivelyDecReferenceCount();
      TR::Node::recreate(newLoadChildNode, TR::sloadi);
      TR::Node::recreate(newConvertChildNode, TR::su2i);
      }

   return currentTreeTop;
   }

TR::TreeTop* generateArraycopyFromSequentialLoads(TR::Compilation* comp, bool trace, TR::TreeTop* currentTreeTop, TR::Node* rootNode, NodeForwardList* combineNodeList)
   {
   static const char * disableSeqLoadOpt = feGetEnv("TR_DisableSeqLoadOpt");
   if (disableSeqLoadOpt)
      {
      return currentTreeTop;
      }

   /*
    * This is an example transformation. 4 consective byte loads from a byte array are used to construct an int.
    * After the transformation a Little Endian int load is used to directly create the int.
    * Big Endian has support as well but the bytes in the original array need to be in the opposite order or a byteswap is inserted.
    *
    * Original trees:
    * n01n  iadd         <<-- rootNode, combineNode (2)
    * n02n    iadd       <<-- combineNode (1)
    * n03n      iadd     <<-- combineNode (0)
    * n04n        imul                     <<-- processedByteNodes (0)
    * n05n          bu2i                   <<-- byteConversionNode (0)
    * n06n            bloadi
    * n07n              aladd
    * n08n                ==>aloadi        <<-- aloadNode, loads byte array address, used in all 4 byte loads
    * n09n                lsub
    * n10n                  i2l
    * n11n                    iload        <<-- baseOffsetNode, used in all 4 byte loads
    * n12n                  lconst -9      <<-- byteOffsets (0)
    * n13n          iconst 256             <<-- converted to shiftValue (0)
    * n14n        bu2i                   <<-- processedByteNodes (1), byteConversionNode (1)
    * n15n          bloadi
    * n16n            aladd
    * n08n              ==>aloadi
    * n17n              lsub
    * n10n                ==>i2l
    * n18n                lconst -8      <<-- byteOffsets (1)
    * n19n      imul                  <<-- processedByteNodes (2)
    * n20n        bu2i                <<-- byteConversionNode (2)
    * n21n          bloadi
    * n22n            aladd
    * n08n              ==>aloadi
    * n23n              lsub
    * n10n                ==>i2l
    * n24n                lconst -10  <<-- byteOffsets (2)
    * n25n        iconst 0x10000      <<-- convreted to shiftValue (2)
    * n26n    imul                  <<-- processedByteNodes (3)
    * n27n      b2i                 <<-- byteConversionNode (3)
    * n28n        bloadi
    * n29n          aladd
    * n08n            ==>aloadi
    * n30n            lsub
    * n10n              ==>i2l
    * n31n              lconst -11  <<-- byteOffsets (3)
    * n32n      iconst 0x1000000    <<-- converted to shiftValue (3)
    *
    *
    * After transformation:
    * n01n  iloadi         <<-- rootNode
    * n16n    aladd
    * n08n      ==>aloadi
    * n17n      lsub
    * n10n        i2l
    * n11n          iload
    * n18n        lconst -8
    */


   /* processedByteNodes are children of a combine node that are not also a combine node. */
   TR::Node* processedByteNodes[8];
   /* byteConversionNodes are the conversion node parent for each loaded byte. Sorted such that the least significant byte is in position 0. */
   TR::Node* byteConversionNodes[8];
   /* byteOffsets are the constants that act as relative offsets for each loaded byte. Sorted such that the least significant byte is in position 0. */
   int64_t byteOffsets[8];

   /* byteCount is the number of loaded bytes. */
   int32_t byteCount = 0;

   /* These are used to make sure the same aload/aloadi node and baseOffsetNode are used for all of the byte loads. */
   TR::Node* aloadNode = NULL;
   TR::Node* baseOffsetNode = NULL;

   /* Tracks if the final loaded result needs to be sign extended. */
   bool signExtendResult = false;
   /* Tracks if the bytes can be loaded with an Little Endian or Big Endian load. If neither works, the opt bails. */
   bool littleEndianLoad = false;
   /* Tracks if the final loaded result is an int or a long. */
   bool is64bitResult = rootNode->getDataType().isInt64();
   /* Tracks if byteswap are needed due to endianness differences. */
   bool swapBytes = false;

   /* These are used to track nodes while performing the transformation. */
   TR::Node* newLoadChildNode = NULL;
   TR::Node* newConvertChildNode = NULL;
   TR::Node* disconnectedNode1 = NULL;
   TR::Node* disconnectedNode2 = NULL;

   if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p\n", rootNode);

   for (int i = 0; i < 8; i++)
      {
      processedByteNodes[i] = NULL;
      byteConversionNodes[i] = NULL;
      }

   /* Iterate over combineNodeList and populate the processedByteNodes array. */
   for (auto it = combineNodeList->begin(); it != combineNodeList->end(); ++it)
      {
      TR::Node* firstChild = (*it)->getFirstChild();
      TR::Node* secondChild = (*it)->getSecondChild();

      if ((firstChild->getOpCodeValue() != TR::iadd) && (firstChild->getOpCodeValue() != TR::ior) &&
          (firstChild->getOpCodeValue() != TR::ladd) && (firstChild->getOpCodeValue() != TR::lor))
         {
         processedByteNodes[byteCount] = firstChild;
         byteCount++;
         }

      if ((secondChild->getOpCodeValue() != TR::iadd) && (secondChild->getOpCodeValue() != TR::ior) &&
          (secondChild->getOpCodeValue() != TR::ladd) && (secondChild->getOpCodeValue() != TR::lor))
         {
         processedByteNodes[byteCount] = secondChild;
         byteCount++;
         }
      }

   if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, byteCount: %d\n", rootNode, byteCount);

   if (!is64bitResult && (byteCount > 4))
      {
      if (trace) traceMsg(comp, "byteCount is too high for constructing an int value. rootNode: %p, byteCount: %d\n", rootNode, byteCount);
      return currentTreeTop;
      }

   if ((byteCount < 2) || (byteCount > 8))
      {
      if (trace) traceMsg(comp, "Sequential Load of size other than 2 to 8 is not supported. rootNode: %p, byteCount: %d\n", rootNode, byteCount);
      return currentTreeTop;
      }

   /* This loop performs more checks and collects info on each loaded byte. */
   for (int i = 0; i < byteCount; i++)
      {
      TR::Node* byteConversionNode = NULL;
      int32_t shiftValue = 0;

      if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, processedByteNodes[%d]: %p\n", rootNode, i, processedByteNodes[i]);

      if (0 == i)
         {
         aloadNode = getALoadReferenceForSeqLoad(processedByteNodes[i]);
         if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, aloadNode: %p\n", rootNode, aloadNode);
         baseOffsetNode = getBaseOffsetForSeqLoad(processedByteNodes[i]);
         if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, baseOffsetNode: %p\n", rootNode, baseOffsetNode);
         }
      else
         {
         /* aloadNode for all loaded bytes must be the exact same node. */
         if (aloadNode != getALoadReferenceForSeqLoad(processedByteNodes[i]))
            {
            if (trace) traceMsg(comp, "Sequential Load Simplification aload mismatch. rootNode: %p, aloadNode(0): %p, aloadNode(%d): %p\n",
                                rootNode, aloadNode, i, getALoadReferenceForSeqLoad(processedByteNodes[i]));
            return currentTreeTop;
            }

         /* baseOffsetNode for all loaded bytes must be the exact same node (or all NULL). */
         if (baseOffsetNode != getBaseOffsetForSeqLoad(processedByteNodes[i]))
            {
            if (trace) traceMsg(comp, "Sequential Load Simplification baseOffsetNode mismatch. rootNode: %p, baseOffsetNode(0): %p, baseOffsetNode(%d): %p\n",
                                rootNode, baseOffsetNode, i, getBaseOffsetForSeqLoad(processedByteNodes[i]));
            return currentTreeTop;
            }
         }

      byteConversionNode = getByteConversionNodeForSeqLoad(processedByteNodes[i]);
      if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, byteConversionNode: %p\n", rootNode, byteConversionNode);
      shiftValue = getShiftValueForSeqLoad(processedByteNodes[i]);
      if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, shiftValue: %d\n", rootNode, shiftValue);

      if ((shiftValue > ((byteCount - 1) * 8)) || (shiftValue < 0))
         {
         if (trace) traceMsg(comp, "shiftValue is out of range for the given byteCount. rootNode: %p, byteCount: %d, shiftValue: %d\n", rootNode, byteCount, shiftValue);
         return currentTreeTop;
         }

      if (((byteCount - 1) * 8) == shiftValue)
         {
         /* Check if most significant byte is signed extended or not. */
         signExtendResult = checkForSeqLoadSignExtendedByte(processedByteNodes[i]);
         if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, signExtendResult: %d\n", rootNode, signExtendResult);
         }
      else
         {
         /* Other bytes can not be sign extended. */
         if (checkForSeqLoadSignExtendedByte(processedByteNodes[i]))
            {
            if (trace) traceMsg(comp, "Bytes other than the most significant byte should not be sign extended. rootNode: %p, processedByteNodes[%d]: %p\n", rootNode, i, processedByteNodes[i]);
            return currentTreeTop;
            }
         }

      /*
       * byteConversionNodes and byteOffsets are sorted such that the least significant byte load is in position 0.
       * Position 1 has the next most significant load and so on.
       * shiftValue is used to determine the significance of the byte load.
       * If two byte loads have the same shiftValue, the opt bails out.
       */
      int32_t byteNumber = shiftValue / 8;

      if (byteConversionNodes[byteNumber] != NULL)
         {
         if (trace) traceMsg(comp, "Duplicate shiftValue. rootNode: %p, shiftValue: %d, byteConversionNodes[%d]: %p, byteConversionNode(%d): %p\n",
                             rootNode, shiftValue, byteNumber, byteConversionNodes[byteNumber], i, byteConversionNode);
         return currentTreeTop;
         }

      byteConversionNodes[byteNumber] = byteConversionNode;
      byteOffsets[byteNumber] = getOffsetForSeqLoad(comp, byteConversionNode);
      }

   if (trace)
      {
      traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, byteOffsets: %ld", rootNode, byteOffsets[0]);
      for (int i = 1; i < byteCount; i++)
         {
         traceMsg(comp, ", %ld", byteOffsets[i]);
         }
      traceMsg(comp, "\n");
      }

   /*
    * Check if the bytes are loaded from consecutive locations in memory.
    * At this point it is known what the significance of each loaded byte is so check if they can be loaded with an LE or BE load.
    * If both are incompatible, the opt bails out.
    */
   if (matchLittleEndianSeqLoadPattern(byteOffsets, byteCount))
      {
      littleEndianLoad = true;

      if ((3 == byteCount) || (5 == byteCount))
         {
         newConvertChildNode = byteConversionNodes[1];
         newLoadChildNode = byteConversionNodes[1]->getFirstChild();
         }
      else if (6 == byteCount)
         {
         newConvertChildNode = byteConversionNodes[2];
         newLoadChildNode = byteConversionNodes[2]->getFirstChild();
         }
      else if (7 == byteCount)
         {
         newConvertChildNode = byteConversionNodes[3];
         newLoadChildNode = byteConversionNodes[3]->getFirstChild();
         }
      else /* byteCount is 2, 4 or 8. */
         {
         newConvertChildNode = byteConversionNodes[0];
         newLoadChildNode = byteConversionNodes[0]->getFirstChild();
         }
      }
   else if (matchBigEndianSeqLoadPattern(byteOffsets, byteCount))
      {
      littleEndianLoad = false;
      newConvertChildNode = byteConversionNodes[byteCount-1];
      newLoadChildNode = byteConversionNodes[byteCount-1]->getFirstChild();
      }
   else
      {
      if (trace) traceMsg(comp, "byteOffsets array does not match a big or little endian pattern. rootNode: %p\n", rootNode);
      return currentTreeTop;
      }

   if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, %s\n", rootNode, littleEndianLoad ? "Little Endian Load" : "Big Endian Load");
   if (trace) traceMsg(comp, "Sequential Load Simplification Candidate - rootNode: %p, newConvertChildNode: %p, newLoadChildNode: %p\n", rootNode, newConvertChildNode, newLoadChildNode);

   /*
    * Check the endianness of the platform and the endianness of the byte array.
    * If they don't match, byteswaps will be needed.
    */
   if (littleEndianLoad && !comp->target().cpu.isLittleEndian())
      {
      if (!comp->cg()->supportsByteswap())
         {
         if (trace) traceMsg(comp, "Little endian load on big endian target without byteswap is not supported. rootNode: %p\n", rootNode);
         return currentTreeTop;
         }
      swapBytes = true;
      }
   else if (!littleEndianLoad && comp->target().cpu.isLittleEndian())
      {
      if (!comp->cg()->supportsByteswap())
         {
         if (trace) traceMsg(comp, "Big endian load on little endian target without byteswap is not supported. rootNode: %p\n", rootNode);
         return currentTreeTop;
         }
      swapBytes = true;
      }

   if (!performTransformation(comp, "%sReducing sequential loads\n", OPT_DETAILS))
      {
      return currentTreeTop;
      }

   /* All checks have passed so perform the transformation. */
   traceMsg(comp, "Sequential Load reduced at rootNode: %p\n", rootNode);

   disconnectedNode1 = rootNode->getFirstChild();
   disconnectedNode2 = rootNode->getSecondChild();

   TR::SymbolReference *symRef = comp->getSymRefTab()->findOrCreateGenericIntArrayShadowSymbolReference(0);
   symRef->setOffset(newLoadChildNode->getSymbolReference()->getOffset());

   if (((4 == byteCount) && !swapBytes && !is64bitResult) ||
       ((8 == byteCount) && !swapBytes))
      {
      if (4 == byteCount)
         {
         /* byteCount is 4, final result is an int, no byteswaps */
         if (trace) traceMsg(comp, "Recreating rootNode (%p) as iloadi.\n", rootNode);
         TR::Node::recreateWithSymRef(rootNode, TR::iloadi, symRef);
         }
      else /* Handles the (8 == byteCount) case. */
         {
         /* byteCount is 8, final result is a long, no byteswaps */
         if (trace) traceMsg(comp, "Recreating rootNode (%p) as lloadi.\n", rootNode);
         TR::Node::recreateWithSymRef(rootNode, TR::lloadi, symRef);
         }

      rootNode->setNumChildren(1);

      if (trace) traceMsg(comp, "Setting rootNode (%p) child to %p.\n", rootNode, newLoadChildNode->getFirstChild());
      rootNode->setAndIncChild(0, newLoadChildNode->getFirstChild());
      }
   else if (8 == byteCount) /* swapBytes must be true here. */
      {
      /* This case handles adding a byteswap. */
      if (trace) traceMsg(comp, "Recreating rootNode (%p) as lbyteswap.\n", rootNode);
      TR::Node::recreate(rootNode, TR::lbyteswap);

      rootNode->setNumChildren(1);

      if (trace) traceMsg(comp, "Setting rootNode (%p) child to %p.\n", rootNode, newLoadChildNode);
      rootNode->setAndIncChild(0, newLoadChildNode);

      if (trace) traceMsg(comp, "Recreating newLoadChildNode (%p) as lloadi.\n", newLoadChildNode);
      TR::Node::recreateWithSymRef(newLoadChildNode, TR::lloadi, symRef);
      }
   else if (4 == byteCount)
      {
      /* This case handles adding a data width conversion or byteswap. */
      if (is64bitResult)
         {
         /* The conversion used is based on whether the final result is sign extended or not. */
         if (signExtendResult)
            {
            if (trace) traceMsg(comp, "Recreating rootNode (%p) as i2l.\n", rootNode);
            TR::Node::recreate(rootNode, TR::i2l);
            }
         else
            {
            if (trace) traceMsg(comp, "Recreating rootNode (%p) as iu2l.\n", rootNode);
            TR::Node::recreate(rootNode, TR::iu2l);
            }

         rootNode->setNumChildren(1);

         if (!swapBytes)
            {
            if (trace) traceMsg(comp, "Setting rootNode (%p) child to %p.\n", rootNode, newLoadChildNode);
            rootNode->setAndIncChild(0, newLoadChildNode);
            }
         else
            {
            /* Inserts byteswap above the load. */
            TR::Node * ibyteswapNode = TR::Node::create(TR::ibyteswap, 1, newLoadChildNode);
            if (trace) traceMsg(comp, "Creating ibyteswapNode (%p) with child %p.\n", ibyteswapNode, ibyteswapNode->getFirstChild());

            if (trace) traceMsg(comp, "Setting rootNode (%p) child to %p.\n", rootNode, ibyteswapNode);
            rootNode->setAndIncChild(0, ibyteswapNode);
            }
         }
      else /* Handles the (swapBytes && !is64bitResult) case. */
         {
         /* Inserts byteswap above the load. */
         if (trace) traceMsg(comp, "Recreating rootNode (%p) as ibyteswap.\n", rootNode);
         TR::Node::recreate(rootNode, TR::ibyteswap);

         rootNode->setNumChildren(1);

         if (trace) traceMsg(comp, "Setting rootNode (%p) child to %p.\n", rootNode, newLoadChildNode);
         rootNode->setAndIncChild(0, newLoadChildNode);
         }

      if (trace) traceMsg(comp, "Recreating newLoadChildNode (%p) as iloadi.\n", newLoadChildNode);
      TR::Node::recreateWithSymRef(newLoadChildNode, TR::iloadi, symRef);
      }
   else if (2 == byteCount)
      {
      /* The conversion used is based on whether the final result is an int or a long and if there is a sign extend or not. */
      if (signExtendResult)
         {
         if (is64bitResult)
            {
            if (trace) traceMsg(comp, "Recreating rootNode (%p) as s2l.\n", rootNode);
            TR::Node::recreate(rootNode, TR::s2l);
            }
         else
            {
            if (trace) traceMsg(comp, "Recreating rootNode (%p) as s2i.\n", rootNode);
            TR::Node::recreate(rootNode, TR::s2i);
            }
         }
      else
         {
         if (is64bitResult)
            {
            if (trace) traceMsg(comp, "Recreating rootNode (%p) as su2l.\n", rootNode);
            TR::Node::recreate(rootNode, TR::su2l);
            }
         else
            {
            if (trace) traceMsg(comp, "Recreating rootNode (%p) as su2i.\n", rootNode);
            TR::Node::recreate(rootNode, TR::su2i);
            }
         }

      rootNode->setNumChildren(1);

      if (!swapBytes)
         {
         if (trace) traceMsg(comp, "Setting rootNode (%p) child to %p.\n", rootNode, newLoadChildNode);
         rootNode->setAndIncChild(0, newLoadChildNode);
         }
      else
         {
         /* Inserts byteswap above the load. */
         TR::Node * sbyteswapNode = TR::Node::create(TR::sbyteswap, 1, newLoadChildNode);
         if (trace) traceMsg(comp, "Creating sbyteswapNode (%p) with child %p.\n", sbyteswapNode, sbyteswapNode->getFirstChild());

         if (trace) traceMsg(comp, "Setting rootNode (%p) child to %p.\n", rootNode, sbyteswapNode);
         rootNode->setAndIncChild(0, sbyteswapNode);
         }

      if (trace) traceMsg(comp, "Recreating newLoadChildNode (%p) as sloadi.\n", newLoadChildNode);
      TR::Node::recreateWithSymRef(newLoadChildNode, TR::sloadi, symRef);
      }
   else if (3 == byteCount)
      {
      /*
       * This case performs two loads.
       * One is a short load that is then shifted to the left by 1 byte.
       * The second is a byte load.
       * The two values are combined to get the final 3 byte value.
       *
       *          b0  bload
       *  s1  s0      sload * 256
       *  ----------
       *  s1  s0  b0  final 3 byte value
       */
      TR::Node * mulNode = NULL;
      if (is64bitResult)
         {
         mulNode = TR::Node::create(TR::lmul, 2, newConvertChildNode, TR::Node::create(TR::lconst, 0, 256));
         }
      else
         {
         mulNode = TR::Node::create(TR::imul, 2, newConvertChildNode, TR::Node::create(TR::iconst, 0, 256));
         }
      if (trace) traceMsg(comp, "Creating mulNode (%p) with children %p and %p.\n", mulNode, mulNode->getFirstChild(), mulNode->getSecondChild());

      if (trace) traceMsg(comp, "Setting rootNode (%p) children to %p and %p.\n", rootNode, mulNode, byteConversionNodes[0]);
      rootNode->setAndIncChild(0, mulNode);
      rootNode->setAndIncChild(1, byteConversionNodes[0]);

      /* The conversion used is based on whether the final result is an int or a long. The byte is never sign extended.*/
      if (is64bitResult)
         {
         if (trace) traceMsg(comp, "Recreating byteConversionNodes[0] (%p) as bu2l.\n", byteConversionNodes[0]);
         TR::Node::recreate(byteConversionNodes[0], TR::bu2l);
         }
      else
         {
         if (trace) traceMsg(comp, "Recreating byteConversionNodes[0] (%p) as bu2i.\n", byteConversionNodes[0]);
         TR::Node::recreate(byteConversionNodes[0], TR::bu2i);
         }

      if (trace) traceMsg(comp, "Recreating newLoadChildNode (%p) as sloadi.\n", newLoadChildNode);
      TR::Node::recreateWithSymRef(newLoadChildNode, TR::sloadi, symRef);

      /* The conversion used is based on whether the final result is an int or a long and if there is a sign extend or not. */
      if (signExtendResult)
         {
         if (is64bitResult)
            {
            if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as s2l.\n", newConvertChildNode);
            TR::Node::recreate(newConvertChildNode, TR::s2l);
            }
         else
            {
            if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as s2i.\n", newConvertChildNode);
            TR::Node::recreate(newConvertChildNode, TR::s2i);
            }
         }
      else
         {
         if (is64bitResult)
            {
            if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as su2l.\n", newConvertChildNode);
            TR::Node::recreate(newConvertChildNode, TR::su2l);
            }
         else
            {
            if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as su2i.\n", newConvertChildNode);
            TR::Node::recreate(newConvertChildNode, TR::su2i);
            }
         }

      if (swapBytes)
         {
         /* Inserts byteswap above the load. */
         TR::Node * sbyteswapNode = TR::Node::create(TR::sbyteswap, 1, newLoadChildNode);
         if (trace) traceMsg(comp, "Creating sbyteswapNode (%p) with child %p.\n", sbyteswapNode, sbyteswapNode->getFirstChild());

         if (trace) traceMsg(comp, "Setting newConvertChildNode (%p) child to %p.\n", newConvertChildNode, sbyteswapNode);
         newConvertChildNode->setAndIncChild(0, sbyteswapNode);
         newLoadChildNode->recursivelyDecReferenceCount();
         }
      }
   else if (5 == byteCount)
      {
      /*
       * This case performs two loads.
       * One is a int load that is then shifted to the left by 1 byte.
       * The second is a byte load.
       * The two values are combined to get the final 5 byte value.
       *
       *                  b0  bload
       *  i3  i2  i1  i0      iload * 256
       *  ------------------
       *  i3  i2  i1  i0  b0  final 5 byte value
       */
      TR::Node * mulNode = TR::Node::create(TR::lmul, 2, newConvertChildNode, TR::Node::create(TR::lconst, 0, 256));

      if (trace) traceMsg(comp, "Creating mulNode (%p) with children %p and %p.\n", mulNode, mulNode->getFirstChild(), mulNode->getSecondChild());

      if (trace) traceMsg(comp, "Setting rootNode (%p) children to %p and %p.\n", rootNode, mulNode, byteConversionNodes[0]);
      rootNode->setAndIncChild(0, mulNode);
      rootNode->setAndIncChild(1, byteConversionNodes[0]);

      /* The byte is never sign extended. */
      if (trace) traceMsg(comp, "Recreating byteConversionNodes[0] (%p) as bu2l.\n", byteConversionNodes[0]);
      TR::Node::recreate(byteConversionNodes[0], TR::bu2l);

      if (trace) traceMsg(comp, "Recreating newLoadChildNode (%p) as iloadi.\n", newLoadChildNode);
      TR::Node::recreateWithSymRef(newLoadChildNode, TR::iloadi, symRef);

      /* The conversion used is based on whether the final result is sign extended or not. */
      if (signExtendResult)
         {
         if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as i2l.\n", newConvertChildNode);
         TR::Node::recreate(newConvertChildNode, TR::i2l);
         }
      else
         {
         if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as iu2l.\n", newConvertChildNode);
         TR::Node::recreate(newConvertChildNode, TR::iu2l);
         }

      if (swapBytes)
         {
         /* Inserts byteswap above the load. */
         TR::Node * ibyteswapNode = TR::Node::create(TR::ibyteswap, 1, newLoadChildNode);
         if (trace) traceMsg(comp, "Creating ibyteswapNode (%p) with child %p.\n", ibyteswapNode, ibyteswapNode->getFirstChild());

         if (trace) traceMsg(comp, "Setting newConvertChildNode (%p) child to %p.\n", newConvertChildNode, ibyteswapNode);
         newConvertChildNode->setAndIncChild(0, ibyteswapNode);
         newLoadChildNode->recursivelyDecReferenceCount();
         }
      }
   else if (6 == byteCount)
      {
      /*
       * This case performs two loads.
       * One is a int load that is then shifted to the left by 2 bytes.
       * The second is a short load.
       * The two values are combined to get the final 6 byte value.
       *
       *                  s1  s0  sload
       *  i3  i2  i1  i0          iload * 256^2
       *  ----------------------
       *  i3  i2  i1  i0  s1  s0  final 6 byte value
       */
      TR::Node * mulNode = TR::Node::create(TR::lmul, 2, newConvertChildNode, TR::Node::create(TR::lconst, 0, 256*256));
      if (trace) traceMsg(comp, "Creating mulNode (%p) with children %p and %p.\n", mulNode, mulNode->getFirstChild(), mulNode->getSecondChild());

      TR::Node * shortConversionNode = NULL;
      /* The location to start the sload from depends on endianness of the byte array. */
      if (littleEndianLoad)
         {
         shortConversionNode = byteConversionNodes[0];
         }
      else
         {
         shortConversionNode = byteConversionNodes[1];
         }
      TR::Node * shortLoadNode = shortConversionNode->getFirstChild();

      if (trace) traceMsg(comp, "Setting rootNode (%p) children to %p and %p.\n", rootNode, mulNode, shortConversionNode);
      rootNode->setAndIncChild(0, mulNode);
      rootNode->setAndIncChild(1, shortConversionNode);

      TR::SymbolReference *shortSymRef = comp->getSymRefTab()->findOrCreateGenericIntArrayShadowSymbolReference(0);
      shortSymRef->setOffset(shortLoadNode->getSymbolReference()->getOffset());

      if (trace) traceMsg(comp, "Recreating shortLoadNode (%p) as sloadi.\n", shortLoadNode);
      TR::Node::recreateWithSymRef(shortLoadNode, TR::sloadi, shortSymRef);

      /* The short is never sign extended. */
      if (trace) traceMsg(comp, "Recreating shortConversionNode (%p) as su2l.\n", shortConversionNode);
      TR::Node::recreate(shortConversionNode, TR::su2l);

      if (trace) traceMsg(comp, "Recreating newLoadChildNode (%p) as iloadi.\n", newLoadChildNode);
      TR::Node::recreateWithSymRef(newLoadChildNode, TR::iloadi, symRef);

      /* The conversion used is based on whether the final result is sign extended or not. */
      if (signExtendResult)
         {
         if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as i2l.\n", newConvertChildNode);
         TR::Node::recreate(newConvertChildNode, TR::i2l);
         }
      else
         {
         if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as iu2l.\n", newConvertChildNode);
         TR::Node::recreate(newConvertChildNode, TR::iu2l);
         }

      if (swapBytes)
         {
         /* Inserts byteswap above both loads. */
         TR::Node * sbyteswapNode = TR::Node::create(TR::sbyteswap, 1, shortLoadNode);
         if (trace) traceMsg(comp, "Creating sbyteswapNode (%p) with child %p.\n", sbyteswapNode, sbyteswapNode->getFirstChild());

         if (trace) traceMsg(comp, "Setting shortConversionNode (%p) child to %p.\n", shortConversionNode, sbyteswapNode);
         shortConversionNode->setAndIncChild(0, sbyteswapNode);
         shortLoadNode->recursivelyDecReferenceCount();

         TR::Node * ibyteswapNode = TR::Node::create(TR::ibyteswap, 1, newLoadChildNode);
         if (trace) traceMsg(comp, "Creating ibyteswapNode (%p) with child %p.\n", ibyteswapNode, ibyteswapNode->getFirstChild());

         if (trace) traceMsg(comp, "Setting newConvertChildNode (%p) child to %p.\n", newConvertChildNode, ibyteswapNode);
         newConvertChildNode->setAndIncChild(0, ibyteswapNode);
         newLoadChildNode->recursivelyDecReferenceCount();
         }
      }
   else if (7 == byteCount)
      {
      /*
       * This case performs two loads.
       * One is a int load that is then shifted to the left by 3 bytes.
       * The second is also an int load that overlaps the first load by 1 byte but the highest byte is masked out.
       * The two values are combined to get the final 7 byte value.
       *
       *              xx  a2  a1  a0  iload & 0x00FFFFFF
       *  b3  b2  b1  b0              iload * 256^3
       *  --------------------------
       *  b3  b2  b1  b0  a2  a1  a0  final 7 byte value
       */
      TR::Node * mulNode = TR::Node::create(TR::lmul, 2, newConvertChildNode, TR::Node::create(TR::lconst, 0, 256*256*256));
      if (trace) traceMsg(comp, "Creating mulNode (%p) with children %p and %p.\n", mulNode, mulNode->getFirstChild(), mulNode->getSecondChild());

      TR::Node * lowerIntConversionNode = NULL;
      /* The location to start the lower iload from depends on endianness of the byte array. */
      if (littleEndianLoad)
         {
         lowerIntConversionNode = byteConversionNodes[0];
         }
      else
         {
         lowerIntConversionNode = byteConversionNodes[3];
         }
      TR::Node * lowerIntLoadNode = lowerIntConversionNode->getFirstChild();

      /* The lower in load needs to have its highest byte masked out. */
      TR::Node * andNode = TR::Node::create(TR::land, 2, lowerIntConversionNode, TR::Node::create(TR::lconst, 0, 0xFFFFFF));
      if (trace) traceMsg(comp, "Creating andNode (%p) with children %p and %p.\n", andNode, andNode->getFirstChild(), andNode->getSecondChild());

      if (trace) traceMsg(comp, "Setting rootNode (%p) children to %p and %p.\n", rootNode, mulNode, lowerIntConversionNode);
      rootNode->setAndIncChild(0, mulNode);
      rootNode->setAndIncChild(1, andNode);

      TR::SymbolReference *lowerSymRef = comp->getSymRefTab()->findOrCreateGenericIntArrayShadowSymbolReference(0);
      lowerSymRef->setOffset(lowerIntLoadNode->getSymbolReference()->getOffset());

      if (trace) traceMsg(comp, "Recreating lowerIntLoadNode (%p) as iloadi.\n", lowerIntLoadNode);
      TR::Node::recreateWithSymRef(lowerIntLoadNode, TR::iloadi, lowerSymRef);

      /* Both a signed or unsigned conversion could work here since the result gets masked later anyways. */
      if (trace) traceMsg(comp, "Recreating lowerIntConversionNode (%p) as i2l.\n", lowerIntConversionNode);
      TR::Node::recreate(lowerIntConversionNode, TR::i2l);

      if (trace) traceMsg(comp, "Recreating newLoadChildNode (%p) as iloadi.\n", newLoadChildNode);
      TR::Node::recreateWithSymRef(newLoadChildNode, TR::iloadi, symRef);

      /* The conversion used is based on whether the final result is sign extended or not. */
      if (signExtendResult)
         {
         if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as i2l.\n", newConvertChildNode);
         TR::Node::recreate(newConvertChildNode, TR::i2l);
         }
      else
         {
         if (trace) traceMsg(comp, "Recreating newConvertChildNode (%p) as iu2l.\n", newConvertChildNode);
         TR::Node::recreate(newConvertChildNode, TR::iu2l);
         }

      if (swapBytes)
         {
         /* Inserts byteswap above both loads. */
         TR::Node * ibyteswapNode1 = TR::Node::create(TR::ibyteswap, 1, lowerIntLoadNode);
         if (trace) traceMsg(comp, "Creating ibyteswapNode (%p) with child %p.\n", ibyteswapNode1, ibyteswapNode1->getFirstChild());

         if (trace) traceMsg(comp, "Setting lowerIntConversionNode (%p) child to %p.\n", lowerIntConversionNode, ibyteswapNode1);
         lowerIntConversionNode->setAndIncChild(0, ibyteswapNode1);
         lowerIntLoadNode->recursivelyDecReferenceCount();

         TR::Node * ibyteswapNode2 = TR::Node::create(TR::ibyteswap, 1, newLoadChildNode);
         if (trace) traceMsg(comp, "Creating ibyteswapNode (%p) with child %p.\n", ibyteswapNode2, ibyteswapNode2->getFirstChild());

         if (trace) traceMsg(comp, "Setting newConvertChildNode (%p) child to %p.\n", newConvertChildNode, ibyteswapNode2);
         newConvertChildNode->setAndIncChild(0, ibyteswapNode2);
         newLoadChildNode->recursivelyDecReferenceCount();
         }
      }
   else
      {
      TR_ASSERT_FATAL_WITH_NODE(rootNode, 0, "Unexpected code path during generateArraycopyFromSequentialLoads transformation. rootNode: %p.", rootNode);
      }

   disconnectedNode1->recursivelyDecReferenceCount();
   disconnectedNode2->recursivelyDecReferenceCount();

   TR::DebugCounter::prependDebugCounter(comp, TR::DebugCounter::debugCounterName(comp, "sequentialStoreSimplification/generateArraycopyFromSequentialLoads/%s", comp->signature()), currentTreeTop);

   return currentTreeTop;
   }

bool TR_arraysetSequentialStores::checkConstant(TR::Node* constExpr)
   {
   TR::DataType constType = constExpr->getDataType();
   bool isValidConst=false;
   uint8_t bv;

   if (!constExpr->getOpCode().isLoadConst())
      {
      return false;
      }

   switch (constType)
      {
      case TR::Int8:
         bv = constExpr->getByte();
         isValidConst = true;
         break;
      case TR::Int16:
         {
         uint32_t value = (uint32_t) constExpr->getShortInt();
         bv = (value & 0xFF);
         if (bv == ((value & 0xFF00) >> 8))
            { // most likely case is 0 or -1
            isValidConst = true;
            }
         break;
         }
      case TR::Int32:
      case TR::Float:
         {
         uint32_t value = (constType == TR::Float ? constExpr->getFloatBits() : constExpr->getInt());
         bv = (value & 0xFF);
         if (bv == ((value & 0xFF00) >> 8) && bv == ((value & 0xFF0000) >> 16) && bv == ((value & 0xFF000000) >> 24))
            { // most likely case is 0 or -1
            isValidConst = true;
            }
         break;
         }
      case TR::Address:
         {
         if (constExpr->getAddress() == 0)
            {
            isValidConst = true;
            bv = 0;
            }
         break;
         }

      case TR::Int64:
      case TR::Double:
         {
         uint32_t value = (uint32_t) constExpr->getLongIntHigh();
         bv = (value & 0xFF);
         if (bv == ((value & 0xFF00) >> 8) && bv == ((value & 0xFF0000) >> 16) && bv == ((value & 0xFF000000) >> 24))
            {
            uint32_t value = (uint32_t) constExpr->getLongIntLow();
            uint32_t bv2 = (value & 0xFF);
            if (bv == bv2 && (bv == ((value & 0xFF00) >> 8) && bv == ((value & 0xFF0000) >> 16) && bv == ((value & 0xFF000000) >> 24)))
               {
               // most likely case is 0 or -1
               isValidConst = true;
               }
            }
         break;
         }

      default:
         break;
      }

   //traceMsg(comp, "arrayset check const %d %d %d %d\n", isValidConst, getProcessedRefs(), _initValue, bv);
   if (isValidConst)
      {
      if (getProcessedRefs())
         {
         if (_initValue == bv)
            {
            return true;
            }
         }
      else
         {
         _initValue = bv;
         return true;
         }
      }
   return false;
   }



bool TR_arraysetSequentialStores::checkArrayStoreConstant(TR::Node* constExpr)
   {
   TR::DataType constType = constExpr->getDataType();
   bool isValidConst = true;
   int64_t bv = 0;

   if (!constExpr->getOpCode().isLoadConst())
      {
      return false;
      }

   switch (constType)
      {
      case TR::Int8:
         bv = constExpr->getByte();
         break;
      case TR::Int16:
         {
         bv = constExpr->getShortInt();
         break;
         }
      case TR::Int32:
         {
         bv = constExpr->getInt();
         break;
         }
      case TR::Float:
         {
         bv = constExpr->getFloatBits();
         if (bv != 0)
            isValidConst = false;
         break;
         }
      case TR::Address:
         {
         if (constExpr->getAddress() == 0)
            bv = 0;
         else
            {
            isValidConst = false;
            }
         break;
         }

      case TR::Int64:
         {
         bv = constExpr->getLongInt();
         break;
         }
      case TR::Double:
         {
         bv = constExpr->getLongInt();
         if (bv != 0)
            isValidConst = false;
         break;
         }

      default:
         break;
      }

   //traceMsg(comp, "arrayset check const %d %d %d %d\n", isValidConst, getProcessedRefs(), _initValue, bv);
   if (isValidConst)
      {
      if (getProcessedRefs())
         {
         if (_initValue == bv)
            {
            return true;
            }
         }
      else
         {
         _initValue = bv;
         return true;
         }
      }
   return false;
   }


static bool useArraySet(int32_t numBytes, TR::CodeGenerator *codeGen)
   {
   if (!codeGen->getSupportsArraySet())
      {
      //traceMsg(comp, "arrayset not enabled for this platform\n");
      return false;
      }
   if (numBytes < codeGen->arrayInitMinimumNumberOfBytes())
      {
      return false;
      }

   return true;
   }


// Transform a series of sequential stores into increasing storage into
// an arrayset call.
//   iT1store (where T1 is one of i, b, c, l, d, f) offset X
//     aload #base (base value)
//     T1const # (constant) where bytes of T1 const are the same
//   iT2store offset (T2 is one of i, b, c, l, d, f) offset X + sizeof(T1)
//     aload #base (baes value)
//     T2const # (constant) where bytes of T2 const are same as bytes of T1const
static TR::TreeTop* generateArraysetFromSequentialStores(TR::Compilation* comp, TR::TreeTop* prevTreeTop, TR::TreeTop* istoreTreeTop, TR::Node* istoreNode, bool *newTempsCreated)
   {
   TR::CodeGenerator* codeGen = comp->cg();

   TR_arraysetSequentialStores arrayset = TR_arraysetSequentialStores(comp);
   TR::Node* curNode = istoreNode;
   TR::TreeTop* curTreeTop = istoreTreeTop;
   bool areSequentialArrayElements = false;
   int totalNumOfStores=0;

   TR_ScratchList<TR::TreeTop> seqStores(comp->trMemory());
   while (arrayset.checkIStore(curNode) &&
          arrayset.checkStore(curNode) &&
          arrayset.checkALoad(curNode->getFirstChild()) &&
          arrayset.checkConstant(curNode->getSecondChild()))
      {
      arrayset.setProcessedRefs();
      arrayset.setLastOffset(arrayset.getActiveOffset());
      curTreeTop = curTreeTop->getNextTreeTop();
      curNode = curTreeTop->getNode();
      totalNumOfStores++;
      seqStores.add(curTreeTop->getPrevTreeTop());
      }

   int32_t numBytes = arrayset.getNumBytes();
   //traceMsg("orig numBytes = %d for node %p\n", numBytes, istoreNode);

   //TR_ScratchList<TR::TreeTop> seqStores(comp->trMemory());
   TR::Node *baseNode = NULL;
   if (!useArraySet(numBytes, codeGen))
      {
      seqStores.deleteAll();
      arrayset = TR_arraysetSequentialStores(comp);
      curNode = istoreNode;
      curTreeTop = istoreTreeTop;
      totalNumOfStores=0;

      // Combining the stores moves all but one of them up, and they can't be
      // moved up past loads of memory that might alias. As a very conservative
      // safety check, don't allow any other trees between the stores.
      //
      // The store trees themselves are constrained enough that they should
      // never evaluate such loads. The stored value is always constant. The
      // address calculation should only evaluate constants and a[il]add,
      // [il]add, [il]sub. Any other nodes (base array address, variable
      // starting offset) have to be commoned between all of the stores, i.e.
      // evaluated once before the first store.
      while (arrayset.checkIStore(curNode) &&
          (baseNode = arrayset.checkArrayStore(curNode, codeGen->getSupportsArraySet())) &&
          arrayset.checkALoad(baseNode) &&
          arrayset.checkArrayStoreConstant(curNode->getSecondChild()))
         {
         areSequentialArrayElements = true;
         arrayset.setProcessedRefs();
         arrayset.setLastOffset(arrayset.getActiveOffset());
         totalNumOfStores++;
         seqStores.add(curTreeTop);
         curTreeTop = curTreeTop->getNextTreeTop();
         curNode = curTreeTop->getNode();
         }
      }

   numBytes = arrayset.getNumBytes();

   bool usingArrayset = true;
   if (areSequentialArrayElements && (totalNumOfStores >= 1))
      {
      if (totalNumOfStores == 1)
         return istoreTreeTop;

      if (!codeGen->getSupportsArraySet())
         {
         if ((curNode->getSize() == 8) ||
             (numBytes == curNode->getSize()))
            return istoreTreeTop;

         TR_ASSERT((numBytes <= 8), "number of bytes is greater than 8 and we cannot use a single store to achieve this\n");
         usingArrayset = false;
         }
      else if (comp->target().cpu.isPower() && numBytes < 32) // tm - The threshold needs to be adjusted for PPC
         {
         //traceMsg(comp, "arrayset istore did not encounter large enough storage range. Range is: %d\n", numBytes);
         return istoreTreeTop;
         }
      else if ((numBytes < 8)  || ((numBytes < 12) && comp->target().is64Bit())) // msf - change to codeGen->arrayInitMinimumNumberOfBytes())
         {
         //traceMsg(comp, "arrayset istore did not encounter large enough storage range. Range is: %d\n", numBytes);
         return istoreTreeTop;
         }

      if (usingArrayset)
         {
         // The arrayset can only be used if the 'value' being stored is the same
         // in each byte of the 'value' (arrayset has a byte child). This is not checked by the sequential array store code
         // but it could be improved to allow arrayset to be used in this case as well.
         //
         return istoreTreeTop;
         }
      }
   else
      {
      if (!useArraySet(numBytes, codeGen))
         {
         //traceMsg(comp, "arrayset not enabled for this platform\n");
         return istoreTreeTop;
         }
      }

   if (!performTransformation(comp, "%sReducing arrayset sequential stores starting from n%un\n", OPT_DETAILS, istoreNode->getGlobalIndex()))
      return istoreTreeTop;

   //printf("Reduced arrayset in %s\n", comp->signature()); fflush(stdout);

   //traceMsg(comp, " First store in sequence %p Load Ref:%p Number of bytes: %d. Offset range:%d to %d. Byte Value:%d\n", istoreNode, arrayset.getALoadRef(), numBytes, arrayset.getBaseOffset(), arrayset.getBaseOffset() + numBytes - 1, arrayset.getConstant());

   //
   // break the istorei trees into tree_tops of the aload and const
   // so that it can be subsequently deleted by simplifier phase
   //
   TR_arraysetSequentialStores arraysetUpdate = TR_arraysetSequentialStores(comp);
   curNode = istoreNode;
   curTreeTop = istoreTreeTop;
   prevTreeTop = curTreeTop->getPrevTreeTop();
   int currNumOfStoresReduced=0;
   TR::TreeTop *restartTree = NULL;
   while (currNumOfStoresReduced<totalNumOfStores)
      {
      TR::TreeTop *nextTree = curTreeTop->getNextTreeTop();
      if (seqStores.find(curTreeTop))
         {
         if (currNumOfStoresReduced == 0)
            {
            istoreTreeTop = curTreeTop;
            istoreNode = curNode;
            }

         arraysetUpdate.setProcessedRefs();
         TR::Node *firstChildTreeNode = TR::Node::create(TR::treetop, 1, curNode->getFirstChild());
         TR::TreeTop *firstChildTreeTop = TR::TreeTop::create(comp, firstChildTreeNode);
         TR::Node *secondChildTreeNode = TR::Node::create(TR::treetop, 1, curNode->getSecondChild());
         TR::TreeTop *secondChildTreeTop = TR::TreeTop::create(comp, secondChildTreeNode);

         if (curNode->getFirstChild()->getReferenceCount() > 0)
            {
            curNode->getFirstChild()->decReferenceCount();
            }
         if (curNode->getSecondChild()->getReferenceCount() > 0)
            {
            curNode->getSecondChild()->decReferenceCount();
            }
         prevTreeTop->join(firstChildTreeTop);
         firstChildTreeTop->join(secondChildTreeTop);
         secondChildTreeTop->join(nextTree);
         prevTreeTop = secondChildTreeTop;
         currNumOfStoresReduced++;
         }
      else
         {
         if (!restartTree)
            restartTree = prevTreeTop;

         prevTreeTop = curTreeTop;
         }

      curTreeTop = nextTree;
      curNode = curTreeTop->getNode();
      }

   TR::TreeTop *arraysetTreeTop = NULL;
   if (usingArrayset)
      {
      // create the arrayinit tree
      // treetop
      //   arrayinit
      //     aiadd
      //       aload base#
      //       iconst offset
      //     ibconst initVal
      //     iconst length
      //TR::Node* aloadNode    = TR::Node::create(istoreNode, arrayset.getLoadOpCode(), 0, arrayset.getALoadRef());
      //

      TR::Node* aloadNode = arrayset.getALoad();
      TR::Node *offsetNode, *arrayRefNode;
      if (comp->target().is64Bit())
         {
         offsetNode = TR::Node::create(istoreNode, TR::lconst);
         offsetNode->setLongInt((int64_t)arrayset.getBaseOffset());
         arrayRefNode = TR::Node::create(TR::aladd, 2, aloadNode, offsetNode);
         }
      else
         {
         offsetNode = TR::Node::create(istoreNode, TR::iconst, 0, (int32_t)arrayset.getBaseOffset());
         arrayRefNode    = TR::Node::create(TR::aiadd, 2, aloadNode, offsetNode);
         }

      TR::Node* constValNode = TR::Node::bconst(istoreNode, (int8_t)arrayset.getConstant());
      TR::Node* numBytesNode = TR::Node::create(istoreNode, TR::iconst, 0, (int32_t)numBytes);

      TR::Node *arraysetNode = TR::Node::create(TR::arrayset, 3, arrayRefNode, constValNode, numBytesNode);

      TR::SymbolReference *arraySetSymRef = NULL;
         {
         arraySetSymRef = comp->getSymRefTab()->findOrCreateArraySetSymbol();
         }
      arraysetNode->setSymbolReference(arraySetSymRef);


      TR::Node *topNode = TR::Node::create(TR::treetop, 1, arraysetNode);
      arraysetTreeTop = TR::TreeTop::create(comp, topNode);

      // delete all the old istorei trees by eliminating them from the tree list
      prevTreeTop->join(arraysetTreeTop);
      arraysetTreeTop->join(curTreeTop);
      }
   else
      {
      TR_ASSERT((numBytes <= 8), "Number of bytes is more than expected\n");
      //
      // delete the bstorei trees and replace them with a new, improved iXstore tree
      //
      //dumpOptDetails(comp, " Remove trees %p to %p\n", istoreTreeTop->getNode(), curTreeTop->getNode());
      //TR::TreeTop::removeDeadTrees(comp, istoreTreeTop, curTreeTop);

      TR::Node* aiaddNode = istoreNode->getFirstChild();

      int32_t numBytesLeft = numBytes;
      int32_t curOffset = istoreNode->getSymbolReference()->getOffset();
      while (numBytesLeft > 0)
         {
         int32_t numBytesToBeStored;
         if (numBytesLeft == 8)
            {
            numBytesToBeStored = 8;
            numBytesLeft = 0;
            }
         else if (numBytesLeft >= 4)
            {
            numBytesToBeStored = 4;
            numBytesLeft = numBytesLeft - 4;
            }
         else if (numBytesLeft >= 2)
            {
            numBytesToBeStored = 2;
            numBytesLeft = numBytesLeft - 2;
            }
         else
            {
            numBytesToBeStored = 1;
            numBytesLeft = 0;
            }

         //traceMsg("num bytes left %d num bytes to be stored %d num of stores %d\n", numBytesLeft, numBytesToBeStored, totalNumOfStores);

        TR::Node* constValueNode;
         TR::ILOpCodes opcode;
         switch(numBytesToBeStored)
            {
            case 1:
               {
               opcode = TR::bstorei;
               constValueNode = TR::Node::bconst(istoreNode,(int8_t)arrayset.getConstant());
               break;
               }
            case 2:
               {
               opcode = TR::sstorei;
               int32_t constValue = (int32_t)arrayset.getConstant();
               if (istoreNode->getOpCodeValue() == TR::bstorei)
                  {
                  uint8_t byteConstValue = (uint8_t) constValue;
                  constValue = (int32_t) byteConstValue;
                  constValue = ((constValue << 8) | constValue);
                  }

               constValueNode = TR::Node::sconst(istoreNode, constValue);
               break;
               }
            case 4:
               {
               opcode = TR::istorei;
               int32_t constValue = (int32_t)arrayset.getConstant();
               if (istoreNode->getOpCodeValue() == TR::bstorei)
                  {
                  uint8_t byteConstValue = (uint8_t) constValue;
                  constValue = (int32_t) byteConstValue;
                  constValue = ((constValue << 24) | (constValue << 16) | (constValue << 8) | constValue);
                  }
               else if (istoreNode->getOpCodeValue() == TR::sstorei)
                  {
                  uint16_t shortConstValue = (uint16_t) constValue;
                  constValue = (int32_t) shortConstValue;
                  constValue = ((constValue << 16) | constValue);
                  }

               constValueNode = TR::Node::create(istoreNode, TR::iconst, 0, constValue);
               break;
               }
            case 8:
               {
               opcode = TR::lstorei;
               int32_t constValue = (int32_t)arrayset.getConstant();
               int64_t longConstValue = (int64_t) constValue;
               constValueNode = TR::Node::create(istoreNode, TR::lconst, 0, 0);

               if (istoreNode->getOpCodeValue() == TR::bstorei)
                  {
                  uint8_t byteConstValue = (uint8_t) longConstValue;
                  longConstValue = (int64_t) byteConstValue;
                  longConstValue = ((longConstValue << 56) | (longConstValue << 48) | (longConstValue << 40) | (longConstValue << 32) |
                                    (longConstValue << 24) | (longConstValue << 16) | (longConstValue << 8) | longConstValue);
                  }
               else if (istoreNode->getOpCodeValue() == TR::sstorei)
                  {
                  uint16_t shortConstValue = (uint16_t) longConstValue;
                  longConstValue = (int64_t) shortConstValue;
                  longConstValue = ((longConstValue << 48) | (longConstValue << 32) |
                                    (longConstValue << 16) | longConstValue);
                  }
               else if (istoreNode->getOpCodeValue() == TR::istorei)
                  {
                  uint32_t intConstValue = (uint32_t) longConstValue;
                  longConstValue = (int64_t) intConstValue;
                  longConstValue = ((longConstValue << 32) | longConstValue);
                  }

               constValueNode->setLongInt(longConstValue);
               break;
               }
            default: TR_ASSERT(0, " number of bytes unexpected\n"); break;
            }

         //printf("Did seq store in %s\n", comp->signature()); fflush(stdout);
         TR::SymbolReference * symRef = comp->getSymRefTab()->findOrCreateGenericIntShadowSymbolReference(curOffset);

         bool canHoistConstant = false;
         TR::Block *prevBlock = NULL;
         static char *disableHoist = feGetEnv("TR_disableHoist");
         if (!disableHoist &&
             comp->getJittedMethodSymbol() && // avoid NULL pointer on non-Wcode builds
             comp->cg()->isMaterialized(constValueNode))
            {
            TR::Block *block = prevTreeTop->getEnclosingBlock();
            if (block && block->getStructureOf() && comp->getFlowGraph()->getStructure())
               {
               TR_Structure *containingLoop = block->getStructureOf()->getContainingLoop();
               if (containingLoop &&
                   (containingLoop->getNumber() == block->getNumber()))
                  {
                  prevBlock = block->getPrevBlock();
                  if (prevBlock)
                     {
                     if (prevBlock->getStructureOf() &&
                         prevBlock->getStructureOf()->isLoopInvariantBlock() &&
                         (prevBlock->getSuccessors().size() == 1) &&
                         (prevBlock->getSuccessors().front()->getTo() == block))
                        {
                        canHoistConstant = true;
                        for (auto edge = block->getPredecessors().begin(); edge != block->getPredecessors().end(); ++edge)
                           {
                           TR::Block *pred = toBlock((*edge)->getFrom());

                           if (pred == prevBlock)
                              continue;

                           if (pred->getStructureOf()->getContainingLoop() != containingLoop)
                              {
                              canHoistConstant = false;
                              break;
                              }
                           }
                        }
                     }
                  }
               }
            }

         if (canHoistConstant)
            {
            TR::SymbolReference *constSymbolReference = comp->getSymRefTab()->createTemporary(comp->getMethodSymbol(), constValueNode->getDataType());
            TR::Node *constStore = TR::Node::createStore(constSymbolReference, constValueNode);
            TR::TreeTop *constStoreTree = TR::TreeTop::create(comp, constStore);
            TR::TreeTop *placeHolderTree = prevBlock->getLastRealTreeTop();
            if (!placeHolderTree->getNode()->getOpCode().isBranch())
               placeHolderTree = prevBlock->getExit();

            TR::TreeTop *treeBeforePlaceHolder = placeHolderTree->getPrevTreeTop();
            treeBeforePlaceHolder->join(constStoreTree);
            constStoreTree->join(placeHolderTree);
            *newTempsCreated = true;
            constValueNode = TR::Node::createLoad(constValueNode, constSymbolReference);
            }

         TR::Node* iXstoreNode = TR::Node::createWithSymRef(opcode, 2, 2, aiaddNode, constValueNode, symRef);
         TR::TreeTop* treeTop = TR::TreeTop::create(comp, iXstoreNode);
         arraysetTreeTop = treeTop;
         //TR::TreeTop* nextTreeTop = prevTreeTop->getNextTreeTop();

         TR::TreeTop *prevTree2 = istoreTreeTop->getPrevTreeTop();
         TR::TreeTop *nextTree2 = prevTree2->getNextTreeTop();
         prevTree2->join(treeTop);
         treeTop->join(nextTree2);

         prevTreeTop = treeTop;

         curOffset = curOffset + numBytesToBeStored;
         }
      }

   if (restartTree)
      return restartTree;
   return arraysetTreeTop;
   }

class TR_ArrayShiftTree
   {
   public:
      TR_ALLOC(TR_Memory::LoopTransformer)
      TR_ArrayShiftTree(TR::Compilation * c, TR::TreeTop * tt);
      TR::Compilation * comp() { return _comp; }
      TR::Node * getRootNode() { return _rootNode; }
      TR::Node * getSourceLoadNode() { return (_rootNode) ? _rootNode->getSecondChild() : NULL; }
      TR_AddressTree * getTargetAddress() { return _targetAddress; }
      TR_AddressTree * getSourceAddress() { return _sourceAddress; }
      TR::Node * getTargetIndexVarNode() { return _targetAddress->getIndexBase()->getParent(); }
      TR::Node * getSourceIndexVarNode() { return _sourceAddress->getIndexBase()->getParent(); }
      TR::Node * getTargetArrayNode() { return _targetAddress->getBaseVarNode()->getParent()->getFirstChild(); }
      TR::Node * getSourceArrayNode() { return _sourceAddress->getBaseVarNode()->getParent()->getFirstChild(); }
      TR::ILOpCode getTargetOpCode() { return _targetAddress->getRootNode()->getOpCode(); }
      TR::ILOpCode getSourceOpCode() { return _sourceAddress->getRootNode()->getOpCode(); }
      TR::ILOpCodes getStoreOpCode() { return _rootNode->getOpCodeValue(); }
      TR::ILOpCodes getLoadOpCode() { return _rootNode->getSecondChild()->getOpCodeValue(); }
      TR::TreeTop * getTreeTop() { return _treeTop; }
      bool process();
   private:
      TR::TreeTop * _treeTop;
      TR::Node * _rootNode;
      TR::Compilation * _comp;
      TR_AddressTree * _targetAddress;
      TR_AddressTree * _sourceAddress;
   };

TR_ArrayShiftTree::TR_ArrayShiftTree(TR::Compilation * c, TR::TreeTop * tt) : _comp(c), _treeTop(tt), _rootNode(tt->getNode())
   {
   _targetAddress = new ((TR_StackMemory) _comp->trMemory()) TR_AddressTree(stackAlloc, _comp);
   _sourceAddress = new ((TR_StackMemory) _comp->trMemory()) TR_AddressTree(stackAlloc, _comp);
   }

bool TR_ArrayShiftTree::process()
   {
   /* parse this form
    * #storei  <- _rootNode
    *   aload
    *   #loadi   <- sourceLoadNode
    *     aload
    *     offset
    */
   if (_rootNode->getNumChildren() == 2 && _rootNode->getOpCode().isStoreIndirect() && _rootNode->getSecondChild()->getOpCode().isLoadIndirect())
      {
      return _targetAddress->process(_rootNode->getFirstChild()) && _sourceAddress->process(_rootNode->getSecondChild()->getFirstChild());
      }
   return false;
   }

class TR_ArrayShiftTreeCollection
   {
   public:
      TR_ALLOC(TR_Memory::LoopTransformer)
      TR_ArrayShiftTreeCollection(TR::Compilation * c) : _useAliases(false), _comp(c), _numTrees(0) { memset(_storeTrees, 0, sizeof(_storeTrees)); }
      TR_ArrayShiftTree * getTree(int32_t i) { return _storeTrees[i]; }
      bool useAliasChecks() { return _useAliases; }
      int32_t getNumTrees() { return _numTrees; }
      void setNumTrees(int32_t t) { _numTrees = t; }
      TR::Compilation * comp() { return _comp; }
      bool insertTree(TR::TreeTop * currTree);
      void checkLoadStoreOrder();
      void sortStoreTrees();
   private:
      void swapTree(int32_t i, int32_t j);
      bool aliasCheck();
      // up to 8 trees in a row for a long being stored - add one extra to test for overflow
      enum { _maxStoreSize = 8 };
      TR_ArrayShiftTree * _storeTrees[_maxStoreSize];
      TR::Compilation * _comp;
      int32_t _numTrees;
      bool _useAliases;
   };

bool TR_ArrayShiftTreeCollection::aliasCheck()
   {
   // this performs the strong check with aliases
   // we are checking for whether we are reading/writing from the same array
   // if the source and the target arrays
   TR::SymbolReference * targetArraySymRef = _storeTrees[_numTrees]->getTargetArrayNode()->getSymbolReference();
   TR::SymbolReference * sourceArraySymRef = _storeTrees[_numTrees]->getSourceArrayNode()->getSymbolReference();
   if(targetArraySymRef && sourceArraySymRef)
      {
      // need to make sure the load does not alias to the previous stores
      if (targetArraySymRef->getSymbol() == _storeTrees[0]->getTargetArrayNode()->getSymbolReference()->getSymbol() &&
          sourceArraySymRef->getSymbol() == _storeTrees[0]->getSourceArrayNode()->getSymbolReference()->getSymbol())
         {
         int32_t loadSymRefNum = _storeTrees[_numTrees]->getRootNode()->getSecondChild()->getSymbolReference()->getReferenceNumber();
         TR::SymbolReference * loadDefSymRef = _storeTrees[_numTrees]->getRootNode()->getSecondChild()->getSymbolReference();
         for(int32_t i = 0; i < _numTrees; ++i)
            {

            TR::SymbolReference * symRef = _storeTrees[i]->getRootNode()->getSymbolReference();
            if (loadDefSymRef->getUseDefAliases().contains(symRef, comp()))
               {
               dumpOptDetails(comp(), "Store tree[%p] reading from alias written to previously\n", _storeTrees[_numTrees]->getRootNode());
               return false;
               }
            }
         return true;
         }
      }
   dumpOptDetails(comp(), "Store tree[%p] has no alias information\n", _storeTrees[_numTrees]->getRootNode());
   return false;
   }

// try to insert sequential store nodes into the collection
bool TR_ArrayShiftTreeCollection::insertTree(TR::TreeTop * currTree)
   {
   if (_numTrees >= _maxStoreSize)
      {
      return false;
      }
   _storeTrees[_numTrees] = new ((TR_StackMemory) _comp->trMemory()) TR_ArrayShiftTree(comp(), currTree);
   if (_storeTrees[_numTrees]->process())
      {
      if(_storeTrees[_numTrees]->getTargetIndexVarNode() == NULL || _storeTrees[_numTrees]->getTargetArrayNode()  == NULL)
         {
         dumpOptDetails(comp(), "Store tree [%p] has null index/array node\n", _storeTrees[_numTrees]->getRootNode());
         return false;
         }
      if (_storeTrees[_numTrees]->getTargetIndexVarNode() != _storeTrees[0]->getTargetIndexVarNode() ||
          _storeTrees[_numTrees]->getTargetArrayNode() != _storeTrees[0]->getTargetArrayNode())
         {
         dumpOptDetails(comp(), "Store tree[%p] with different index/array than first in sequence[%p]\n", _storeTrees[_numTrees]->getRootNode(), _storeTrees[0]->getRootNode());
         return false;
         }
      if (_storeTrees[_numTrees]->getTargetIndexVarNode() != _storeTrees[_numTrees]->getSourceIndexVarNode() ||
          _storeTrees[_numTrees]->getTargetArrayNode() != _storeTrees[_numTrees]->getSourceArrayNode())
         {
         dumpOptDetails(comp(), "Store tree[%p] with different index/array nodes\n", _storeTrees[_numTrees]->getRootNode());
         return false;
         }
      for(int32_t i = 0; i < _numTrees; ++i)
         {
         if (_storeTrees[i]->getTargetAddress()->getOffset() == _storeTrees[_numTrees]->getSourceAddress()->getOffset())
            {
            dumpOptDetails(comp(), "Store tree[%p] reading from offset written to previously\n", _storeTrees[_numTrees]->getRootNode());
            return false;
            }
         }
      if (_storeTrees[_numTrees]->getStoreOpCode() != _storeTrees[0]->getStoreOpCode() ||
          _storeTrees[_numTrees]->getLoadOpCode() != _storeTrees[0]->getLoadOpCode())
         {
         dumpOptDetails(comp(), "Store tree[%p] with different store/load opcode\n", _storeTrees[_numTrees]->getRootNode());
         return false;
         }
      if (_storeTrees[_numTrees]->getRootNode()->getOpCode().getSize() * (_numTrees + 1) > _maxStoreSize)
         {
         dumpOptDetails(comp(), "Max store size of %d exceeeded the max [%p]\n", (_storeTrees[_numTrees]->getRootNode()->getOpCode().getSize() * (_numTrees + 1)), _storeTrees[_numTrees]->getRootNode());
         return false;
         }

      /* If we have a load with refcount > 1. We need to check if symref is killed between
       * referenced loads in previous treetops. We need to reject below case since coalesced load creates
       * a new node and symref. todo: anchor new load before the first kill?

            BBStart
            ...
          ^ T_k (treetop that contains any source node) - stop and reject.
          | ...
          | istorei #1
          | ...
          L T_n  (start scanning from prev treetop of coalescing store)
            istorei ... (coalescing store)
            istorei ...
       */
      if (_storeTrees[_numTrees]->getSourceLoadNode() &&
          _storeTrees[_numTrees]->getSourceLoadNode()->getReferenceCount() > 1)
         {
         TR::Node *sourceLoad = _storeTrees[_numTrees]->getSourceLoadNode();
         TR::SparseBitVector loadAlias(comp()->allocator("SLSS bitvector"));
         loadAlias[sourceLoad->getSymbolReference()->getReferenceNumber()] = 1;
         TR::Node *firstEncounteredKillNode = NULL;
         TR::TreeTop *tmpTT = _storeTrees[0]->getTreeTop()->getPrevTreeTop();

         // Code that walks treetops back to root of extended BB
         while (!((tmpTT->getNode()->getOpCode().getOpCodeValue() == TR::BBStart) &&
                  !tmpTT->getNode()->getBlock()->isExtensionOfPreviousBlock()))
            {
            TR::Node * sideEffectNode = (tmpTT->getNode()->getOpCodeValue() == TR::treetop) ?
                                   tmpTT->getNode()->getFirstChild() : tmpTT->getNode();
            // Check if killed.
            if (!firstEncounteredKillNode &&
                sideEffectNode->mayKill().containsAny(loadAlias, comp()))
               {
               firstEncounteredKillNode = sideEffectNode;
               }
            // if we already found first kill and then found a reference of load, there is overlap; must reject.
            if (firstEncounteredKillNode &&
                tmpTT->getNode()->containsNode(sourceLoad, comp()->incVisitCount()))
               {
               dumpOptDetails(comp(), "Store tree[%p]: multi-ref load[%p] was killed by[%p]\n",
                     _storeTrees[_numTrees]->getRootNode(), _storeTrees[_numTrees]->getSourceLoadNode(), firstEncounteredKillNode);
               return false;
               }
            tmpTT = tmpTT->getPrevTreeTop();
            }
         }

      //dumpOptDetails(comp(), "Store tree[%p] added to collection\n", _storeTrees[_numTrees]->getRootNode());
      _numTrees++;
      return true;
      }
   return false;
   }

void TR_ArrayShiftTreeCollection::checkLoadStoreOrder()
   {
   // we are checking for consistent offsets of both the stores and the loads here
   // only supporting inorder stores now since all platforms have not implemented reverse stores
   int32_t expectedOffsetDelta = _storeTrees[0]->getRootNode()->getOpCode().getSize();
   int32_t baseTargetOffset = _storeTrees[0]->getTargetAddress()->getOffset();
   int32_t baseSourceOffset = _storeTrees[0]->getSourceAddress()->getOffset();
   for(int32_t i = 1; i < _numTrees; ++i)
      {
      if((baseTargetOffset != _storeTrees[i]->getTargetAddress()->getOffset() - (expectedOffsetDelta * i))||
         (baseSourceOffset != _storeTrees[i]->getSourceAddress()->getOffset() - (expectedOffsetDelta * i)) )
         {
         _numTrees = i;
         return;
         }
      }
   }

void TR_ArrayShiftTreeCollection::sortStoreTrees()
   {
   // we are sorting a max of 8 items, just do it the stupid way for now
   for(int32_t i = 0; i < _numTrees - 1; ++i)
      {
      int32_t minIndex = i;
      int32_t min = _storeTrees[i]->getTargetAddress()->getOffset();
      for(int32_t j = i + 1; j < _numTrees; ++j)
         {
         if(_storeTrees[j]->getTargetAddress()->getOffset() < min)
            {
            minIndex = j;
            min = _storeTrees[j]->getTargetAddress()->getOffset();
            }
         }
      swapTree(i, minIndex);
      }
   }

void TR_ArrayShiftTreeCollection::swapTree(int32_t i, int32_t j)
   {
   if (i == j) return;
   TR_ArrayShiftTree * temp = _storeTrees[i];
   _storeTrees[i] = _storeTrees[j];
   _storeTrees[j] = temp;
   }

bool mayKillInterferenceBetweenNodes(TR::Compilation * comp,
                                     TR::Node * argNode1,
                                     TR::Node * argNode2,
                                     vcount_t thisIterationVisitCount,
                                     vcount_t newVisitCount,
                                     bool trace);

static TR::TreeTop * reduceArrayLoad(TR_ArrayShiftTreeCollection * storeTrees, TR::Compilation * comp, TR::TreeTop * oldTreeTop)
   {
   // before any tree modification, check that each tree "movement" is valid.
   //
   //    t0
   //    ..
   //    t1
   //    ..
   //    ..
   //    t2
   //    ..
   //    t3
   //    ..
   //    ..
   //    t4
   //
   // each tN will be 'treetopped' and t0 will be widened to a larger store.
   // make sure that this is valid by testing the backwards 'movement' of tN to t0

   for (size_t i = storeTrees->getNumTrees() - 1; i >= 1; i--)
      {
      vcount_t vc1 = comp->incOrResetVisitCount();
      vcount_t vc2 = comp->incOrResetVisitCount();

      TR::TreeTop * ttMoveMe = storeTrees->getTree(i)->getTreeTop();
      TR::TreeTop * ttFrom = ttMoveMe->getPrevTreeTop();
      TR::TreeTop * ttTo = storeTrees->getTree(0)->getTreeTop();

      // widening t0 to a larger store means that ttMoveMe 'moves' after ttTo
      ttTo = ttTo->getNextTreeTop();

      size_t j = i - 1;

      while (true)
         {
         if (ttTo->getPrevTreeTop() == ttFrom)
            {
            break;
            }

         // do not compare with other trees in the collection, because the fineGrainedOverlap infrastructure
         // is not good enough to figure out that these trees do not collide.
         if (ttTo == storeTrees->getTree(j)->getTreeTop())
            {
            j = j - 1;
            }
         else
            {
            // is ttMoveMe killed by ttFrom?
            // is ttFrom killed by ttMoveMe?
            if (mayKillInterferenceBetweenNodes(comp,
                                                ttMoveMe->getNode(),
                                                ttFrom->getNode(),
                                                vc1,
                                                vc2,
                                                false)) // TODO: trace flag / function?
               {
               storeTrees->setNumTrees(i - 1);
               break;
               }
            }

         ttFrom = ttFrom->getPrevTreeTop();
         }
      }

   if (storeTrees->getNumTrees() > 1)
      {
      // at the point, we have collected all the consecutive trees in the pack and we know that we are not
      // reading from locations written to, so we can freely move the trees (to have incrementing order)
      storeTrees->sortStoreTrees();
      storeTrees->checkLoadStoreOrder();

      // everything checked out, we can now collect info needed to do the transformation
      TR::ILOpCodes storeOpCode, loadOpCode;
      int32_t numByteStore = storeTrees->getTree(0)->getRootNode()->getOpCode().getSize() * storeTrees->getNumTrees();
      int32_t numValidTrees = 0;
      TR::DataType newDataType = TR::NoType;
      if (numByteStore >= 8)
         {
         // if there were more than 8 or more byte's worth of data collected in the tree
         newDataType = TR::Int64;
         numValidTrees = 8 / storeTrees->getTree(0)->getRootNode()->getOpCode().getSize();
         // if we support reverse order load/stores, we will have a bit more logic here to pick the right operation
         storeOpCode = TR::lstorei;
         loadOpCode = TR::lloadi;
         }
      else if (numByteStore >= 4)
         {
         // between 4 and 7 bytes go into here, and we will transfrom the first 4 bytes
         newDataType = TR::Int32;
         numValidTrees = 4 / storeTrees->getTree(0)->getRootNode()->getOpCode().getSize();
         storeOpCode = TR::istorei;
         loadOpCode = TR::iloadi;
         }
      else if(numByteStore >= 2)
         {
         // between 2 and 3 bytes go into here, and we will transform the first 2 bytes
         newDataType = TR::Int16;
         numValidTrees = 2 / storeTrees->getTree(0)->getRootNode()->getOpCode().getSize();
         storeOpCode = TR::sstorei;
         loadOpCode = TR::sloadi;
         }

      if (numValidTrees < 2 ||
         !performTransformation(comp, "%sReducing array shift sequential stores\n", OPT_DETAILS))
         {
         return NULL;
         }

      TR::SymbolReference *symRef = storeTrees->getTree(0)->getRootNode()->getSymbolReference();

      TR::TreeTop * prevTreeTop = storeTrees->getTree(0)->getTreeTop()->getPrevTreeTop();

      TR::SparseBitVector fromAliases(comp->allocator("SCSS bitvector"));

      // delete the bstorei trees and replace them with a new, improved Xstorei tree
      for (int32_t i = 0; i < numValidTrees; i++)
         {
         TR::TreeTop * newTreeTop = storeTrees->getTree(i)->getTreeTop();
         TR::TreeTop * tempTreeTop = newTreeTop;
         newTreeTop = newTreeTop->getNextTreeTop();
         TR::TreeTop::removeDeadTrees(comp, tempTreeTop, newTreeTop);
         }

      // copy the old source node, and update the load operation
      TR::Node * sourceNode = TR::Node::copy(storeTrees->getTree(0)->getRootNode()->getSecondChild());
      TR::Node::recreate(sourceNode, loadOpCode);
      sourceNode->getFirstChild()->incReferenceCount();
      sourceNode->setReferenceCount(0);

      TR::Node * newStoreNode = TR::Node::createWithSymRef(storeOpCode, 2, 2, storeTrees->getTree(0)->getTargetAddress()->getRootNode(), sourceNode, symRef);
      TR::TreeTop* newTreeTop = TR::TreeTop::create(comp, newStoreNode);
      prevTreeTop->insertAfter(newTreeTop);

      dumpOptDetails(comp, "new store: %p, replaced %d trees\n", newStoreNode, numValidTrees);
      return newTreeTop;
      }
   return NULL;
   }

static bool hasSideEffect(TR::Node * n, TR::Compilation * c)
   {
   if (n->getOpCode().isStore())
      {
      // we don't deal with stores here.  So just return false
      return false;
      }

   return true;
   }

static bool isViolatingDirectStore(TR::Node * n, TR_ArrayShiftTreeCollection * st)
   {
   // if the store is to the same symbol reference is the same as that of the index variable
   return n->getOpCode().isStore() && n->getSymbolReference() == st->getTree(0)->getTargetIndexVarNode()->getSymbolReference();
   }

static bool isViolatingIndirectStore(TR::Node * n, TR_ArrayShiftTreeCollection * st, TR::Compilation * c)
   {
   if (n->getOpCode().isStoreIndirect())
      {
      TR_AddressTree tempAddrTree = TR_AddressTree(stackAlloc, c);
      bool validIndirectStore = tempAddrTree.process(n->getFirstChild());
      if ((validIndirectStore && tempAddrTree.getBaseVarNode()->getParent()->getFirstChild() == st->getTree(0)->getTargetArrayNode()) ||
          !validIndirectStore)
         {
         // if we can't parse the iXstore target tree or if the store is to the same array as the one we are trying to reduce
         // treat it as a violating store
         return true;
         }
      }
   return false;
   }

static TR::TreeTop* generateArrayshiftFromSequentialStores(TR::Compilation * comp, TR::TreeTop * prevTreeTop, TR::TreeTop * istoreTreeTop)
   {
   TR_ArrayShiftTreeCollection storeTrees = TR_ArrayShiftTreeCollection(comp);
   TR::CodeGenerator * cg = comp->cg();
   TR::TreeTop * currTreeTop = istoreTreeTop;
   TR::TreeTop * exitTreeTop = comp->getStartTree()->getExtendedBlockExitTreeTop();
   // try to insert consecutive store trees with the same op code/base array
   while (currTreeTop && currTreeTop != exitTreeTop)
      {
      if (!storeTrees.insertTree(currTreeTop))
         {
         // try to skip over the tree if it's a store
         TR::Node * node = currTreeTop->getNode();
         if ((storeTrees.getNumTrees() == 0) || hasSideEffect(node, comp) ||
             isViolatingDirectStore(node, &storeTrees) || isViolatingIndirectStore(node, &storeTrees, comp))
            {
            break;
            }
         }
      currTreeTop = currTreeTop->getNextTreeTop();
      }

   if(storeTrees.getNumTrees() > 1)
      {
      // if there are more than 1 tree in the collection
      // we try to reduce it to use a wider store operation
      TR::TreeTop * newTreeTop = reduceArrayLoad(&storeTrees, comp, istoreTreeTop);
      if (newTreeTop)
         {
         return newTreeTop;
         }
      }
   return istoreTreeTop;
   }

int32_t TR_SequentialStoreSimplifier::perform()
   {
   static bool forceSequentialStoreSimplifier = (feGetEnv("TR_ForceSequentialStoreSimplifier") != NULL);

   /*
    * SupportsAlignedAccessOnly is currently only set for Power 7 and earlier and also LE Power 8. It is not set for Power 9 and onward
    * and also not set for BE Power 8 and non-Power platforms.
    */
   if (!forceSequentialStoreSimplifier && comp()->cg()->getSupportsAlignedAccessOnly() && comp()->cg()->supportsInternalPointers())
      return 1;

   bool newTempsCreated = false;
   bool trace = comp()->trace(OMR::sequentialStoreSimplification);

   {
   TR::StackMemoryRegion stackMemoryRegion(*trMemory());

   TR::TreeTop* currentTree = comp()->getStartTree();
   TR::TreeTop* prevTree = NULL;

   vcount_t visitCount1 = comp()->incOrResetVisitCount();

   /*
    * Combine nodes are the "add" and "or" nodes that construct the longer value out of several loaded bytes.
    * combineNodeList keeps tracks of the nodes used to construct one particular value. It is populated inside the call to seqLoadSearchAndCombine.
    */
   NodeForwardList* combineNodeList = new (stackMemoryRegion) NodeForwardList(NodeForwardListAllocator(stackMemoryRegion));
   TR_BitVector* visitedNodes = new (stackMemoryRegion) TR_BitVector(comp()->getNodeCount(), trMemory(), stackAlloc, growable);

   while (currentTree)
      {
      TR::Node *firstNodeInTree = currentTree->getNode();

      if (firstNodeInTree->getOpCode().isStore() && firstNodeInTree->getOpCode().isIndirect())
         {
         currentTree = generateArraysetFromSequentialStores(comp(), prevTree, currentTree, firstNodeInTree, &newTempsCreated);
         currentTree = generateArraycopyFromSequentialStores(comp(), prevTree, currentTree, firstNodeInTree);
         currentTree = generateArrayshiftFromSequentialStores(comp(), prevTree, currentTree);
         }

      TR::Node *currentNode = firstNodeInTree;

      /* Set TR_UseOldSeqLoadOpt to use the original version of the opt that combines byte loads into a wider load. */
      static bool useOldSeqLoadOpt = (feGetEnv("TR_UseOldSeqLoadOpt") != NULL);
      if (!useOldSeqLoadOpt)
         {
         /*
          * seqLoadSearchAndCombine goes through the trees are tries to find a matching pattern for sequential byte loads
          * that can be replaced by wider loads.
          */
         currentTree = seqLoadSearchAndCombine(comp(), trace, visitedNodes, currentTree, currentNode, combineNodeList);
         }
      else
         {
         while ((currentNode->getNumChildren() >= 1) && (currentNode->getFirstChild()->getNumChildren() >= 1))
            {
            currentNode = currentNode->getFirstChild();
            if (currentNode->getOpCodeValue()==TR::bu2i && currentNode->getFirstChild()->getOpCode().isLoad() && currentNode->getFirstChild()->getOpCode().isIndirect())
               {
               currentTree = generateArraycopyFromSequentialLoadsDEPRECATED(comp(), currentTree, currentNode->getFirstChild());
               }
            }
         }

      prevTree = currentTree;
      currentTree = currentTree->getNextTreeTop();
      }

   } // scope of the stack memory region

   if (newTempsCreated)
      optimizer()->setAliasSetsAreValid(false);

   // should also add code to generate a new style of arrayinit for a series of sequential stores of
   // different values (the new style would have a read-only block of data to use for initialization
   // instead of a byte constant)
   //
   // derek brought up another good point - it would be good to:
   // a) organize the stores in ascending (memory) order in case
   //    the user didn't specify them in the same order as in the object
   // b) be able to handle relatively complex code in the middle of the
   //    stream in this sorting process.
   return 1;
   }

const char *
TR_SequentialStoreSimplifier::optDetailString() const throw()
   {
   return "O^O SEQUENTIAL STORE TRANSFORMATION: ";
   }

// ixstore
//   axadd
//      _baseAddress
//      xsub/xadd
//         ixload/xload/...
//         xconst
//   xconst/...
//
// TODO: Match against this?
//
// ixstore
//   axadd
//      _baseAddress
//      ixload/xload/...
//   xconst/...
//


/*
 * Seen in atmstac0.cbl after removing the reassociation opts from Simplifier:
 *
 * [0x2A0A5874]   bstorei #182[id=42:"WS-ATM-ACCT-ENTRY-USED-F"][0x29201614]  Shadow[<refined-array-shadow>] <intPrec=2>
 *                  ==>aiadd at [0x2A0A561C]
 * [0x2A0A58B4]     buconst -16 <intPrec=2>    <flags:"0x204" (X!=0 X<=0 )/>
 * [0x2A0A5A94]   astore #201[id=305:"Subscr-AddrTemp"][0x2A08552C]  Auto[<auto slot 84>]
 *                  ==>aiadd at [0x2A0A561C]
 * [0x2A0A5CAC]   ipdstore #184[id=44:"WS-ATM-ACCT-BRANCH"][0x2920168C]  Shadow[<refined-array-shadow>] <prec=5 (len=3)>
 * [0x2A0A5C2C]     aiadd   <flags:"0x8000" (internalPtr )/>
 *                    ==>aiadd at [0x2A0A561C]
 * [0x2A0A5C6C]       iconst 1 <intPrec=1>    <flags:"0x104" (X!=0 X>=0 )/>
 * [0x2A0A5AEC]     pdconst "+00000" @ offset 240 <prec=5 (len=3)> sign=known(clean/preferred/0xc)    <flags:"0x800302" (X==0 X>=0 X<=0 skipCopyOnLoad )/>
 * [0x2A0A5B74]       aiadd   <flags:"0x8000" (internalPtr )/>
 * [0x2A0A5B34]         loadaddr #35[0x292009B8]  Static[$CONSTANT_AREA]
 * [0x2A0A5BB4]         iconst 240 <intPrec=3>
 * [0x2A0A5E84]   astore #200[id=306:"Subscr-AddrTemp"][0x2A0854D0]  Auto[<auto slot 83>]
 *                  ==>aiadd at [0x2A0A561C]
 * [0x2A0A609C]   ipdstore #185[id=45:"WS-ATM-ACCT-NO"][0x292016C8]  Shadow[<refined-array-shadow>] <prec=5 (len=3)>
 * [0x2A0A601C]     aiadd   <flags:"0x8000" (internalPtr )/>
 *                    ==>aiadd at [0x2A0A561C]
 * [0x2A0A605C]       iconst 4 <intPrec=1>    <flags:"0x104" (X!=0 X>=0 )/>
 *                  ==>pdconst "+00000" @ offset 240 <prec=5 (len=3)> sign=known(clean/preferred/0xc)  at [0x2A0A5AEC]
 * [0x2A0A6274]   astore #199[id=307:"Subscr-AddrTemp"][0x2A085474]  Auto[<auto slot 82>]
 *                  ==>aiadd at [0x2A0A561C]
 * [0x2A0A6494]   ipdstore #186[id=49:"WS-ATM-ACCT-OVERDRAW-AMOUNT"][0x292017B8]  Shadow[<refined-array-shadow>] <prec=11 (len=6)>
 * [0x2A0A6414]     aiadd   <flags:"0x8000" (internalPtr )/>
 *                    ==>aiadd at [0x2A0A561C]
 * [0x2A0A6454]       iconst 10 <intPrec=2>    <flags:"0x104" (X!=0 X>=0 )/>
 * [0x2A0A62CC]     pdconst "+00000000000" @ offset 808 <prec=11 (len=6)> sign=known(clean/preferred/0xc)    <flags:"0x800302" (X==0 X>=0 X<=0 skipCopyOnLoad )/>
 * [0x2A0A635C]       aiadd   <flags:"0x8000" (internalPtr )/>
 *                      ==>loadaddr at [0x2A0A5B34]
 * [0x2A0A639C]         iconst 808 <intPrec=3>
 *
 * Form 1 is 0x2A0A5874. Form 2 is 0x2A0A5CAC.
 *
 */

// true if they do overlap, false if they do not
bool fineGrainedOverlap(TR::Compilation * comp, TR::Node * n1, TR::Node * n2)
   {
   if (!n1->getOpCode().isStore() || !n1->getOpCode().isIndirect() ||
       !n2->getOpCode().isStore() || !n2->getOpCode().isIndirect())
      {
      // Return true for everything other than indirect stores.
      return true;
      }

   if (n1->getOpCode().hasSymbolReference() &&
       n1->getSymbolReference() &&
       n2->getOpCode().hasSymbolReference() &&
       n1->getSymbolReference())
      {
      TR::Symbol * s1 = n1->getSymbol();
      TR::Symbol * s2 = n2->getSymbol();

      bool nodesShareSymbol = false;

         {
         nodesShareSymbol = (s1 == s2);
         }

      if (nodesShareSymbol)
         {
         uint32_t n1_len = n1->getSize();
         uint32_t n2_len = n2->getSize();

         return (TR_NoOverlap != comp->cg()->storageMayOverlap(n1, n1_len, n2, n2_len));
         }
      }

   return true;
   }


bool mayKillInterferenceBetweenNodes(TR::Compilation * comp,
                                     TR::Node * argNode1,
                                     TR::Node * argNode2,
                                     vcount_t thisIterationVisitCount,
                                     vcount_t newVisitCount,
                                     bool trace)
   {
   LexicalTimer lt("mayKillInterferenceBetweenNodes", comp->phaseTimer());
   TR::Node * n1 = 0;
   TR::Node * n2 = 0;

   // First question: does argNode1 kill argNode2?
   n1 = argNode1;
   n2 = argNode2;

   if (n1->getOpCodeValue() == TR::treetop) n1 = n1->getFirstChild();
   if (n2->getOpCodeValue() == TR::treetop) n2 = n2->getFirstChild();

   n1->resetVisitCounts(thisIterationVisitCount);

   if (trace)
      {
      comp->getDebug()->trace(" --- resetVisitCounts on %p done\n", n1);
      comp->getDebug()->trace(" --- does node %p get killed somewhere in the subtree of node %p?\n", n2, n1);
      }

   if (n1->referencesMayKillAliasInSubTree(n2, newVisitCount, comp))
      {
      if (trace)
         comp->getDebug()->trace(" ---- node %p is killed somewhere in the subtree of node %p\n", n2, n1);

      if (!fineGrainedOverlap(comp, n1, n2))
         {
         if (trace)
            comp->getDebug()->trace(" ----- n1 %p and n2 %p return false for fineGrainedOverlap\n", n1, n2);
         }
      else
         {
         return true;
         }
      }

   // Second question: does argNode2 kill argNode1?
   n2 = argNode1;
   n1 = argNode2;

   // Code below is a copy of the code above.

   if (n1->getOpCodeValue() == TR::treetop) n1 = n1->getFirstChild();
   if (n2->getOpCodeValue() == TR::treetop) n2 = n2->getFirstChild();

   n1->resetVisitCounts(thisIterationVisitCount);

   if (trace)
      {
      comp->getDebug()->trace(" --- resetVisitCounts on %p done\n", n1);
      comp->getDebug()->trace(" --- does node %p get killed somewhere in the subtree of node %p?\n", n2, n1);
      }

   if (n1->referencesMayKillAliasInSubTree(n2, newVisitCount, comp))
      {
      if (trace)
         comp->getDebug()->trace(" ---- node %p is killed somewhere in the subtree of node %p\n", n2, n1);

      if (!fineGrainedOverlap(comp, n1, n2))
         {
         if (trace)
            comp->getDebug()->trace(" ----- n1 %p and n2 %p return false for fineGrainedOverlap\n", n1, n2);
         }
      else
         {
         return true;
         }
      }

   return false;
   }


