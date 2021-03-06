/*****************************************************************************
 *  Copyright (C) 2008-2011 Massachusetts Institute of Technology            *
 *                                                                           *
 *  Permission is hereby granted, free of charge, to any person obtaining    *
 *  a copy of this software and associated documentation files (the          *
 *  "Software"), to deal in the Software without restriction, including      *
 *  without limitation the rights to use, copy, modify, merge, publish,      *
 *  distribute, sublicense, and/or sell copies of the Software, and to       *
 *  permit persons to whom the Software is furnished to do so, subject       *
 *  to the following conditions:                                             *
 *                                                                           *
 *  The above copyright notice and this permission notice shall be included  *
 *  in all copies or substantial portions of the Software.                   *
 *                                                                           *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY                *
 *  KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE               *
 *  WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND      *
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE   *
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION   *
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION    *
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE           *
 *                                                                           *
 *  This source code is part of the PetaBricks project:                      *
 *    http://projects.csail.mit.edu/petabricks/                              *
 *                                                                           *
 *****************************************************************************/
//#define FORCE_OPENCL
//#define OPENCL_LOGGING

//#define TRACE(x) std::cout << "Trace " << x << "\n"

#define TRACE JTRACE

#define MAX_BLOCK_SIZE 16
//#define BLOCK_SIZE_X 16
//#define BLOCK_SIZE_Y 16

#include "userrule.h"

#include "maximawrapper.h"
#include "rircompilerpass.h"
#include "scheduler.h"
#include "transform.h"
#include "syntheticrule.h"
#include "gpurule.h"

#include "common/jconvert.h"

#include <algorithm>
#include <stdlib.h>
#include <set>


petabricks::UserRule::UserRule(const RegionPtr& to, const RegionList& from, const MatrixDefList& through, const FormulaList& cond)
  : _from(from)
  , _through(through)
  , _conditions(cond)
{
  _flags.isReturnStyle = true;
  _to.push_back(to);
}

petabricks::UserRule::UserRule(const RegionList& to, const RegionList& from, const MatrixDefList& through, const FormulaList& cond)
  : _from(from)
  , _to(to)
  , _through(through)
  , _conditions(cond)
{
  _flags.isReturnStyle = false;
}

namespace{
  static const char* theOffsetVarStrs[] = {"x","y","z","d4","d5","d6","d7","d8","d9","d10"};
  std::string _getOffsetVarStr(int ruleid, int dim, const char* extra) {
    if(extra==NULL) extra="";
    JASSERT(dim>=0 && dim<(int)(sizeof(theOffsetVarStrs)/sizeof(char*)))(dim);
    std::string name = "_r" + jalib::XToString(ruleid) + "_" + theOffsetVarStrs[dim];
    return extra+name;
  }
}

petabricks::FormulaPtr petabricks::RuleInterface::getOffsetVar(int dim, const char* extra /*= NULL*/) const{
  return new FormulaVariable(_getOffsetVarStr(_id, dim, extra).c_str()); //cache me!
}

int petabricks::RuleInterface::offsetVarToDimension(const std::string& var, const char* extra /*=NULL*/) const
{
  SRCPOSSCOPE();
  for(size_t dim=0; dim<(sizeof(theOffsetVarStrs)/sizeof(char*)); ++dim){
    if(_getOffsetVarStr(_id, dim, extra)==var)
      return dim;
  }
  JASSERT(false)(var).Text("unknown variable name");
  return 0;
}


void petabricks::UserRule::setBody(const char* str, const jalib::SrcPos& p){
  JWARNING(_bodysrc=="")(_bodysrc)(p);
  _bodysrc=str;
  _bodysrc[_bodysrc.length()-1] = ' ';
  _bodysrcPos.tagPosition(p.clone());
}

void petabricks::UserRule::compileRuleBody(Transform& tx, RIRScope& parentScope){
  SRCPOSSCOPE();
  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());

  jalib::Map(&Region::validate, _from);
  jalib::Map(&Region::validate, _to);

  RIRScopePtr scope = parentScope.createChildLayer();
  for(RegionList::iterator i=_from.begin(); i!=_from.end(); ++i){
    (*i)->addArgToScope(scope);
  }
  for(RegionList::iterator i=_to.begin(); i!=_to.end(); ++i){
    (*i)->addArgToScope(scope);
  }
  DebugPrintPass    print;
  ExpansionPass     expand(tx, *this, scope);
  AnalysisPass      analysis(*this, tx.name(), scope);
#ifdef HAVE_OPENCL
  OpenClCleanupPass opencl(*this, scope);
  OpenClFunctionRejectPass openclfnreject(*this, scope);
  CollectLoadStorePass loadstore(*this, scope);
  GpuRenamePass gpurename;
  bool failgpu = false;
#endif
  RIRBlockCopyRef bodyir = RIRBlock::parse(_bodysrc, &_bodysrcPos);

  bodyir->accept(expand);
  bodyir->accept(analysis);

#ifdef HAVE_OPENCL
  bodyir->accept(loadstore);
  _loads = loadstore.loads();
  _stores = loadstore.stores();
#endif

#ifdef DEBUG
  std::cerr << "--------------------\nEXPANDED compileRuleBody:\n" << bodyir << std::endl;
  bodyir->accept(print);
  std::cerr << "--------------------\n";
#endif

  for(RuleFlavor::iterator i=RuleFlavor::begin(); i!=RuleFlavor::end(); ++i) {
    _bodyir[i] = bodyir;
    RuleFlavorSpecializePass pass(i);
    _bodyir[i]->accept(pass);
  }

#ifdef HAVE_OPENCL
#ifdef DEBUG
  /*std::cerr << "--------------------\nAFTER compileRuleBody:\n" << bodyir << std::endl;
    bodyir->accept(print);
    std::cerr << "--------------------\n";*/
  TRACE("ENABLE/DISABLE GPU RULE");
  std::cerr << "----------------------------------" << std::endl;
  this->print(std::cout);
  std::cerr << "----------------------------------" << std::endl;
#endif
  // prepareBuffers, for loop, collectGpuLocalMemoryData have to be in this order because the dependence on _loads
  prepareBuffers();
  //for( RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i ) {
  //if(_loads.find(*i) == _loads.end())
  //  (*i)->setBuffer(false);
  //}
  collectGpuLocalMemoryData();
  
  // int lastdim = dimensions() - 1;
  // std::string lastdim_string = jalib::XToString(lastdim);
  // for(RuleFlavor::iterator i=RuleFlavor::begin(); i!=RuleFlavor::end(); ++i) {
  //   if(i != RuleFlavor::OPENCL) {
  //     ManageCpuGpuMemPass pass(*this, scope, loadstore.loads(), loadstore.stores());
  //     _bodyir[i]->accept(pass);
  //     if(pass.state() == 2) {  
  // 	RegionSet& stores = loadstore.stores();
  // 	for(RegionSet::iterator region_it = stores.begin(); region_it != stores.end(); ++region_it) {
  // 	  if((*region_it)->getRegionType() == Region::REGION_ALL)
  // 	    _bodyir[i]->addStmt(RIRStmt::parse((*region_it)->name()+".storageInfo()->modifyOnCpu(0);", SRCPOS()));
  // 	  // else
  // 	  //   _bodyir[i]->addStmt(RIRStmt::parse((*region_it)->name()+".storageInfo()->modifyOnCpu(_iter_begin["+lastdim_string+"]);", SRCPOS()));
  // 	}
  //     }
  //     if(pass.addfront()) {
  // 	RegionSet& loads = loadstore.loads();
  // 	for(RegionSet::iterator region_it = loads.begin(); region_it != loads.end(); ++region_it) {
  // 	  switch(stencilType(*region_it)) {
  // 	  case 0:
  // 	    _bodyir[i]->addFrontStmt(RIRStmt::parse((*region_it)->name()+".useOnCpu(0);", SRCPOS()));
  // 	    break;
  // 	  default:
  // 	  // case 1:
  // 	  //   _bodyir[i]->addFrontStmt(RIRStmt::parse((*region_it)->name()+".useOnCpu(_iter_begin["+lastdim_string+"]);", SRCPOS()));
  // 	  //   break;
  // 	  // case 2:
  // 	  //   _bodyir[i]->addFrontStmt(RIRStmt::parse((*region_it)->name()+".useOnCpu(_iter_begin["+lastdim_string+"] + "+jalib::XToString(minCoordOffsets()[(*region_it)->matrix()->name()][lastdim])+");", SRCPOS()));
  // 	    break;
  // 	  }
  // 	}
  //     }
  //     // DebugPrintPass pdebug;
  //     // _bodyir[i]->accept(pdebug);
  //     // exit(1);
  //   }
  // }

  if(isOpenClRule() && getSelfDependency().isNone()){
    try {
      _bodyir[RuleFlavor::OPENCL] = bodyir;
      _bodyir[RuleFlavor::OPENCL]->accept(openclfnreject);
      _bodyir[RuleFlavor::OPENCL]->accept(opencl);
      _bodyir[RuleFlavor::OPENCL]->accept(gpurename);

      _bodyirLocalMem = bodyir;
      _bodyirLocalMem->accept(openclfnreject);
      opencl.setLocalMemoryData(_local, _id);
      _bodyirLocalMem->accept(opencl);
      _bodyirLocalMem->accept(gpurename);
      std::cerr << "--------------------\nAFTER compileRuleBody:\n" << bodyir << std::endl;
      bodyir->accept(print);
      std::cerr << "--------------------\n";

      if(!passBuildGpuProgram(tx)) {
	      std::cout << "(>) RULE REJECTED BY TryBuildGpuProgram: RULE " << id() << "\n";
        failgpu = true;
      }

    }
    catch( OpenClCleanupPass::NotValidSource e )
    {
      std::cout << "(>) RULE REJECTED BY OpenClCleanupPass: RULE " << id() << "\n";
      failgpu = true;
    }
    catch( OpenClFunctionRejectPass::NotValidSource e )
    {
      std::cout << "(>) RULE REJECTED BY OpenClFunctionRejectPass: RULE " << id() << "\n";
      failgpu = true;
    }

  }
	else if(isOpenClRule()) {
		std::cout << "(>) RULE REJECTED BY Self Dependency \n";
		failgpu = true;
	}
  else
  {
    std::cout << "(>) NO OPENCL SUPPORT FOR RULE " << id() << "\n";
    failgpu = true;
  }

  if( failgpu )
  {
    if(_gpuRule) _gpuRule->disableRule();
    _gpuRule = NULL;
    _bodyir[RuleFlavor::OPENCL] = NULL;
  }

#endif
}

#ifdef HAVE_OPENCL
void petabricks::UserRule::collectGpuLocalMemoryData() {

  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());
  unsigned int dim = iterdef.dimensions();

  //for( RegionSet::const_iterator it =_loads.begin( ); it != _loads.end( ); ++it )
  //for( RegionList::counst_iterator it = _from.begin(); it != _from.end(); ++it)
  for( RegionList::iterator it = _from.begin(); it != _from.end(); ++it)
  {
  

    std::cout << "collect = " << (*it)->name() << " " << (*it)->matrix()->name() << std::endl;
    if((*it)->dimensions() == dim) {
      SimpleRegionPtr region = _fromBoundingBox[(*it)->matrix()];
      bool local = true;
      FormulaList min, max;
      for(int i = 0; i < (int) dim; i++) {
	FormulaPtr min_diff, max_diff;
	if(region) {
	  min_diff = new FormulaSubtract(region->minCoord().at(i), getOffsetVar(i));
	  min_diff = MAXIMA.normalize(min_diff);
	  FreeVarsPtr vars = min_diff->getFreeVariables();
	  if(vars->size() > 0) {
          local = false;
          break;
	  }
	  
	  
	  max_diff = new FormulaSubtract(region->maxCoord().at(i), getOffsetVar(i));
	  max_diff = MAXIMA.normalize(max_diff);
	  vars = max_diff->getFreeVariables();
	  if(vars->size() > 0) {
	    local = false;
	    break;
	  }
	}
	else {
	  min_diff = FormulaInteger::zero();
	  max_diff = FormulaInteger::one();
	}

        min.push_back(min_diff);
        max.push_back(max_diff);

      }

      if(local) {
	//std::cout << "ADD collect = " << (*it)->name() << " " << (*it)->matrix()->name() << std::endl;
	std::string matrix = (*it)->matrix()->name();
	if(_minCoordOffsets.find(matrix) == _minCoordOffsets.end()) {
	  _minCoordOffsets[matrix] = min;
	}
	else {
	  FormulaList& oldmin = _minCoordOffsets[matrix];
	  FormulaList newmin;
	  FormulaList::iterator fml1, fml2;
	  for(fml1 = oldmin.begin(), fml2 = min.begin(); fml1 != oldmin.end(); ++fml1, ++fml2) {
	    if(MAXIMA.compare(*fml1, "<", *fml2))
	      newmin.push_back(*fml1);
	    else
	      newmin.push_back(*fml2);
	  }
	  _minCoordOffsets[matrix] = newmin;
	}

	if(_maxCoordOffsets.find(matrix) == _maxCoordOffsets.end()) {
	  _maxCoordOffsets[matrix] = max;
	}
	else {
	  FormulaList& oldmax = _maxCoordOffsets[matrix];
	  FormulaList newmax;
	  FormulaList::iterator fml1, fml2;
	  for(fml1 = oldmax.begin(), fml2 = max.begin(); fml1 != oldmax.end(); ++fml1, ++fml2) {
	    if(MAXIMA.compare(*fml1, ">", *fml2))
	      newmax.push_back(*fml1);
	    else
	      newmax.push_back(*fml2);
	  }
	  _maxCoordOffsets[matrix] = newmax;
	}
      }
    }
  }

  for(std::map<std::string, FormulaList>::iterator it = _minCoordOffsets.begin(); it != _minCoordOffsets.end(); ++it) {
    std::string matrix = it->first;
    FormulaList& min = _minCoordOffsets[matrix];
    if(min.size() != 2)
      continue;
    FormulaList& max = _maxCoordOffsets[matrix];
    FormulaList::iterator fml1, fml2;
    FormulaPtr p = FormulaInteger::one();
    for(fml1 = min.begin(), fml2 = max.begin(); fml1 != min.end(); ++fml1, ++fml2) {
      p = new FormulaMultiply(p, new FormulaSubtract(*fml2, *fml1));
    }
    if(MAXIMA.comparePessimistically(p, ">", FormulaInteger::one()) ) {
      std::cout << "LOCAL: " << matrix << std::endl;
      std::cout << "MIN: " << min << std::endl;
      std::cout << "MAX: " << max << std::endl;
      _local.insert(matrix);
    }
  }
  //exit(1);
}

bool petabricks::UserRule::passBuildGpuProgram(Transform& trans) {
  TrainingDeps* tmp = new TrainingDeps();
  CLCodeGenerator clcodegen(tmp);

  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());
  generateOpenCLKernel( trans, clcodegen, iterdef );

  cl_int err;
  std::string s = clcodegen.os().str();
  const char *clsrc = s.c_str();

  std::cout << clsrc << std::endl;
  cl_context ctx = OpenCLUtil::getContext( );
  cl_program clprog  = clCreateProgramWithSource( ctx, 1, (const char **)&clsrc, NULL, &err );
  if(err != CL_SUCCESS)
    return false;
  err = clBuildProgram( clprog, 0, NULL, NULL, NULL, NULL);
  return (err == CL_SUCCESS);
}
#endif

void petabricks::UserRule::print(std::ostream& os) const {
  SRCPOSSCOPE();
  _flags.print(os);
  os << "UserRule " << _id << " " << _label;
  if(!_from.empty()){
    os << "\nfrom(";  printStlList(os,_from.begin(),_from.end(), ", "); os << ")";
  }
  if(!_to.empty()){
    os << "\nto(";    printStlList(os,_to.begin(),_to.end(), ", "); os << ")";
  }

  for(MatrixToRegionMap::const_iterator i=_fromBoundingBox.begin(); i!=_fromBoundingBox.end(); ++i) {
    os << "\nfromBoundingBox " << i->first
       << " " << i->second;
  }

  if(!_dataDependencyVectorMap.empty()) {
    os << "\ndata dependency vector map: " << _dataDependencyVectorMap;
  }
  if(!_conditions.empty()){
    os << "\nwhere ";  printStlList(os,_conditions.begin(),_conditions.end(), ", ");
  }
  if(!_definitions.empty()){
    os << "\ndefinitions ";  printStlList(os,_definitions.begin(),_definitions.end(), ", ");
  }
  if(!_duplicateVars.empty()){
    os << "\nduplicateVars ";  printStlList(os,_duplicateVars.begin(),_duplicateVars.end(), ", ");
  }
  os << "\nisRecursive " << isRecursive();
  os << "\nisOpenCLRule " << isOpenClRule();
  os << "\napplicableregion " << _applicableRegion;
  os << "\ndepends: \n";
  for(MatrixDependencyMap::const_iterator i=_depends.begin(); i!=_depends.end(); ++i){
    os << "  " << i->first << ": " << i->second << "\n";
  }
  os << "provides: \n";
  for(MatrixDependencyMap::const_iterator i=_provides.begin(); i!=_provides.end(); ++i){
    os << "  " << i->first << ": " << i->second << "\n";
  }
  //os << "SRC = {" << _bodysrc << "}\n";
  os << "BodyIR= {" << _bodyir[RuleFlavor::WORKSTEALING] << "}\n";
}

namespace {// file local
  struct CmpRegionsByDimensions {
    bool operator() ( const petabricks::RegionPtr& a, const petabricks::RegionPtr& b ){
      return a->dimensions() > b->dimensions();
    }
  };
}

void petabricks::UserRule::initialize(Transform& trans) {
  SRCPOSSCOPE();
  MaximaWrapper::instance().pushContext();

  MatrixDefList extraFrom = trans.defaultVisibleInputs();
  for(MatrixDefList::const_iterator i=extraFrom.begin(); i!=extraFrom.end(); ++i){
    RegionPtr r = new Region((*i)->name().c_str(), FormulaList(), "all", FormulaList());
    r->setName((*i)->name().c_str());
    _from.push_back(r);
  }

  jalib::Map(&Region::initialize, trans, _from);
  jalib::Map(&Region::initialize, trans, _to);
  jalib::Map(&Region::assertNotInput,    _to);
  _conditions.normalize();

  std::sort(_to.begin(), _to.end(), CmpRegionsByDimensions());

  FormulaList centerEqs = _to.front()->calculateCenter();
  FreeVars vars = centerEqs.getFreeVariables();
  vars.eraseAll(trans.constants());

  for(size_t i=0; i<centerEqs.size(); ++i)
    centerEqs[i] = new FormulaEQ(getOffsetVar(i), centerEqs[i]);

  for( FreeVars::const_iterator i = vars.begin(); i!=vars.end(); ++i )
  {
    FormulaListPtr v = MaximaWrapper::instance().solve(centerEqs, *i);
    JASSERT(v->size()>0)(v)(*i).Text("failed to solve for i in v");
    _definitions.push_back( trimImpossible(v) );
  }

  _from.makeRelativeTo(_definitions);
  _to.makeRelativeTo(_definitions);
  _conditions.makeRelativeTo(_definitions);

  buildApplicableRegion(trans, _applicableRegion, true);

  buildFromBoundingBox();
  buildToBoundingBox();
  buildScratchBoundingBox();

  FormulaList condtmp;
  condtmp.swap(_conditions);
  //simplify simple where clauses
  for(FormulaList::iterator i=condtmp.begin(); i!=condtmp.end(); ++i){
    FreeVars fv;
    (*i)->getFreeVariables(fv);
    fv.eraseAll(trans.constants());
    FormulaPtr l,r;
    (*i)->explodeEquality(l,r);
    if(fv.size() == 1){
      //for a single var where we can just update the applicable region
      std::string var = *fv.begin();
      int dim =  offsetVarToDimension(var);
      FormulaPtr varf = new FormulaVariable(var);
      FormulaPtr criticalPoint = trimImpossible(MaximaWrapper::instance().solve(new FormulaEQ(l,r), var))->rhs();
      bool smaller, larger, eq;
      MAXIMA.pushContext();
      MAXIMA.assume(*i);
      smaller=MAXIMA.tryCompare(varf, "<", criticalPoint)!=MaximaWrapper::NO;
      eq=     MAXIMA.tryCompare(varf, "=", criticalPoint)!=MaximaWrapper::NO;
      larger= MAXIMA.tryCompare(varf, ">", criticalPoint)!=MaximaWrapper::NO;
      MAXIMA.popContext();
      JASSERT(smaller|eq|larger)(criticalPoint)(*i).Text("where clause is never true");

      if(!smaller){
        FormulaPtr& f = _applicableRegion->minCoord()[dim];
        if(eq)
          f=MAXIMA.max(f, criticalPoint);
        else
          f=MAXIMA.max(f, criticalPoint->plusOne());
      }
      if(!larger){
        FormulaPtr& f = _applicableRegion->maxCoord()[dim];
        if(eq)
          f=MAXIMA.min(f, criticalPoint->plusOne());
        else
          f=MAXIMA.min(f, criticalPoint);
      }

      JTRACE("where clause handled")(*i)(criticalPoint)(_applicableRegion)(smaller)(eq)(larger);
    }else{
      //test if we can statically prove it
      int rslt = MAXIMA.is((*i)->printAsAssumption());
      if(rslt!=MaximaWrapper::YES){
        //otherwise handle it dynamically
        _conditions.push_back(*i);
      }else{
        JTRACE("where clause statically eliminated")(*i);
      }
    }
  }

  addAssumptions();

  //fill dependencies
  for(RegionList::iterator i=_from.begin(); i!=_from.end(); ++i){
    (*i)->collectDependencies(trans, *this, _depends);
  }
  for(RegionList::iterator i=_to.begin(); i!=_to.end(); ++i){
    (*i)->collectDependencies(trans, *this, _provides);
  }

  computeDataDependencyVector();

  //expand through() clause
  /*
  for(MatrixDefList::const_iterator i=_through.begin(); i!=_through.end(); ++i){
    _bodysrc=(*i)->genericTypeName()+" "+(*i)->name()+" = "+(*i)->genericAllocateStr()+";\n"+_bodysrc;
  }
  */

  MaximaWrapper::instance().popContext();
}

/**
 * Compute the data dependency vector for two region as the difference of the
 * dimensions of the two regions
 */
petabricks::CoordinateFormula petabricks::UserRule::computeDDVAsDifference(const RegionPtr inputRegion,
                                                                           const RegionPtr outputRegion
                                                                          ) const {
  JASSERT(inputRegion->dimensions()==outputRegion->dimensions());

  CoordinateFormula& inputMinCoord = inputRegion->minCoord();
  CoordinateFormula& outputMinCoord = outputRegion->minCoord();
  size_t dimensions=inputRegion->dimensions();

  CoordinateFormulaPtr newDataDependencyVector = new CoordinateFormula();

  for(size_t i=0; i<dimensions; ++i) {
    FormulaPtr difference = new FormulaSubtract(inputMinCoord[i],
                                             outputMinCoord[i]);
    difference = MaximaWrapper::instance().normalize(difference);
    newDataDependencyVector->push_back(difference);
  }
  return newDataDependencyVector;
}

/**
 * Computes the distance between the given output region and all the input
 * regions coming from the same original matrix
 */
void petabricks::UserRule::computeDDVForGivenOutput(const RegionPtr outputRegion
                                                   ) {
  for(RegionList::const_iterator i=_from.begin(), e=_from.end(); i!=e; ++i ) {
    const RegionPtr inputRegion = *i;

    if(outputRegion->matrix()->name() != inputRegion->matrix()->name()) {
      continue;
    }

    CoordinateFormula ddv = computeDDVAsDifference(inputRegion,
                                                    outputRegion);

    MatrixDefPtr inputMatrixDef=inputRegion->matrix();
    DataDependencyVectorMap::value_type newElement(inputMatrixDef,ddv);
    _dataDependencyVectorMap.insert(newElement);
  }

}

/**
 * Computes the distance between input and output for each dimension
 * of each region that is used both as input and output
 */
void petabricks::UserRule::computeDataDependencyVector() {
  //Loop on outputs (_to) first, because they usually are less then inputs
  for(RegionList::const_iterator i=_to.begin(), e=_to.end(); i != e; ++i) {
    const RegionPtr outputRegion = *i;

    computeDDVForGivenOutput(outputRegion);

  }
}


void petabricks::UserRule::buildFromBoundingBox(){
  SRCPOSSCOPE();
  _fromBoundingBox.clear();
  _fromBoundingBoxNoOptional.clear();
  for(RegionList::iterator i=_from.begin(); i!=_from.end(); ++i){
    JTRACE("building from bb")(_fromBoundingBox[(*i)->matrix()])(*(*i));
    if(_fromBoundingBox[(*i)->matrix()]) {
      _fromBoundingBox[(*i)->matrix()] = _fromBoundingBox[(*i)->matrix()]->regionUnion(*i);
    }else{
      _fromBoundingBox[(*i)->matrix()] = new SimpleRegion(*(*i));
    }

    if (!(*i)->isOptional()) {
      if(_fromBoundingBoxNoOptional[(*i)->matrix()]) {
        _fromBoundingBoxNoOptional[(*i)->matrix()] = _fromBoundingBoxNoOptional[(*i)->matrix()]->regionUnion(*i);
      }else{
        _fromBoundingBoxNoOptional[(*i)->matrix()] = new SimpleRegion(*(*i));
      }
    } else {
#ifndef DISABLE_DISTRIBUTED
      // TODO: compute real bounding box before create subregion
      // Currently, we check for optional at access time.
      UNIMPLEMENTED();
#endif
    }
  }
}

void petabricks::UserRule::buildToBoundingBox(){
  SRCPOSSCOPE();
  _toBoundingBox.clear();
  for(RegionList::iterator i=_to.begin(); i!=_to.end(); ++i){
    JTRACE("building to bb")(_toBoundingBox[(*i)->matrix()])(*(*i));
    if(_toBoundingBox[(*i)->matrix()]) {
      _toBoundingBox[(*i)->matrix()] = _toBoundingBox[(*i)->matrix()]->regionUnion(*i);
    }else{
      _toBoundingBox[(*i)->matrix()] = new SimpleRegion(*(*i));
    }
  }
}

void petabricks::UserRule::buildScratchBoundingBox(){
  SRCPOSSCOPE();
  _scratchBoundingBox = _toBoundingBox;
  for(MatrixToRegionMap::const_iterator i=_fromBoundingBoxNoOptional.begin(); i!=_fromBoundingBoxNoOptional.end(); ++i) {
    if(_scratchBoundingBox[i->first]) {
      _scratchBoundingBox[i->first] = _scratchBoundingBox[i->first]->regionUnion(i->second);
    }else{
      _scratchBoundingBox[i->first] = i->second;
    }
  }
}

void petabricks::UserRule::buildApplicableRegion(Transform& trans, SimpleRegionPtr& ar, bool allowOptional){
  SRCPOSSCOPE();
  for(RegionList::iterator i=_to.begin(); i!=_to.end(); ++i){
    JASSERT(!(*i)->isOptional())((*i)->name())
      .Text("optional regions are not allowed in outputs");
    SimpleRegionPtr t = (*i)->getApplicableRegion(trans, *this, _definitions, true);
    ar = ar ? ar->intersect(t) : t;
  }
  for(RegionList::iterator i=_from.begin(); i!=_from.end(); ++i){
    if(allowOptional && (*i)->isOptional())
      continue;
    SimpleRegionPtr t = (*i)->getApplicableRegion(trans, *this, _definitions, false);
    ar = ar ? ar->intersect(t) : t;
  }
  JASSERT(ar);
}

void petabricks::UserRule::performExpansion(Transform& trans){
  SRCPOSSCOPE();
  if(isDuplicated()){
    JTRACE("expanding duplicates")(duplicateCount());
    JASSERT(getDuplicateNumber()==0)(getDuplicateNumber());
    size_t dc = duplicateCount();
    for(size_t i=1; i<dc; ++i){
      trans.addRule(new DuplicateExpansionRule(this, i));
    }
  }
 #ifdef HAVE_OPENCL
  if(!hasWhereClause() && getMaxOutputDimension() > 0){
    _gpuRule = new GpuRule( this );
    trans.addRule( _gpuRule );
  }
 #endif
}

void petabricks::UserRule::getApplicableRegionDescriptors(RuleDescriptorList& output,
                                                          const MatrixDefPtr& matrix,
                                                          int dimension,
                                                          const RulePtr& rule
                                                          ) {
  SRCPOSSCOPE();
  MatrixDependencyMap::const_iterator i = _provides.find(matrix);
  if(i!=_provides.end()){
    FormulaPtr beginPos = i->second->region()->minCoord()[dimension];
    FormulaPtr endPos = i->second->region()->maxCoord()[dimension];
    endPos = MaximaWrapper::instance().normalize(endPos);
    output.push_back(RuleDescriptor(RuleDescriptor::RULE_BEGIN, rule, matrix, beginPos));
    output.push_back(RuleDescriptor(RuleDescriptor::RULE_END,   rule, matrix, endPos));
  }
}

void petabricks::UserRule::generateMetadataCode(Transform& trans, CodeGenerator& o, RuleFlavor rf){

  // Create _scratch map.
  // Need to clear first (this is called more than once)
  _scratch.clear();

  if (rf == RuleFlavor::DISTRIBUTED) {
    for(MatrixToRegionMap::const_iterator i=_fromBoundingBoxNoOptional.begin(); i!=_fromBoundingBoxNoOptional.end(); ++i) {
      MatrixDefPtr matrix = i->first;
      MatrixDefPtr scratch = new MatrixDef(("scratch_"+matrix->name()).c_str(), matrix->getVersion(), matrix->getSize());
      // Don't initialize (don't need to modify version)
      scratch->addType(MatrixDef::T_FROM);
      _scratch[matrix->name()] = scratch;
    }

    for(MatrixToRegionMap::const_iterator i=_toBoundingBox.begin(); i!=_toBoundingBox.end(); ++i) {
      MatrixDefPtr matrix = i->first;
      MatrixDefPtr scratch = new MatrixDef(("scratch_"+matrix->name()).c_str(), matrix->getVersion(), matrix->getSize());
      // Don't initialize (don't need to modify version)
      scratch->addType(MatrixDef::T_TO);
      _scratch[matrix->name()] = scratch;
    }
  }

  std::string metadataClass = itertrampmetadataname(trans)+"_"+rf.str();
  o.beginClass(metadataClass, "jalib::JRefCounted");
  o.addMember("IndexT*", "begin", "");
  o.addMember("IndexT*", "end", "");

  // Scratch regions
  for(MatrixDefMap::const_iterator i=_scratch.begin(); i!=_scratch.end(); ++i){
    JASSERT((rf == RuleFlavor::DISTRIBUTED));
    if (!hasCellAccess(i->first)) {
      // don't make a scratch
      continue;
    }
    MatrixDefPtr matrix = i->second;
    bool isConst = (matrix->type() == MatrixDef::T_FROM);
    o.addMember(matrix->typeName(rf, isConst), matrix->name());

    if (matrix->type() == MatrixDef::T_TO && matrix->numDimensions() > 0) {
      o.addMember(matrix->typeName(rf), "remote_TO_" + matrix->name());
      o.addMember(matrix->typeName(rf), "local_TO_" + matrix->name());
    }
  }

  // destructor
  o.beginFunc("", "~" + metadataClass);
  o.write("free(begin);");
  o.write("free(end);");
  o.endFunc();

  o.endClass();
}

void petabricks::UserRule::generateDeclCode(Transform& trans, CodeGenerator& o, RuleFlavor rf){
  /* code generated here goes at the top level of the file before the instance class */
  SRCPOSSCOPE();
  if(rf == RuleFlavor::SEQUENTIAL) {
    generateDeclCodeSequential(trans, o);
    return;
  }
  if(rf == RuleFlavor::WORKSTEALING && !isRecursive()) {
    //don't generate workstealing code for leaf nodes
    return;
  }

  generateMetadataCode(trans, o, rf);

  std::string classname = implcodename(trans)+"_"+ rf.str();
  o.beginClass(classname, "petabricks::RuleInstance");

  std::vector<std::string> to_cells;
  std::vector<std::string> from_cells;

  for(RegionList::const_iterator i=_to.begin(); i!=_to.end(); ++i){
    if ((rf == RuleFlavor::DISTRIBUTED) &&
        ((*i)->getRegionType() == Region::REGION_CELL) &&
        _to.size() == 1) {
      // will read CellProxy to ElementT in before running the task
      o.addMember((*i)->genTypeStr(rf, false), "_cellproxy_" + (*i)->name());
      o.addMember("ElementT", (*i)->name(), "0");
      to_cells.push_back((*i)->name());
    } else {
      o.addMember((*i)->genTypeStr(rf, false), (*i)->name());
    }
  }

  for(RegionList::const_iterator i=_from.begin(); i!=_from.end(); ++i){
    o.comment("_from " + (*i)->name());
    if ((rf == RuleFlavor::DISTRIBUTED) &&
        ((*i)->getRegionType() == Region::REGION_CELL)) {
      // will read CellProxy to const ElementT in before running the task
      o.addMember((*i)->genTypeStr(rf, true), "_cellproxy_" + (*i)->name());
      o.addMember("const ElementT", (*i)->name(), "0");
      from_cells.push_back((*i)->name());
    } else {
      o.addMember((*i)->genTypeStr(rf, true), (*i)->name());
    }
  }
  for(ConfigItems::const_iterator i=trans.config().begin(); i!=trans.config().end(); ++i){
    if(i->shouldPass()){
      o.addMember("const "+i->passType(), i->name());
    }
  }
  for(ConfigItems::const_iterator i=_duplicateVars.begin(); i!=_duplicateVars.end(); ++i){
      o.addMember("const IndexT", i->name());
  }
  for(int i=0; i<dimensions(); ++i){
    o.addMember("const IndexT", getOffsetVar(i)->toString());
  }
  for(FormulaList::const_iterator i=_definitions.begin(); i!=_definitions.end(); ++i){
    o.addMember("const IndexT",(*i)->lhs()->toString(), (*i)->rhs()->toString());
  }

  o.addMember("DynamicTaskPtr", "_completion", "new NullDynamicTask()");

  if (rf == RuleFlavor::DISTRIBUTED) {
    o.addMember("DynamicTaskPtr", "_cleanUpTask", "new MethodCallTask<"+classname+", &"+classname+"::cleanUp>(this)");
  }

  o.define("PB_FLAVOR", rf.str());
  o.define("SPAWN", "PB_SPAWN");
  o.define("CALL",  "PB_SPAWN");
  o.define("SYNC",  "PB_SYNC");

  if (rf == RuleFlavor::DISTRIBUTED) {
    o.define("DEFAULT_RV", "cleanUpTaskTmp");
    o.define("RETURN", "PB_RETURN_DISTRIBUTED");
    o.define("RETURN_VOID", "PB_RETURN_VOID_DISTRIBUTED");
  } else {
    o.define("DEFAULT_RV", "_completion");
    o.define("RETURN", "PB_RETURN");
    o.define("RETURN_VOID", "PB_RETURN_VOID");
  }
  o.beginFunc("petabricks::DynamicTaskPtr", "runDynamic");

  if (rf == RuleFlavor::DISTRIBUTED) {
    o.comment("read cellproxy in TO to ElementT");
    for (unsigned int i = 0; i < to_cells.size(); i++) {
      o.write(to_cells[i] + " = _cellproxy_" + to_cells[i] + ";");
    }

    o.comment("read all cellproxy in FROM to ElementT");
    for (unsigned int i = 0; i < from_cells.size(); i++) {
      o.write("const_cast<ElementT&> (" + from_cells[i] + ") = _cellproxy_" + from_cells[i] + ";");
    }
  }

  RIRBlockCopyRef bodytmp = _bodyir[rf];
  o.beginUserCode(rf);

  // Expand through() (using())
  for(MatrixDefList::const_iterator i=_through.begin(); i!=_through.end(); ++i){
    (*i)->allocateTemporary(o, rf, false, false);
  }

  {
    LiftVardeclPass p3(trans,*this, o);
    bodytmp->accept(p3);
  }
  {
    DynamicBodyPrintPass dbpp(o);
    bodytmp->accept(dbpp);
  }
  o.write("RETURN_VOID;");
  o.endUserCode();
  o.endFunc();

  if (rf == RuleFlavor::DISTRIBUTED) {
    o.beginFunc("petabricks::DynamicTaskPtr", "cleanUp");

    // Cannot do this when the same cell can be pass to TO more than once
    // (ex: BitonicInner)
    if (to_cells.size() == 1) {
      o.comment("write _to back to cellproxy");
      for (unsigned int i = 0; i < to_cells.size(); i++) {
        o.write("_cellproxy_" + to_cells[i] + " = " + to_cells[i] + ";");
      }
    }
    o.write("return NULL;");
    o.endFunc();
  }

  o.undefineAll();

  o.endClass();
}

void petabricks::UserRule::generateDeclCodeSequential(Transform& trans, CodeGenerator& o) {
  RuleFlavor rf = RuleFlavor::SEQUENTIAL;

  /* code generated here goes at the top level of the file before the instance class */
  std::vector<std::string> args;
  for(RegionList::const_iterator i=_to.begin(); i!=_to.end(); ++i){
    args.push_back((*i)->generateSignatureCode(rf, false));
  }
  for(RegionList::const_iterator i=_from.begin(); i!=_from.end(); ++i){
    args.push_back((*i)->generateSignatureCode(rf, true));
  }
  for(ConfigItems::const_iterator i=trans.config().begin(); i!=trans.config().end(); ++i){
    if(i->shouldPass()){
      args.push_back("const "+i->passType()+" "+i->name());
    }
  }
  for(ConfigItems::const_iterator i=_duplicateVars.begin(); i!=_duplicateVars.end(); ++i){
    args.push_back("const IndexT "+i->name());
  }
  for(int i=0; i<dimensions(); ++i){
    args.push_back("const IndexT "+getOffsetVar(i)->toString());
  }

  //static version
  o.beginFunc("void", implcodename(trans)+TX_STATIC_POSTFIX, args);
  for(FormulaList::const_iterator i=_definitions.begin(); i!=_definitions.end(); ++i){
    o.addMember("const IndexT",(*i)->lhs()->toString(), (*i)->rhs()->toString());
  }
  o.define("SPAWN", "PB_STATIC_CALL");
  o.define("CALL",  "PB_STATIC_CALL");
  o.define("SYNC",  "PB_NOP");
  o.define("DEFAULT_RV",  "");
  o.define("RETURN", "PB_RETURN");
  o.define("RETURN_VOID", "PB_RETURN_VOID");
  o.beginUserCode(rf);

  // Expand through() (using())
  for(MatrixDefList::const_iterator i=_through.begin(); i!=_through.end(); ++i){
    (*i)->allocateTemporary(o, rf, false, false);
  }

  o.write(_bodyir[RuleFlavor::SEQUENTIAL]->toString());
  o.endUserCode();
  o.undefineAll();
  o.endFunc();
}

void petabricks::UserRule::prepareBuffers() {
  std::set<std::string> set;
  for( RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i ) {
    std::string matrix_name = (*i)->matrix( )->name( );
    std::set<std::string>::iterator matrix_it = set.find(matrix_name);
    if(matrix_it == set.end()) {
      // If matrix that region belongs to is not in the set, this region is in charge of handling gpu buffer
      set.insert(matrix_name);
      (*i)->setBuffer(true);
    }
    else {
      (*i)->setBuffer(false);
    }
  }
  
  //TODO: check this
  set.clear();
  for( RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i ) {
    std::string matrix_name = (*i)->matrix( )->name( );
    std::set<std::string>::iterator matrix_it = set.find(matrix_name);
    if(matrix_it == set.end()) {
      // If matrix that region belongs to is not in the set, this region is in charge of handling gpu buffer
      set.insert(matrix_name);
      (*i)->setBuffer(true);
    }
    else {
      (*i)->setBuffer(false);
    }
  }
}

void petabricks::UserRule::generateDeclCodeOpenCl(Transform& /*trans*/, CodeGenerator& /*o*/) {
  /* code generated here goes at the top level of the file before the instance class */
}

void petabricks::UserRule::generateTrampCode(Transform& trans, CodeGenerator& o, RuleFlavor flavor){
  SRCPOSSCOPE();

#ifdef HAVE_OPENCL
  if(RuleFlavor::WORKSTEALING_OPENCL == flavor) {
    generateMultiOpenCLTrampCodes(trans, o, flavor);
    return;
  }
  if(RuleFlavor::DISTRIBUTED_OPENCL == flavor) {
    o.comment("UserRule::generateTrampCode RuleFlavor::DISTRIBUTED_OPENCL");
    //TODO
    return;
  }
#endif

  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());
  std::vector<std::string> taskargs = iterdef.packedargs();
  std::vector<std::string> packedargs = iterdef.packedargs();
  std::vector<std::string> packedargnames = iterdef.packedargnames();

  taskargs.insert(taskargs.begin(), "const jalib::JRef<"+trans.instClassName()+"> transform");

  o.beginFunc("petabricks::DynamicTaskPtr", trampcodename(trans)+"_"+flavor.str(), packedargs);

  // o.trace(trampcodename(trans));

#ifdef HAVE_OPENCL
  int dim_int = iterdef.dimensions();
  std::string dim_string = jalib::XToString(dim_int);
  
  if(RuleFlavor::WORKSTEALING == flavor || RuleFlavor::DISTRIBUTED == flavor) {
    CodeGenerator helper = o.forkhelper();
    std::string taskclass;
    //std::string dim_string = jalib::XToString(region.dimensions() + region.removedDimensions());
    std::string methodname = trampcodename(trans)+"_"+flavor.str();
    
    helper.beginFunc("petabricks::DynamicTaskPtr", trampcodename(trans)+"_"+flavor.str()+"_wrap", packedargs);
    taskclass = "petabricks::SpatialMethodCallTask<CLASS"
      ", " + dim_string
      + ", &CLASS::" + methodname + "_useOnCpu"
      + ">";
    helper.write("DynamicTaskPtr pre = new "+taskclass+"(this, _iter_begin, _iter_end);");
    helper.write("pre->enqueue();");
    
    taskclass = "petabricks::SpatialMethodCallTask<CLASS"
      ", " + dim_string
      + ", &CLASS::" + methodname
      + ">";
    helper.write("DynamicTaskPtr main = new "+taskclass+"(this, _iter_begin, _iter_end);");
    helper.write("main->dependsOn(pre);");
    helper.write("main->enqueue();");
    
    taskclass = "petabricks::SpatialMethodCallTask<CLASS"
      ", " + dim_string
      + ", &CLASS::" + methodname + "_modifyOnCpu"
      + ">";
    helper.write("DynamicTaskPtr post = new "+taskclass+"(this, _iter_begin, _iter_end);");
    helper.write("post->dependsOn(main);");
    helper.write("return post;");
    helper.endFunc();
    
    helper.beginFunc("petabricks::DynamicTaskPtr", trampcodename(trans)+"_"+flavor.str()+"_useOnCpu", packedargs);
    generateUseOnCpu(helper);
    helper.write("return NULL;");
    helper.endFunc();
    
    
    helper.beginFunc("petabricks::DynamicTaskPtr", trampcodename(trans)+"_"+flavor.str()+"_modifyOnCpu", packedargs);
    generateModifyOnCpu(helper);
    helper.write("return NULL;");
    helper.endFunc();
  }
  else {
    generateUseOnCpu(o);
  }
#endif

  for(size_t i=0; i<_duplicateVars.size(); ++i){
    o.varDecl("const IndexT "+_duplicateVars[i].name() + " = " + jalib::XToString(_duplicateVars[i].initial()));
  }

#ifdef HAVE_OPENCL
  if(RuleFlavor::SEQUENTIAL_OPENCL == flavor)
  {
    o.write(trampcodename(trans)+TX_STATIC_POSTFIX + "(_iter_begin, _iter_end);");
    o.write("return NULL;");
    o.endFunc();
    return;
  } else {
#else
  if(true) {
#endif

    if(RuleFlavor::DISTRIBUTED != flavor) {
      std::string outputDimensionCheck;
      for( RegionList::const_iterator i = _to.begin(); i != _to.end(); ++i) {
        if(i != _to.begin()) {
          outputDimensionCheck = outputDimensionCheck + " && ";
        }
        outputDimensionCheck = outputDimensionCheck + (*i)->matrix()->name() + ".bytes() == 0";
      }

      o.beginIf(outputDimensionCheck);
      o.write("return NULL;");
      o.endIf();
    }

    iterdef.unpackargs(o);

    if(isSingleElement()){
      trans.markSplitSizeUse(o);
      Heuristic blockNumberHeur = HeuristicManager::instance().getHeuristic("UserRule_blockNumber");
      blockNumberHeur.setMin(2);
      unsigned int blockNumber = blockNumberHeur.eval(ValueMap()); /**< The number of blocks the loop will be
                                                                    * splitted into */
      // JTRACE("LOOP BLOCKING")(blockNumber);

      if (flavor == RuleFlavor::DISTRIBUTED) {
        o.beginIf("petabricks::split_condition<"+jalib::XToString(dimensions())+", "+jalib::XToString(blockNumber)+">(distributedcutoff,"COORD_BEGIN_STR","COORD_END_STR")");

        // Can distribute apply task across nodes
        iterdef.genSplitCode(o, trans, *this, flavor, blockNumber, SpatialCallTypes::DISTRIBUTED);
      }

      if (flavor == RuleFlavor::DISTRIBUTED) {
        if (isRecursive()) {
          std::string splitCondition = "petabricks::split_condition<"+jalib::XToString(dimensions())+", "+jalib::XToString(blockNumber)+">("SPLIT_CHUNK_SIZE_DISTRIBUTED","COORD_BEGIN_STR","COORD_END_STR")";
          o.elseIf(splitCondition);

          // Can spawn distributed tasks
          iterdef.genSplitCode(o, trans, *this, flavor, blockNumber, SpatialCallTypes::NORMAL);
        }

      } else {
        std::string splitCondition = "petabricks::split_condition<"+jalib::XToString(dimensions())+", "+jalib::XToString(blockNumber)+">("SPLIT_CHUNK_SIZE","COORD_BEGIN_STR","COORD_END_STR")";
        o.beginIf(splitCondition);
        iterdef.genSplitCode(o, trans, *this, flavor, blockNumber, SpatialCallTypes::NORMAL);
      }

      // return written in get split code
      o.elseIf();
    }

    if(!((RuleFlavor::WORKSTEALING == flavor ) && !isRecursive())
       && flavor != RuleFlavor::SEQUENTIAL) {
      o.write("DynamicTaskPtr _task;");
    }

    if (flavor == RuleFlavor::DISTRIBUTED) {
      CoordinateFormula startCoord;

      for (int i = 0; i < iterdef.dimensions(); ++i){
        FormulaPtr b = iterdef.begin()[i];
        FormulaPtr e = iterdef.end()[i];

        DependencyDirection& order = iterdef.order();
        bool iterateForward = (order.canIterateForward(i) || !order.canIterateBackward(i));

        if (iterateForward) {
          startCoord.push_back(b);

        } else {
          startCoord.push_back(MaximaWrapper::instance().normalize(new FormulaSubtract(e, FormulaInteger::one())));

        }

        o.beginIf(b->toString()+"<"+e->toString());
      }


      if (shouldGenerateTrampIterCode(flavor)) {
        // Create iteration tramp task
        std::string metadataclass = itertrampmetadataname(trans)+"_"+flavor.str();

        generateToLocalRegionCode(trans, o, flavor, iterdef, false, true, false);

        o.mkIterationTrampTask("_task", trans.instClassName(), itertrampcodename(trans)+"_"+flavor.str(), metadataclass, "metadata", startCoord);

      } else if (isSingleCall()) {
        iterdef.genLoopBegin(o);
        generateTrampCellCodeSimple( trans, o, flavor );
        iterdef.genLoopEnd(o);

      } else if (!isRecursive()) {
        o.comment("Call apply_ruleX_partial");

        std::string methodname = partialtrampcodename(trans)+"_workstealing";
        std::string metadataname = partialtrampmetadataname(trans)+"_workstealing";
        std::string taskclass =
          "petabricks::SpatialMethodCallTask_partial<"
          + metadataname + ", "
          + jalib::XToString(iterdef.dimensions())
          + ", &::" + methodname + ">";

        o.write("jalib::JRef<"+metadataname+"> metadata = "+metadataname+"_generator(_iter_begin, _iter_end);");
        o.write("DynamicTaskPtr applyPartialTask = new "+taskclass+"(_iter_begin, _iter_end, metadata);");

        // clean up: copy back to remote region
        methodname = partialtrampcodename(trans)+"_cleanup_workstealing";
        taskclass =
          "petabricks::SpatialMethodCallTask_partial<"
          + metadataname + ", "
          + jalib::XToString(iterdef.dimensions())
          + ", &::" + methodname + ">";
        o.write("_task = new "+taskclass+"(_iter_begin, _iter_end, metadata);");
        o.write("_task->dependsOn(applyPartialTask);");
        o.write("applyPartialTask->enqueue();");

      } else {
        JASSERT(false).Text("Check shouldGenerateTrampIterCode method");

      }

      for (int i = 0; i < iterdef.dimensions(); ++i){
        o.endIf();
      }

    } else {
      // non-distributed
      if (shouldGenerateTrampIterCode(flavor)) {
        // TODO: we don't need new regions for metadata in this case
        CoordinateFormula startCoord;

        for (int i = 0; i < iterdef.dimensions(); ++i){
          FormulaPtr b = iterdef.begin()[i];
          FormulaPtr e = iterdef.end()[i];

          DependencyDirection& order = iterdef.order();
          bool iterateForward = (order.canIterateForward(i) || !order.canIterateBackward(i));

          if (iterateForward) {
            startCoord.push_back(b);

          } else {
            startCoord.push_back(MaximaWrapper::instance().normalize(new FormulaSubtract(e, FormulaInteger::one())));

          }

          o.beginIf(b->toString()+"<"+e->toString());
        }

        generateToLocalRegionCode(trans, o, flavor, iterdef, false, true, false);

        std::string metadataclass = itertrampmetadataname(trans)+"_"+flavor.str();
        o.mkIterationTrampTask("_task", trans.instClassName(), itertrampcodename(trans)+"_"+flavor.str(), metadataclass, "metadata", startCoord);

        for (int i = 0; i < iterdef.dimensions(); ++i){
          o.endIf();
        }

      } else {
        iterdef.genLoopBegin(o);
        generateTrampCellCodeSimple( trans, o, flavor );
        iterdef.genLoopEnd(o);
      }
    }

#ifdef HAVE_OPENCL
    generateModifyOnCpu(o);
#endif

    if(!((RuleFlavor::WORKSTEALING == flavor ) && !isRecursive())
       && flavor != RuleFlavor::SEQUENTIAL) {
      o.write("return _task;");
    } else {
      o.write("return NULL;");
    }

    if(isSingleElement())
      o.endIf();
  }
  o.endFunc();

  std::string metadataclass = itertrampmetadataname(trans)+"_"+flavor.str();
  if (shouldGenerateTrampIterCode(flavor)) {
    IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());
    std::vector<std::string> args;
    args.push_back("jalib::JRef<" + metadataclass + "> metadata");
    args.push_back("IndexT coord["+jalib::XToString(iterdef.dimensions())+"]");

    o.beginFunc("petabricks::DynamicTaskPtr", itertrampcodename(trans)+"_"+flavor.str(), args);


    for(int i=0; i<iterdef.dimensions(); ++i) {
      o.write("IndexT " + getOffsetVar(i)->toString() + " = coord[" +jalib::XToString(i)+"];");
      FormulaPtr b = iterdef.begin()[i];
      FormulaPtr e = iterdef.end()[i];
      o.write("const IndexT " + b->toString() + " = metadata->begin[" + jalib::XToString(i) + "];");
      o.write("const IndexT " + e->toString() + " = metadata->end[" + jalib::XToString(i) + "];");
    }

    o.write("DynamicTaskPtr _task;");

    if (flavor == RuleFlavor::DISTRIBUTED && hasCellAccess()) {
      for(MatrixDefMap::const_iterator i=_scratch.begin(); i!=_scratch.end(); ++i){
        MatrixDefPtr matrix = i->second;
        if (hasCellAccess(i->first)) {
          bool isConst = (matrix->type() == MatrixDef::T_FROM);
          o.write(matrix->typeName(flavor, isConst)+"& "+matrix->name()+" = metadata->"+matrix->name()+";");
        }
      }

      generateTrampCellCodeSimple(trans, o, RuleFlavor::DISTRIBUTED_SCRATCH);
    } else {
      generateTrampCellCodeSimple(trans, o, flavor);
    }

    // Compute next coordinate
    o.comment("Compute next coordinate");
    o.write("bool hasNext = true;");
    for (int i = iterdef.dimensions() - 1; i >= 0; --i){
      FormulaPtr b = iterdef.begin()[i];
      FormulaPtr e = iterdef.end()[i];
      FormulaPtr s = iterdef.step()[i];
      FormulaPtr v = iterdef.var()[i];

      //TODO: expand to reorder dimensions
      DependencyDirection& order = iterdef.order();
      bool iterateForward = (order.canIterateForward(i) || !order.canIterateBackward(i));

      if (iterateForward) {
        JWARNING(order.canIterateForward(i))(order).Text("couldn't find valid iteration order, assuming forward");
        o.write(v->toString() + " = " + MaximaWrapper::instance().normalize(new FormulaAdd(v, s))->toString() + ";");
        o.beginIf(v->toString() + " >= " + e->toString() + "");
        o.write(v->toString() + " = " + b->toString() + ";");

      } else {
        o.write(v->toString() + " = " + MaximaWrapper::instance().normalize(new FormulaSubtract(v, s))->toString() + ";");
        o.beginIf(v->toString() + " < " + b->toString() + "");
        o.write(v->toString() + " = " + e->toString() + " - 1;");

      }
    }

    o.write("hasNext = false;");
    for (int i = 0; i < iterdef.dimensions(); ++i) {
      o.endIf();
    }

    o.write("DynamicTaskPtr _next;");
    o.beginIf("hasNext");
    o.mkIterationTrampTask("_next", trans.instClassName(), itertrampcodename(trans)+"_"+flavor.str(), metadataclass, "metadata", iterdef.var());

    o.elseIf();
    if (flavor == RuleFlavor::DISTRIBUTED && hasCellAccess()) {
      // Need clean up
      o.mkIterationTrampTask("_next", trans.instClassName(), itertrampcodename(trans)+"_clean_up_"+flavor.str(), metadataclass, "metadata", iterdef.var());

    } else {
      o.write("return _task;");
    }
    o.endIf();

    o.write("_next->dependsOn(_task);");
    o.write("_task->enqueue();");
    o.write("return _next;");
    o.endFunc();

    if (flavor == RuleFlavor::DISTRIBUTED && hasCellAccess()) {
      // Clean up iteration tramp
      o.beginFunc("petabricks::DynamicTaskPtr", itertrampcodename(trans)+"_clean_up_"+flavor.str(), args);
      o.comment("Copy from scratch");

      for(MatrixDefMap::const_iterator i=_scratch.begin(); i!=_scratch.end(); ++i){
        if (hasCellAccess(i->first)) {
          MatrixDefPtr matrix = i->second;
          if (matrix->type() == MatrixDef::T_TO) {
            if (matrix->numDimensions() > 0) {
              o.write("metadata->remote_TO_" + matrix->name() + ".fromScratchRegion(metadata->local_TO_" + matrix->name() + ");");
              // Debug
              // o.write("MatrixIOGeneral().write(metadata->remote_TO_" + matrix->name()+ ");");
              // o.write("MatrixIOGeneral().write(metadata->local_TO_" + matrix->name()+ ");");
            } else {
              o.write(i->first + ".writeCell(NULL, metadata->" + matrix->name() + ".cell());");
            }
          }
        }
      }

      o.write("return NULL;");
      o.endFunc();
    }
  }

  if (flavor == RuleFlavor::DISTRIBUTED) {
    // Generate getDataHosts for rule
    std::vector<std::string> args;
    args.push_back("DataHostPidList& list");
    args.push_back("IndexT _iter_begin["+jalib::XToString(iterdef.dimensions())+"]");
    args.push_back("IndexT _iter_end["+jalib::XToString(iterdef.dimensions())+"]");

    o.beginFunc("void", trampcodename(trans)+"_"+flavor.str()+"_getDataHosts", args);
    o.comment("getDataHosts for SpatialMethodCallTask");
    iterdef.unpackargs(o);

    CoordinateFormula iterdefEndInclusive = iterdef.end();
    iterdefEndInclusive.subToEach(FormulaInteger::one());
    for(MatrixDefMap::const_iterator i=_scratch.begin(); i!=_scratch.end(); ++i){
      MatrixDefPtr matrix = i->second;
      SimpleRegionPtr region = _scratchBoundingBox[trans.lookupMatrix(i->first)];

      CoordinateFormulaPtr lowerBounds = region->getIterationLowerBounds(iterdef.var(), iterdef.begin(), iterdefEndInclusive);
      CoordinateFormulaPtr upperBounds = region->getIterationUpperBounds(iterdef.var(), iterdef.begin(), iterdefEndInclusive);

      o.write("{");
      o.incIndent();
      o.write("IndexT _tmp_begin[] = {" + lowerBounds->toString() + "};");
      o.write("IndexT _tmp_end[] = {" + upperBounds->toString() + "};");
      o.write(i->first + ".dataHosts(list, _tmp_begin, _tmp_end);");
      o.decIndent();
      o.write("}");
    }

    o.endFunc();

    // Generate apply_ruleX_partial_metadata_workstealing_generator
    args.clear();
    args.push_back("IndexT _iter_begin["+jalib::XToString(iterdef.dimensions())+"]");
    args.push_back("IndexT _iter_end["+jalib::XToString(iterdef.dimensions())+"]");

    std::string metadataname = partialtrampmetadataname(trans)+"_workstealing";
    o.beginFunc("jalib::JRef<"+metadataname+">", metadataname+"_generator", args);

    // duplicate vars
    for(size_t i=0; i<_duplicateVars.size(); ++i){
      o.varDecl("const IndexT "+_duplicateVars[i].name() + " = " + jalib::XToString(_duplicateVars[i].initial()));
    }

    iterdef.unpackargs(o);

    o.write("jalib::JRef<"+metadataname+"> metadata = new " + metadataname + "();");

    generateToLocalRegionCode(trans, o, flavor, iterdef, true, false, true);

    for(ConfigItems::const_iterator i=trans.config().begin(); i!=trans.config().end(); ++i){
      if(i->shouldPass()) {
        o.write("metadata->" + i->name() + " = " + i->name() + ";");
      }
    }
    for(ConfigItems::const_iterator i=_duplicateVars.begin(); i!=_duplicateVars.end(); ++i){
      o.write("metadata->" + i->name() + " = " + i->name() + ";");
    }

    o.write("return metadata;");
    o.endFunc();
  }
}

void petabricks::UserRule::generatePartialTrampCode(Transform& trans, CodeGenerator& o, RuleFlavor flavor){
  if (!shouldGeneratePartialTrampCode(flavor)) {
    return;
  }

  JASSERT(flavor == RuleFlavor::WORKSTEALING);

  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());

  std::string metadataClass = partialtrampmetadataname(trans)+"_"+flavor.str();
  o.beginClass(metadataClass, "jalib::JRefCounted");

  _partial.clear();
  _partialCoordOffsets.clear();

  for(MatrixToRegionMap::const_iterator i=_fromBoundingBoxNoOptional.begin(); i!=_fromBoundingBoxNoOptional.end(); ++i) {
    MatrixDefPtr matrix = i->first;
    MatrixDefPtr partial = new MatrixDef(("partial_"+matrix->name()).c_str(), matrix->getVersion(), matrix->getSize());
    partial->addType(MatrixDef::T_FROM);
    _partial[matrix->name()] = partial;
  }

  for(MatrixToRegionMap::const_iterator i=_toBoundingBox.begin(); i!=_toBoundingBox.end(); ++i) {
    MatrixDefPtr matrix = i->first;
    MatrixDefPtr partial = new MatrixDef(("partial_"+matrix->name()).c_str(), matrix->getVersion(), matrix->getSize());
    // Don't initialize (don't need to modify version)
    partial->addType(MatrixDef::T_TO);
    _partial[matrix->name()] = partial;
  }

  // Add members to metadata obj
  for(MatrixDefMap::const_iterator i=_partial.begin(); i!=_partial.end(); ++i){
    MatrixDefPtr matrix = i->second;
    bool isConst = (matrix->type() == MatrixDef::T_FROM);
    o.addMember(matrix->typeName(RuleFlavor::WORKSTEALING, isConst), i->first, "");
    if (matrix->type() == MatrixDef::T_TO) {
      o.addMember(matrix->typeName(RuleFlavor::DISTRIBUTED, isConst), "remote_TO_"+i->first, "");
      if (matrix->numDimensions() > 0) {
        o.addMember(matrix->typeName(RuleFlavor::DISTRIBUTED, isConst), "local_TO_"+i->first+"", "");
      }
    }
    o.addMember("IndexT", i->first+"_offset["+jalib::XToString(matrix->numDimensions())+"]", "");

    CoordinateFormulaPtr offset = new CoordinateFormula();
    for (unsigned int j = 0; j < matrix->numDimensions(); ++j) {
      offset->push_back(new FormulaVariable("metadata->" + i->first + "_offset[" + jalib::XToString(j) + "]"));
    }
    _partialCoordOffsets[i->first] = offset;
  }

  for(ConfigItems::const_iterator i=trans.config().begin(); i!=trans.config().end(); ++i){
    if(i->shouldPass()) {
      o.addMember(i->memberType(), i->name(), "");
    }
  }
  for(ConfigItems::const_iterator i=_duplicateVars.begin(); i!=_duplicateVars.end(); ++i){
    o.addMember("IndexT", i->name(), "");
  }

  o.endClass();

  // apply_ruleX_partial_cleanup
  std::vector<std::string> cleanupArgs;
  cleanupArgs.push_back("IndexT*");
  cleanupArgs.push_back("IndexT*");
  cleanupArgs.push_back("const jalib::JRef<" + metadataClass + ">& metadata");

  o.beginFunc("DynamicTaskPtr", partialtrampcodename(trans)+"_cleanup_"+ flavor.str(), cleanupArgs);

  for(MatrixDefMap::const_iterator i=_partial.begin(); i!=_partial.end(); ++i){
    MatrixDefPtr matrix = i->second;
    if (matrix->type() == MatrixDef::T_TO) {
      if (matrix->numDimensions() > 0) {
        o.write("metadata->remote_TO_" + i->first + ".fromScratchRegion(metadata->local_TO_" + i->first + ");");
        // o.write("MatrixIOGeneral().write(metadata->local_TO_" + i->first + ");");
      } else {
        o.write("metadata->remote_TO_" + i->first + ".writeCell(NULL, metadata->" + i->first + ".cell());");
        // o.write("MatrixIOGeneral().write(metadata->remote_TO_" + i->first + ");");
      }
    }
  }

  o.write("return NULL;");
  o.endFunc();

  // apply_ruleX_partial

  std::vector<std::string> args = iterdef.packedargs();
  args.push_back("const jalib::JRef<" + metadataClass + ">& metadata");

  o.beginFunc("DynamicTaskPtr", partialtrampcodename(trans)+"_"+ flavor.str(), args);

  iterdef.unpackargs(o);

  if(isSingleElement()){
    trans.markSplitSizeUse(o);
    Heuristic blockNumberHeur = HeuristicManager::instance().getHeuristic("UserRule_blockNumber");
    blockNumberHeur.setMin(2);
    unsigned int blockNumber = blockNumberHeur.eval(ValueMap()); /**< The number of blocks the loop will be
                                                                  * splitted into */
    // JTRACE("LOOP BLOCKING")(blockNumber);

    std::string splitCondition = "petabricks::split_condition<"+jalib::XToString(dimensions())+", "+jalib::XToString(blockNumber)+">("SPLIT_CHUNK_SIZE","COORD_BEGIN_STR","COORD_END_STR")";
    o.beginIf(splitCondition);

    iterdef.genSplitCode(o, trans, *this, flavor, blockNumber, SpatialCallTypes::WORKSTEALING_PARTIAL);
    // return written in get split code
    o.elseIf();
  }

  o.comment("Call sequential code");

  if(!((RuleFlavor::WORKSTEALING == flavor ) && !isRecursive())
     && flavor != RuleFlavor::SEQUENTIAL) {
    o.write("DynamicTaskPtr _task;");
  }

  if (shouldGenerateTrampIterCode(flavor)) {
    o.comment("call iter tramp");
    o.write("UNIMPLEMENTED();");

    /*
    // TODO: we don't need new regions for metadata in this case
    CoordinateFormula startCoord;

    for (int i = 0; i < iterdef.dimensions(); ++i){
      FormulaPtr b = iterdef.begin()[i];
      FormulaPtr e = iterdef.end()[i];

      DependencyDirection& order = iterdef.order();
      bool iterateForward = (order.canIterateForward(i) || !order.canIterateBackward(i));

      if (iterateForward) {
        startCoord.push_back(b);

      } else {
        startCoord.push_back(MaximaWrapper::instance().normalize(new FormulaSubtract(e, FormulaInteger::one())));

      }

      o.beginIf(b->toString()+"<"+e->toString());
    }

    generateToLocalRegionCode(trans, o, flavor, iterdef, true, false);

    std::string metadataclass = itertrampmetadataname(trans)+"_"+flavor.str();
    o.mkIterationTrampTask("_task", trans.instClassName(), itertrampcodename(trans)+"_"+flavor.str(), metadataclass, "metadata", startCoord);

    for (int i = 0; i < iterdef.dimensions(); ++i){
      o.endIf();
    }
    */

  } else {
    // Copy variables from metadata obj
    for(MatrixDefMap::const_iterator i=_partial.begin(); i!=_partial.end(); ++i){
      MatrixDefPtr matrix = i->second;
      bool isConst = (matrix->type() == MatrixDef::T_FROM);
      o.write(matrix->typeName(RuleFlavor::WORKSTEALING, isConst) + "& " + i->first + " = metadata->" + i->first + ";");
    }
    for(ConfigItems::const_iterator i=trans.config().begin(); i!=trans.config().end(); ++i){
      if(i->shouldPass()) {
        o.write("const " + i->passType() + " " + i->name() + " = metadata->" + i->name() + ";");
      }
    }
    for(ConfigItems::const_iterator i=_duplicateVars.begin(); i!=_duplicateVars.end(); ++i){
      o.write("IndexT " + i->name() + " = metadata->" + i->name() + ";");
    }

    iterdef.genLoopBegin(o);
    generateTrampCellCodeSimple( trans, o, RuleFlavor::WORKSTEALING_PARTIAL );
    iterdef.genLoopEnd(o);
  }

#ifdef HAVE_OPENCL
  for(RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i) {
    if((*i)->isBuffer())
      o.write((*i)->matrix()->name()+".storageInfo()->modifyOnCpu();");
  }
#endif

  if(!((RuleFlavor::WORKSTEALING == flavor ) && !isRecursive())
     && flavor != RuleFlavor::SEQUENTIAL) {
    o.write("return _task;");
  } else {
    o.write("return NULL;");
  }

  if(isSingleElement()) {
    o.endIf();
  }
  o.endFunc();

}

bool petabricks::UserRule::shouldGenerateTrampIterCode(RuleFlavor::RuleFlavorEnum flavor) {
  return (((flavor == RuleFlavor::DISTRIBUTED && (isRecursive() || !hasCellAccess())) ||
      (flavor == RuleFlavor::WORKSTEALING && isRecursive())) &&
     !isSingleCall());
}

bool petabricks::UserRule::shouldGeneratePartialTrampCode(RuleFlavor::RuleFlavorEnum flavor) {
#ifndef DISABLE_DISTRIBUTED
  return flavor == RuleFlavor::WORKSTEALING;
#else
  return false;
#endif
}

#ifdef HAVE_OPENCL
void petabricks::UserRule::generateUseOnCpu(CodeGenerator& o){
  int lastdim_int = dimensions()-1;
  std::string lastdim = jalib::XToString(lastdim_int);
  for(RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i) {
    if(!(*i)->isBuffer() || _loads.find(*i) == _loads.end())
      continue;
    //for(RegionSet::const_iterator i = _loads.begin( ); i != _loads.end( ); ++i) {
    //if(!(*i)->isBuffer())
    //continue;
    switch(stencilType(*i, true)) {
    case 0:
      o.write((*i)->matrix()->name()+".useOnCpu(0);");
      break;
    case 1:
      o.write((*i)->matrix()->name()+".useOnCpu(_iter_begin["+lastdim+"]);");
      break;
    case 2:
      o.write((*i)->matrix()->name()+".useOnCpu(_iter_begin["+lastdim+"] + "+jalib::XToString(_minCoordOffsets[(*i)->matrix()->name()][lastdim_int])+");");
      break;
    }
  } 
  for(RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i) {
    if(!(*i)->isBuffer() || _loads.find(*i) == _loads.end())
      continue;
    //for(RegionSet::const_iterator i = _loads.begin( ); i != _loads.end( ); ++i) {
    //if(!(*i)->isBuffer())
    //continue;
    switch(stencilType(*i, true)) {
    case 0:
      o.write((*i)->matrix()->name()+".useOnCpu(0);");
      break;
    case 1:
      o.write((*i)->matrix()->name()+".useOnCpu(_iter_begin["+lastdim+"]);");
      break;
    case 2:
      o.write((*i)->matrix()->name()+".useOnCpu(_iter_begin["+lastdim+"] + "+jalib::XToString(_minCoordOffsets[(*i)->matrix()->name()][lastdim_int])+");");
      break;
    }
  } 
}

void petabricks::UserRule::generateModifyOnCpu(CodeGenerator& o){
  std::string lastdim = jalib::XToString(dimensions()-1);
  for(RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i) {
    if(!(*i)->isBuffer() || _stores.find(*i) == _stores.end())
      continue;
    if((*i)->getRegionType() == Region::REGION_ALL) {
      o.write((*i)->matrix()->name()+".storageInfo()->modifyOnCpu(0);");
    }
    else {
      //if((*i)->getRegionType() != Region::REGION_ALL) {
      //o.write("std::cout << \"*** modify "+(*i)->matrix()->name()+": \" << _iter_begin["+lastdim+"]<< std::endl;");
      o.write((*i)->matrix()->name()+".modifyOnCpu(_iter_begin["+lastdim+"]);");
    }
  }
}

void petabricks::UserRule::generateMultiOpenCLTrampCodes(Transform& trans, CodeGenerator& o, RuleFlavor flavor){
  SRCPOSSCOPE();
  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());
  std::vector<std::string> packedargs = iterdef.packedargs();
  std::string codename = trampcodename(trans) + TX_OPENCL_POSTFIX;
  
  o.comment("UserRule:generateMultiOpenCLTrampCodes");

  // Call prepare, copy-in, run ,copy-out tasks
  generateOpenCLCallCode(trans, o, flavor);
  // Prepare task
  generateOpenCLPrepareCode(trans, codename,o);

  // Copy-in tasks (one per IN region)
  for( RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i ) {
    if((*i)->isBuffer() && _loads.find(*i) != _loads.end())
      generateOpenCLCopyInCode(codename,packedargs,o,*i);
  }

  for( RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i ) {
    if((*i)->isBuffer() && _loads.find(*i) != _loads.end())
      generateOpenCLCopyInCode(codename,packedargs,o,*i);
  }

  //for( RegionSet::const_iterator i = _loads.begin( ); i != _loads.end( ); ++i ) {
  //if((*i)->isBuffer())
  //  generateOpenCLCopyInCode(codename,packedargs,o,*i);
  //}

  // Run task
  generateOpenCLRunCode(trans, o);

  // Copy-out tasks (one per OUT region)
  for( RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i ) {
    if((*i)->isBuffer())
      generateOpenCLCopyOutCode(codename,o,*i);
  }
}

void petabricks::UserRule::generateOpenCLCallCode(Transform& trans,  CodeGenerator& o, RuleFlavor flavor){
  SRCPOSSCOPE();
  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());
  std::vector<std::string> packedargs = iterdef.packedargs();
  packedargs.push_back("int nodeID");
  packedargs.push_back("RegionNodeGroupMapPtr map");
  packedargs.push_back("int gpuCopyOut");
  std::string codename = trampcodename(trans) + TX_OPENCL_POSTFIX;
  std::string objectname;
  if(flavor == RuleFlavor::SEQUENTIAL_OPENCL)
    objectname = trans.instClassName() + "_sequential";
  else
    objectname = trans.instClassName() + "_workstealing";

  int dim_int = iterdef.dimensions();
  std::string dimension = jalib::XToString(dim_int);

  std::string prepareclass = "petabricks::GpuSpatialMethodCallTask<"+objectname
                           + ", " + dimension
                           + ", &" + objectname + "::" + codename + "_prepare"
                           + ">";
  std::string runclass = "petabricks::GpuSpatialMethodCallTask<"+objectname
                           + ", " + dimension
                           + ", &" + objectname + "::" + codename + "_run"
                           + ">";

  o.beginFunc("petabricks::DynamicTaskPtr", codename+"_createtasks", packedargs);

  std::string outputDimensionCheck;
  for( RegionList::const_iterator i = _to.begin(); i != _to.end(); ++i) {
    if(i != _to.begin()) {
      outputDimensionCheck = outputDimensionCheck + " && ";
    }
    outputDimensionCheck = outputDimensionCheck + (*i)->matrix()->name() + ".bytes() == 0";
  }

  o.write("DynamicTaskPtr end = new NullDynamicTask();");

  o.beginIf(outputDimensionCheck);
  o.write("return end;");
  o.endIf();
  
  o.write("ElementT gpu_ratio = "+trans.name()+"_gpuratio/8.0;");
  o.write("GpuTaskInfoPtr taskinfo = new GpuTaskInfo(nodeID, map, gpuCopyOut,"+dimension+", gpu_ratio);");
  
  o.write("DynamicTaskPtr prepare = new "+prepareclass+"(this,_iter_begin, _iter_end, taskinfo, GpuDynamicTask::PREPARE);");
  o.write("prepare->enqueue();");

  int id = 0;
  for(RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i ) {
  //if((*i)->isBuffer()){
  //for(RegionSet::const_iterator i = _loads.begin( ); i != _loads.end( ); ++i ) {
    if((*i)->isBuffer() && _loads.find(*i) != _loads.end()){
      std::string copyinclass = "petabricks::GpuCopyInMethodCallTask<"+objectname
                              + ", " + dimension
                              + ", &" + objectname + "::" + codename + "_copyin_" + (*i)->name()
                              + ">";
      std::string taskid = jalib::XToString(id);
      o.write("DynamicTaskPtr copyin_"+taskid+" = new "+copyinclass+"(this, _iter_begin, _iter_end, taskinfo, "+(*i)->matrix()->name()+".storageInfo());");
      o.write("copyin_"+taskid+"->enqueue();");
      id++;
    }
  }
  for(RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i ) {
  //if((*i)->isBuffer()){
  //for(RegionSet::const_iterator i = _loads.begin( ); i != _loads.end( ); ++i ) {
    if((*i)->isBuffer() && _loads.find(*i) != _loads.end()){
      std::string copyinclass = "petabricks::GpuCopyInMethodCallTask<"+objectname
                              + ", " + dimension
                              + ", &" + objectname + "::" + codename + "_copyin_" + (*i)->name()
                              + ">";
      std::string taskid = jalib::XToString(id);
      o.write("DynamicTaskPtr copyin_"+taskid+" = new "+copyinclass+"(this, _iter_begin, _iter_end, taskinfo, "+(*i)->matrix()->name()+".storageInfo());");
      o.write("copyin_"+taskid+"->enqueue();");
      id++;
    }
  }

  o.write("\nDynamicTaskPtr run = new "+runclass+"(this,_iter_begin, _iter_end, taskinfo, GpuDynamicTask::RUN);");

  o.beginIf("gpuCopyOut == 1");  o.write("run->enqueue();");
  id = 0;
  for(RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i ) {
    if((*i)->isBuffer()){
      o.beginIf("map->find(\""+(*i)->matrix()->name()+"\") != map->end()");
      std::string copyoutclass = "petabricks::GpuCopyOutMethodCallTask<"+objectname
                              + ", " + dimension
                              + ", &" + objectname + "::" + codename + "_copyout_" + (*i)->name()
                              + ">";
      std::string taskid = jalib::XToString(id);
      o.write("DynamicTaskPtr copyout_"+taskid+" = new "+copyoutclass+"(this, _iter_begin, _iter_end, taskinfo, "+(*i)->matrix()->name()+".storageInfo());");
      o.write("end->dependsOn(copyout_"+taskid+");");
      o.write("copyout_"+taskid+"->enqueue();");
      id++;
      o.endIf();
    }
  }
  o.elseIf();
  o.write("end->dependsOn(run);");
  o.write("run->enqueue();");
  o.endIf();
  o.write("return end;");
  o.endFunc();
}

 /// for _from only
 std::string petabricks::UserRule::getLastRowOnGpuGuide(RegionPtr region, int dim_int) {
   if(dim_int == 0)
     return "0";

  std::string dimension = jalib::XToString(dim_int);
  std::string last;

  switch(stencilType(region, false)) {
  case 0: last = "-1"; break;
  case 1: last = "_iter_end["+dimension+"-1]"; break;
  case 2: 
    std::stringstream ss;
    ss << "_iter_end[" << dimension <<  "-1] + "
       << _maxCoordOffsets[region->matrix()->name()][dim_int-1]
       << "-1";
    last = ss.str();
    break;
  }

  return last;
}

 /// for _from only
 std::string petabricks::UserRule::getLastRowOnGpuOffset(RegionPtr region, int dim_int) {
   if(dim_int == 0)
     return "0";

  std::string dimension = jalib::XToString(dim_int);
  std::string last;

  switch(stencilType(region, false)) {
  case 0: last = "INT_MIN"; break;
  case 1: last = "0"; break;
  case 2: 
    std::stringstream ss;
    ss << _maxCoordOffsets[region->matrix()->name()][dim_int-1]
       << "-1";
    last = ss.str();
    break;
  }

  return last;
}
void petabricks::UserRule::generateOpenCLPrepareCode(Transform& trans, std::string& codename, CodeGenerator& o){
  SRCPOSSCOPE();
  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());
  std::vector<std::string> packedargs = iterdef.packedargs();
  int dim_int = iterdef.dimensions();
  std::string dimension = jalib::XToString(dim_int);

  o.beginFunc("petabricks::DynamicTaskPtr", codename+"_prepare", packedargs);
  
  bool divisible = isDivisible();
  if(divisible) {
    o.write("ElementT gpu_ratio = "+trans.name()+"_gpuratio/8.0;");
    RegionPtr proxy = _to.front();
    o.write("IndexT totalRow = "+proxy->matrix()->name()+".size("+jalib::XToString(dim_int - 1)+");");
    o.write("IndexT div = ceil(gpu_ratio * totalRow);");
  }

  for( RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i ) {
    std::string matrix_name = (*i)->matrix()->name();
    if((*i)->isBuffer()) {
      if(_loads.find(*i) == _loads.end())
	o.write(matrix_name + ".storageInfo()->setCreateClMem(true);");
      else
	o.write(matrix_name + ".storageInfo()->setCreateClMem(false);");

      o.write("GpuManager::_currenttaskinfo->addToMatrix(" + matrix_name + ".storageInfo());");

      o.write(matrix_name + ".storageInfo()->setName(std::string(\""+matrix_name+"\"));");
      if(divisible)
	o.write(matrix_name + ".storageInfo()->setLastRowGuide(div, "+dimension+");");
      else
	o.write(matrix_name + ".storageInfo()->setLastRowGuide(-1, "+dimension+");");
    }
    else {
      //TODO
    }
  }
  for( RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i ) {
    std::string matrix_name = (*i)->matrix()->name();
    if((*i)->isBuffer() && _loads.find(*i) != _loads.end()) {
      o.write("GpuManager::_currenttaskinfo->addFromMatrix(" + matrix_name + ".storageInfo());");
      std::string offset = getLastRowOnGpuOffset(*i, dim_int);
      if(divisible && offset != "INT_MIN")
	o.write(matrix_name + ".storageInfo()->setLastRowGuide(div + "+getLastRowOnGpuOffset(*i, dim_int)+","+dimension+");");
      else 
	o.write(matrix_name + ".storageInfo()->setLastRowGuide(-1,"+dimension+");");
    }
  }

  o.write( "return NULL;" );
  o.endFunc();
}

void petabricks::UserRule::generateOpenCLCopyInCode(std::string& codename, std::vector<std::string>& packedargs, CodeGenerator& o, RegionPtr region){
  SRCPOSSCOPE();
  std::string name = region->matrix()->name();
  o.beginFunc("petabricks::DynamicTaskPtr", codename+"_copyin_"+region->name(), packedargs);
  o.write("MatrixStorageInfoPtr storage_"+name+" = "+name+".storageInfo();");
#ifdef GPU_TRACE
  o.write("MatrixIOGeneral().write("+name+");");
  //o.write("MatrixIO().write("+name+".asGpuInputBuffer());");
  o.write("std::cout << \"copyin: bytesOnGpu = \" << storage_" + name + "->bytesOnGpu() << std::endl;");
#endif
  // TODO bytesOnGpu might not be equal to buffer.size
  o.write("cl_int err = clEnqueueWriteBuffer(GpuManager::_queue, storage_"+name+"->getClMem(), CL_FALSE, 0, storage_"+name+"->bytesOnGpu(), "+name+".getGpuInputBufferPtr(), 0, NULL, NULL);");
  //o.write("storage_"+name+"->storeGpuData();");
  o.write("clFlush(GpuManager::_queue);");
#ifdef DEBUG
  o.write("JASSERT(CL_INVALID_CONTEXT != err).Text( \"Failed to write to buffer: invalid context.\");");
  o.write("JASSERT(CL_INVALID_VALUE != err).Text( \"Failed to write to buffer: invalid value.\");");
  o.write("JASSERT(CL_INVALID_BUFFER_SIZE != err).Text( \"Failed to write to buffer: invalid buffer size.\");");
  o.write("JASSERT(CL_DEVICE_MAX_MEM_ALLOC_SIZE != err).Text( \"Failed to write to buffer: max mem alloc.\");");
  o.write("JASSERT(CL_INVALID_HOST_PTR != err).Text( \"Failed to write to buffer: invalid host ptr.\");");
  o.write("JASSERT(CL_MEM_OBJECT_ALLOCATION_FAILURE != err).Text( \"Failed to write to buffer: mem alloc failure.\");");
  o.write("JASSERT(CL_OUT_OF_HOST_MEMORY != err).Text( \"Failed to write to buffer: out of host mem.\");");
#endif
  o.write("JASSERT(CL_SUCCESS == err)(err).Text( \"Failed to write to buffer.\");");
  o.write( "return NULL;" );
  o.endFunc();
}

void petabricks::UserRule::generateOpenCLRunCode(Transform& trans, CodeGenerator& o){
  SRCPOSSCOPE();
  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());
  std::vector<std::string> packedargs = iterdef.packedargs();

  o.beginFunc("petabricks::DynamicTaskPtr", trampcodename(trans) + TX_OPENCL_POSTFIX + "_run", packedargs);
  o.write("cl_int err = CL_SUCCESS;");

  RegionPtr rep = regionRep();//*(_to.begin());
  //if(isSingleCall()) {
  if(rep->getRegionType() == Region::REGION_ALL) {
    o.os( ) << "size_t worksize[] = {1};\n";
    o.os( ) << "size_t workdim = 1;\n";
  }
  else if(rep->getRegionType() == Region::REGION_ROW) {
    o.os( ) << "size_t worksize[] = {" << rep->matrix( )->name( ) << ".size(1)};\n";
    o.os( ) << "size_t workdim = 1;\n";
  }
  else if(rep->getRegionType() == Region::REGION_COL) {
    o.os( ) << "size_t worksize[] = {" << rep->matrix( )->name( ) << ".size(0)};\n";
    o.os( ) << "size_t workdim = 1;\n";
  }
  else if(rep->getRegionType() == Region::REGION_CELL) {
    o.os( ) << "size_t worksize[] = { ";
    for( int i = 0; i < iterdef.dimensions( ); ++i )
      {
        if(i > 0) {
          o.os() << ", ";
        }
        o.os( ) << rep->matrix( )->name( ) << ".size(" << i << ")";
      }
    o.os( ) << "};\n";
    o.os( ) << "size_t workdim = " << iterdef.dimensions( ) << ";\n";
  }
  else {
    UNIMPLEMENTED();
  }

  // Choose between local mem or global mem.
  if(canUseLocalMemory() && iterdef.dimensions() >= 1 && iterdef.dimensions() <= 2) {    o.os() << "cl_kernel clkern;\n";
#ifdef GPU_TRACE
    o.write("std::cout << \"opencl_blocksize \" << opencl_blocksize << std::endl;");
#endif
    if(iterdef.dimensions( ) == 1) {
      o.os() << "if(opencl_blocksize > 0 && worksize[0] % opencl_blocksize*opencl_blocksize == 0) {";
    }
    else {
      o.os() << "if(opencl_blocksize > 0 && worksize[0] % opencl_blocksize == 0 && worksize[1] % opencl_blocksize == 0) {";
    }
    o.os() << "clkern = " << "get_kernel_" << id() << "_local();\n";
#ifdef GPU_TRACE
    o.write("std::cout << \"LOCAL\" << std::endl;");
#endif
    o.os() << "} else {\n";
    o.os() << "clkern = " << "get_kernel_" << id() << "_nolocal();\n";
#ifdef GPU_TRACE
    o.write("std::cout << \"NO LOCAL\" << std::endl;");
#endif
    o.os() << "}\n";
  }
  else {
    o.os() << "cl_kernel clkern = " << "get_kernel_" << id() << "_nolocal();\n";
#ifdef GPU_TRACE
    o.write("std::cout << \"NO CHOICE\" << std::endl;");
#endif
  }

  // clSetKernelArg needs to be conformed with CLCodeGenerator::beginKernel
  int arg_pos = 0;
  // Pass clmem args
  for( RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i ) {
    if((*i)->isBuffer())
      o.write("err |= clSetKernelArg(clkern, "+jalib::XToString(arg_pos++)+", sizeof(cl_mem), "+(*i)->matrix()->name()+".storageInfo()->getClMemPtr());");
  }

  for( RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i ) {
    if((*i)->isBuffer() && _loads.find(*i) != _loads.end())
      o.write("err |= clSetKernelArg(clkern, "+jalib::XToString(arg_pos++)+", sizeof(cl_mem), "+(*i)->matrix()->name()+".storageInfo()->getClMemPtr());");
  }
  o.os( ) << "JASSERT( CL_SUCCESS == err )(err).Text( \"Failed to bind kernel arguments.\" );\n\n";

  // Pass config parameters
  for(ConfigItems::const_iterator i=trans.config().begin(); i!=trans.config().end(); ++i){
    if(i->shouldPass()) {
      o.os( ) << "err |= clSetKernelArg( clkern, " << arg_pos++ << ", sizeof(int), &" << i->name() << " );\n";
    }
  }

  // Bind rule dimension arguments to kernel.
  RegionList::const_iterator output = _to.begin( );
  for( int i = 0; i < iterdef.dimensions( ); ++i )
  {
    o.os( ) << "err |= clSetKernelArg(clkern, " << arg_pos++ << ", sizeof(int), &_iter_begin[" << i << "]);\n";
    o.os( ) << "err |= clSetKernelArg(clkern, " << arg_pos++ << ", sizeof(int), &_iter_end[" << i << "]);\n";
#ifdef GPU_TRACE
    o.write("std::cout << \"iter_begin["+jalib::XToString(i)+"] = \" << _iter_begin["+jalib::XToString(i)+"] << std::endl;");
    o.write("std::cout << \"iter_end["+jalib::XToString(i)+"] = \" << _iter_end["+jalib::XToString(i)+"] << std::endl;");
#endif
  }

  // Bind matrix dimension arguments to kernel.
  int count = 0;
  for( RegionList::const_iterator it = _to.begin( ); it != _to.end( ); ++it )
  {
    std::string name = (*it)->matrix()->name();
    if((*it)->isBuffer()) {
      for( int i = 0; i < (int) (*it)->size() - 1; ++i ) {
        o.os( ) << "int ruledim_out" << count << "_" << i << " = " << name << ".size(" << i << ");\n";
        o.os( ) << "err |= clSetKernelArg(clkern, " << arg_pos++ << ", sizeof(int), &ruledim_out" << count << "_" << i << " );\n";
      }
      count++;
      // Update coverage infomation for dynamic memory management.
      o.write(name+".storageInfo()->incCoverage(_iter_begin, _iter_end, "+name+".intervalSize(_iter_begin, _iter_end));");
    }
  }

  count = 0;
  for( RegionList::const_iterator it = _from.begin( ); it != _from.end( ); ++it )
  {
    if((*it)->isBuffer() && _loads.find(*it) != _loads.end()) {
      for( int i = 0; i < (int) (*it)->size(); ++i ) {
        o.os( ) << "int ruledim_in" << count << "_" << i << " = " << (*it)->matrix( )->name() << ".size(" << i << ");\n";
        o.os( ) << "err |= clSetKernelArg(clkern, " << arg_pos++ << ", sizeof(int), &ruledim_in" << count << "_" << i << " );\n";
      }
      count++;
    }
  }
  o.os( ) << "JASSERT( CL_SUCCESS == err ).Text( \"Failed to bind kernel arguments.\" );\n\n";

  // Invoke kernel.
  o.comment( "Invoke kernel." );

  if(canUseLocalMemory() && iterdef.dimensions() >= 1 && iterdef.dimensions() <= 2) {
    if(iterdef.dimensions( ) == 1) {
      o.os() << "if(opencl_blocksize > 0 && worksize[0] % opencl_blocksize*opencl_blocksize == 0) {\n";
      o.os( ) << "size_t localdim[] = {opencl_blocksize*opencl_blocksize};\n";
      o.os( ) << "int blocksize = opencl_blocksize*opencl_blocksize;\n";
    }
    else {
      o.os() << "if(opencl_blocksize > 0 && worksize[0] % opencl_blocksize == 0 && worksize[1] % opencl_blocksize == 0) {\n";
      o.os( ) << "size_t localdim[] = {opencl_blocksize, opencl_blocksize};\n";
      o.os( ) << "int blocksize = opencl_blocksize;\n";
    }

    o.os( ) << "err = clSetKernelArg(clkern, " << arg_pos++ << ", sizeof(int), &blocksize);\n";
    o.os( ) << "JASSERT( CL_SUCCESS == err ).Text( \"Failed to bind kernel arguments.\" );\n\n";
    o.os( ) << "err = clEnqueueNDRangeKernel(GpuManager::_queue, clkern, workdim, 0, worksize, localdim, 0, NULL, NULL );\n";
    o.os() << "} else {\n";
    o.os( ) << "err = clEnqueueNDRangeKernel(GpuManager::_queue, clkern, workdim, 0, worksize, NULL, 0, NULL, NULL );\n";
    o.os() << "}\n";
  }
  else {
    o.os( ) << "err = clEnqueueNDRangeKernel(GpuManager::_queue, clkern, workdim, 0, worksize, NULL, 0, NULL, NULL );\n";
  }
  o.write("clFinish(GpuManager::_queue);"); //TODO:clFlush but need to make sure we don't change kernel args before it runs

#ifdef GPU_TRACE
  o.os( ) << "std::cout << \"Kernel execution error #\" << err << \": \" << OpenCLUtil::errorString(err) << std::endl;\n";
#endif
  o.os( ) << "JASSERT( CL_SUCCESS == err ).Text( \"Failed to execute kernel.\" );\n";

  o.write( "return NULL;" );
  o.endFunc();
}

void petabricks::UserRule::generateOpenCLCopyOutCode(std::string& codename, CodeGenerator& o, RegionPtr region){
  SRCPOSSCOPE();
  std::string name = region->matrix()->name();
  std::string storage = "storage_" + name;
  std::string dim = jalib::XToString(region->dimensions());
  std::vector<std::string> args;
  args.push_back("std::vector<IndexT*>& begins");
  args.push_back("std::vector<IndexT*>& ends");
  args.push_back("int nodeID");
  o.beginFunc("petabricks::DynamicTaskPtr", codename+"_copyout_"+region->name(), args/*packedargs*/);
  o.write("MatrixStorageInfoPtr "+storage+" = "+name+".storageInfo();");
  o.write("IndexT sizes["+dim+"];");
  o.write("memcpy(sizes, "+storage+"->sizes(), sizeof(sizes));");
  o.write("MatrixStoragePtr outstorage = "+storage+"->getGpuOutputStoragePtr(nodeID);");
  o.write("MatrixRegion<"+dim+", "+STRINGIFY(MATRIX_ELEMENT_T)+"> normalized(outstorage, outstorage->data(), sizes);");
  //o.write("outstorage->print();");
  //o.write("std::cout << sizes[0] << \" \" << sizes[1] << std::endl;");/
  o.write("normalized.copyTo("+name+", begins, ends);");
#ifdef GPU_TRACE
  //o.write("outstorage->print();");
  //o.write("std::cout << sizes[0] << \" \" << sizes[1] << std::endl;");
  //o.write("MatrixIO().write(normalized);");
  //o.write("MatrixIO().write("+name+");");
#endif
  o.write( "return NULL;" );
  o.endFunc();
}

void petabricks::UserRule::generateOpenCLKernel( Transform& trans, CLCodeGenerator& clo, IterationDefinition& iterdef, bool local)
{
  SRCPOSSCOPE();
  // This is only null if code generation failed (that is, the rule is
  // unsupported.)
  if( !isOpenClRule() )
    return;

  std::vector<std::string> from_matrices, to_matrices;
  for( RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i )
    to_matrices.push_back( (*i)->name( ) );
  for( RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i )
    if(_loads.find(*i) != _loads.end())
      from_matrices.push_back( (*i)->name( ) );

  clo.beginKernel(_to, _from, _loads, iterdef.dimensions(), trans, local);

  // Get indices.
  for( int i = 0; i < iterdef.dimensions( ); ++i )
    clo.os( ) << "int " << _getOffsetVarStr( _id, i, NULL ) << " = get_global_id( " << i << " );\n";

  // Define rule index variables (from _definitions).
  for( FormulaList::iterator it = _definitions.begin( ); it != _definitions.end( ); ++it )
    clo.os( ) << STRINGIFY(MATRIX_INDEX_T) " " << (*it)->lhs()->toString() << " = " << (*it)->rhs()->toString() << ";\n";

  // Define Sizes
  //trans.extractOpenClSizeDefines(clo, iterdef.dimensions());

  // Use local memory
  if(local)
    generateLocalBuffers(clo);

  // Conditional to ensure we are about to work on a valid part of the buffer.
  if(iterdef.dimensions()>0)
    clo.os( ) << "if( ";
  for( int i = 0; i < iterdef.dimensions( ); ++i )
  {
    clo.os( ) << _getOffsetVarStr( _id, i, NULL ) << " >= dim_d" << i << "_begin && ";
    clo.os( ) << _getOffsetVarStr( _id, i, NULL ) << " < dim_d" << i << "_end ";
    if( i != ( iterdef.dimensions( ) - 1 ) )
    clo.os( ) << "&& ";
  }
  if(iterdef.dimensions()>0)
    clo.os( ) << ") {\n";

  // Generate indices into input and output arrays.
  for( RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i )
  {
    // Build & normalize formula for index.
    int minCoordSize = (*i)->minCoord().size();
    FormulaPtr idx_formula = new FormulaAdd((*i)->minCoord().at(minCoordSize - 1), FormulaInteger::zero());
    for( int j = minCoordSize - 2; j >= 0; --j )
    {
      std::stringstream sizevar;
      sizevar << "dim_" << (*i)->name( ) << "_d" << j;
      idx_formula = new FormulaAdd( (*i)->minCoord( ).at( j ),
            new FormulaMultiply( new FormulaVariable( sizevar.str( ) ), idx_formula ) );
    }

    clo.os( ) << "int idx_" << (*i)->name( ) << " = ";
    idx_formula->print( clo.os( ) );
    clo.os( ) << ";\n";
  }

  for( RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i )
  {
    if(_loads.find(*i) == _loads.end()) continue;
    // Build & normalize formula for index.
    FormulaPtr idx_formula;
    switch((*i)->getRegionType()) {
      case Region::REGION_CELL:
      case Region::REGION_BOX:
        {
        idx_formula = FormulaInteger::zero( );
        int minCoordSize = (*i)->minCoord().size();
        if(minCoordSize > 0) {
          idx_formula = new FormulaAdd((*i)->minCoord().at(minCoordSize - 1), idx_formula);
          for( int j = minCoordSize - 2; j >= 0; --j )
          {
            std::stringstream sizevar;
            sizevar << "dim_" << (*i)->name( ) << "_d" << j;
            idx_formula = new FormulaAdd( (*i)->minCoord( ).at( j ),
                  new FormulaMultiply( new FormulaVariable( sizevar.str( ) ), idx_formula ) );
          }
        }
        }
        break;
      case Region::REGION_ROW:
        idx_formula = new FormulaMultiply( new FormulaVariable( "dim_" + (*i)->name( ) + "_d0" ), (*i)->minCoord().at(1) );
        break;
      case Region::REGION_COL:
        idx_formula = (*i)->minCoord().at(0);
        break;
      case Region::REGION_ALL:
        idx_formula = FormulaInteger::zero( );
        break;
      default:
        UNIMPLEMENTED();
/*
    FormulaPtr idx_formula = FormulaInteger::zero( );
    int minCoordSize = (*i)->minCoord().size();
    if(minCoordSize > 0) {
      idx_formula = new FormulaAdd((*i)->minCoord().at(minCoordSize - 1), idx_formula);
      for( int j = minCoordSize - 2; j >= 0; --j )
      {
        std::stringstream sizevar;
        sizevar << "dim_" << (*i)->name( ) << "_d" << j;
        idx_formula = new FormulaAdd( (*i)->minCoord( ).at( j ),
              new FormulaMultiply( new FormulaVariable( sizevar.str( ) ), idx_formula ) );
      }
*/
    }
    clo.os( ) << "int idx_" << (*i)->name( ) << " = ";
    idx_formula->print( clo.os( ) );
    clo.os( ) << ";\n";
  }

  // Quick hack -- generate a macro that will store the output from the rule.
  {
    RegionList::const_iterator i = _to.begin( );
    clo.os( ) << "#define RETURN(x) _region_" << (*i)->name( ) << "[idx_" << (*i)->name( ) << "] = x; return\n";
  }

  // Support for multiple-output rules
  /*for( RegionList::const_iterator i = _to.begin( ); i != _to.end( ); ++i )
  {
    clo.os() << "#define " << (*i)->name( ) << " _region_" << (*i)->name( ) << "[idx_" << (*i)->name( ) << "]\n";
  }*/

  // Generate OpenCL implementation of rule logic.
 #ifdef DEBUG
  std::cerr << "--------------------\nAFTER GPU PASSES:\n" << _bodyir[RuleFlavor::OPENCL] << std::endl;
  {
    if( _bodyir[RuleFlavor::OPENCL] )
    {
      DebugPrintPass pdebug;
      _bodyir[RuleFlavor::OPENCL]->accept(pdebug);
    }
    else
    {
      std::cerr << " ( No OpenCL code was generated. )\n";
    }
  }
  std::cerr << "--------------------\n";
 #endif
  if(local)
    clo.write( _bodyirLocalMem->toString( ) ); // Use local memory
  else
    clo.write( _bodyir[RuleFlavor::OPENCL]->toString( ) ); // Use global memory

  // Close conditional and kernel.
  if(iterdef.dimensions()>0)
    clo.os( ) << "}\n";
  clo.endKernel( );
}

void petabricks::UserRule::generateLocalBuffers(CLCodeGenerator& clo) {
  IterationDefinition iterdef(*this, getSelfDependency(), isSingleCall());

  for( int i = 0; i < iterdef.dimensions( ); ++i ) {
    std::string var;
    if(i==0) var = "x";
    else     var = "y";
    clo.os( ) << "int " << var << "_local = get_local_id( " << i << " );\n";
  }

  clo.os() << "int _x_ = " << _getOffsetVarStr( _id, 0, NULL ) << ";\n";
  if(iterdef.dimensions() == 2)
    clo.os() << "int _y_ = " << _getOffsetVarStr( _id, 1, NULL ) << ";\n";

  for( RegionList::const_iterator i = _from.begin( ); i != _from.end( ); ++i )
  {
    if((*i)->isBuffer()) {
      std::string matrix = (*i)->matrix()->name();
      if(_local.find(matrix) != _local.end()){
        for(int d = 0; d < iterdef.dimensions( ); d++) {
          clo.os( ) << "int " << matrix << jalib::XToString(d) << "_minoffset = -(" << _minCoordOffsets[matrix][d] << ");\n";
          clo.os( ) << "int " << matrix << jalib::XToString(d) << "_maxoffset = " << _maxCoordOffsets[matrix][d] << " - 1;\n";
        }

         clo.os() << "__local " << STRINGIFY( MATRIX_ELEMENT_T ) << " buff_" << matrix;
        for(int d = iterdef.dimensions( ) - 1; d >= 0; d--) {
          int block_size;
          if(iterdef.dimensions()==0)
            block_size = MAX_BLOCK_SIZE * MAX_BLOCK_SIZE;
          else
            block_size = MAX_BLOCK_SIZE;
          FormulaPtr formula = new FormulaSubtract(_maxCoordOffsets[matrix][d], _minCoordOffsets[matrix][d]);
          formula = new FormulaAdd(new FormulaLiteral<int>(block_size - 1),formula);
          clo.os() << "[" << MAXIMA.normalize(formula) << "]";
        }
        clo.os() << ";\n";

        if(iterdef.dimensions( )==1) {
          // Left region
          clo.os() << "for(int i = -" << matrix << "0_minoffset; i < 0; i += block_size) {\n";
          clo.os() << "  if(_x_ + i >= 0)\n";
          clo.os() << "    buff_" << matrix << "[x_local + " << matrix << "0_minoffset + i] = "
                                  << "_region_" << (*i)->name() << "[_x_ + i];\n";
          clo.os() << "}\n";
          // Right region
          clo.os() << "for(int i = " << matrix << "0_maxoffset; i > 0; i -= block_size) {\n";
          clo.os() << "  if(_x_ + i < dim_" << (*i)->name() << "_d0)\n";
          clo.os() << "    buff_" << matrix << "[x_local + " << matrix << "0_minoffset + i] = "
                                  << "_region_" << (*i)->name() << "[_x_ + i];\n";
          clo.os() << "}\n";
          // Self
          clo.os() << "buff_" << matrix << "[x_local + " << matrix << "0_minoffset] = "
                              << "_region_" << (*i)->name() << "[_x_];\n";
        }
        else {
          // Middle region
          clo.os() << "for(int i = -" << matrix << "0_minoffset; i < 0; i += block_size) {\n";
          clo.os() << "  if(_x_ + i >= 0)\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset]"
                                  << "[x_local + " << matrix << "0_minoffset + i] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_ + i];\n";
          clo.os() << "}\n";
          clo.os() << "for(int i = " << matrix << "0_maxoffset; i > 0; i -= block_size) {\n";
          clo.os() << "  if(_x_ + i < dim_" << (*i)->name() << "_d0)\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset]"
                                  << "[x_local + " << matrix << "0_minoffset + i] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_ + i];\n";
          clo.os() << "}\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset]"
                                  << "[x_local + " << matrix << "0_minoffset] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_];\n";

          // Top region
          clo.os() << "for(int j = -" << matrix << "1_minoffset; j < 0; j += block_size) {\n";
          clo.os() << "if(_y_ + j >= 0) {\n";
          clo.os() << "for(int i = -" << matrix << "0_minoffset; i < 0; i += block_size) {\n";
          clo.os() << "  if(_x_ + i >= 0)\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset + j]"
                                  << "[x_local + " << matrix << "0_minoffset + i] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_ + j) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_ + i];\n";
          clo.os() << "}\n";
          clo.os() << "for(int i = " << matrix << "0_maxoffset; i > 0; i -= block_size) {\n";
          clo.os() << "  if(_x_ + i < dim_" << (*i)->name() << "_d0)\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset + j]"
                                  << "[x_local + " << matrix << "0_minoffset + i] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_ + j) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_ + i];\n";
          clo.os() << "}\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset + j]"
                                  << "[x_local + " << matrix << "0_minoffset] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_ + j) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_];\n";
          clo.os() << "}\n";
          clo.os() << "}\n";

          // Bottom region
          clo.os() << "for(int j = " << matrix << "1_maxoffset; j > 0; j -= block_size) {\n";
          clo.os() << "if(_y_ + j < dim_" << (*i)->name() << "_d1) {\n";
          clo.os() << "for(int i = -" << matrix << "0_minoffset; i < 0; i += block_size) {\n";
          clo.os() << "  if(_x_ + i >= 0)\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset + j]"
                                  << "[x_local + " << matrix << "0_minoffset + i] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_ + j) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_ + i];\n";
          clo.os() << "}\n";
          clo.os() << "for(int i = " << matrix << "0_maxoffset; i > 0; i -= block_size) {\n";
          clo.os() << "  if(_x_ + i < dim_" << (*i)->name() << "_d0)\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset + j]"
                                  << "[x_local + " << matrix << "0_minoffset + i] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_ + j) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_ + i];\n";
          clo.os() << "}\n";
          clo.os() << "    buff_" << matrix << "[y_local + " << matrix << "1_minoffset + j]"
                                  << "[x_local + " << matrix << "0_minoffset] = "
                                  << "_region_" << (*i)->name()
                                  << "[(_y_ + j) * dim_" << (*i)->name() << "_d0 + "
                                  << "_x_];\n";
          clo.os() << "}\n";
          clo.os() << "}\n";

        } // endif 2D

      } // endif gernerate buffer
    }
  }
  clo.os() << "barrier(CLK_LOCAL_MEM_FENCE);\n";
}

#endif



void petabricks::UserRule::generateTrampCellCodeSimple(Transform& trans, CodeGenerator& o, RuleFlavor flavor){
  o.comment("MARKER 4");
  SRCPOSSCOPE();
#ifdef DEBUG
  JASSERT( RuleFlavor::OPENCL != flavor );
#endif

  if( ( RuleFlavor::WORKSTEALING == flavor ) && !isRecursive( ) ) {
    //use sequential code if rule doesn't make calls
    flavor = RuleFlavor::SEQUENTIAL;
  }

  std::vector<std::string> args;
  std::vector<std::string> to_0d;
  for(RegionList::const_iterator i=_to.begin(); i!=_to.end(); ++i){
    if (flavor == RuleFlavor::DISTRIBUTED_SCRATCH
        && (!(*i)->isOptional())) {
      Region region = *i;
      std::string name = (*i)->matrix()->name();
      if (hasCellAccess(name)) {
        region.changeMatrix(lookupScratch(name));
        args.push_back(region.generateAccessorCode(*(_scratchRegionLowerBounds[name])));
      } else {
        args.push_back((*i)->generateAccessorCode());
      }

    } else if (flavor == RuleFlavor::WORKSTEALING_PARTIAL) {
      CoordinateFormulaPtr offset = _partialCoordOffsets[(*i)->matrix()->name()];
      args.push_back((*i)->generateAccessorCode(*offset));

    } else {
      args.push_back((*i)->generateAccessorCode());
    }
  }
  for(RegionList::const_iterator i=_from.begin(); i!=_from.end(); ++i){
    if (flavor == RuleFlavor::DISTRIBUTED_SCRATCH
        && (!(*i)->isOptional())) {
      Region region = *i;
      std::string name = (*i)->matrix()->name();
      if (hasCellAccess(name)) {
        region.changeMatrix(lookupScratch(name));
        args.push_back(region.generateAccessorCode(*(_scratchRegionLowerBounds[name])));
      } else {
        args.push_back((*i)->generateAccessorCode());
      }

    } else if (flavor == RuleFlavor::WORKSTEALING_PARTIAL) {
      CoordinateFormulaPtr offset = _partialCoordOffsets[(*i)->matrix()->name()];
      args.push_back((*i)->generateAccessorCode(*offset));

    } else {
      args.push_back((*i)->generateAccessorCode());
    }
  }
  for(ConfigItems::const_iterator i=trans.config().begin(); i!=trans.config().end(); ++i){
    if(i->shouldPass())
      args.push_back(i->name());
  }
  for(ConfigItems::const_iterator i=_duplicateVars.begin(); i!=_duplicateVars.end(); ++i){
    args.push_back(i->name());
  }

  for(int i=0; i<dimensions(); ++i)
    args.push_back(getOffsetVar(i)->toString());

  if (RuleFlavor::SEQUENTIAL == flavor || RuleFlavor::WORKSTEALING_PARTIAL == flavor) {
    o.call(implcodename(trans)+TX_STATIC_POSTFIX, args);

    for(std::vector<std::string>::const_iterator i=to_0d.begin(); i!=to_0d.end(); ++i){
      o.write((*i) + " = scratch_" + (*i) + ";");
    }

  } else {
    std::string classname = implcodename(trans)+"_"+flavor.str();
    o.setcall("jalib::JRef<"+classname+"> _rule", "new "+classname, args);
    std::string tasktype = "petabricks::MethodCallTask<"+classname+", &"+classname+"::runDynamic>";
    o.write("_task = new "+tasktype+"(_rule);");
  }
}

 void petabricks::UserRule::generateToLocalRegionCode(Transform& trans, CodeGenerator& o, RuleFlavor flavor, IterationDefinition& iterdef, bool generateWorkStealingRegion, bool generateIterTrampMetadata, bool generatePartialTrampMetadata) {
  std::vector<std::string> args;

  _scratchRegionLowerBounds.clear();
  CoordinateFormula iterdefEndInclusive = iterdef.end();
  iterdefEndInclusive.subToEach(FormulaInteger::one());

  if (_scratch.size() > 0) {
    o.comment("Copy to local region");
  }

  std::string scratchSuffix = "";
  if (generateWorkStealingRegion) {
    // generate workstealing region
    scratchSuffix = "_distributed";
  }

  for(MatrixDefMap::const_iterator i=_scratch.begin(); i!=_scratch.end(); ++i){
    if (!hasCellAccess(i->first) && !generatePartialTrampMetadata) {
      continue;
    }
    MatrixDefPtr matrix = i->second;
    bool isConst = (matrix->type() == MatrixDef::T_FROM);

    SimpleRegionPtr region = _scratchBoundingBox[trans.lookupMatrix(i->first)];

    CoordinateFormulaPtr lowerBounds = region->getIterationLowerBounds(iterdef.var(), iterdef.begin(), iterdefEndInclusive);
    CoordinateFormulaPtr upperBounds = region->getIterationUpperBounds(iterdef.var(), iterdef.begin(), iterdefEndInclusive);

    _scratchRegionLowerBounds[i->first] = lowerBounds;

    if (matrix->numDimensions() > 0) {
      o.write(matrix->typeName(flavor, isConst) + " remote_" + matrix->name() + ";");
      o.write("{");
      o.incIndent();
      o.write("IndexT _tmp_begin[] = {" + lowerBounds->toString() + "};");
      o.write("IndexT _tmp_end[] = {" + upperBounds->toString() + "};");
      o.write("remote_" + matrix->name() + " = " + i->first + ".region(_tmp_begin, _tmp_end);");

      if (generatePartialTrampMetadata) {
        o.write("memcpy(metadata->" + i->first + "_offset, _tmp_begin, sizeof(IndexT) * " + jalib::XToString(lowerBounds->size()) + ");");
      }

      o.decIndent();
      o.write("}");

      o.write(matrix->typeName(flavor, isConst) + " " + matrix->name() + scratchSuffix + "(remote_" + matrix->name() + ".size());");
      o.write(matrix->name() + scratchSuffix + ".allocDataLocal();");

      if (matrix->type() == MatrixDef::T_FROM) {
        o.write("remote_" + matrix->name() + ".localCopy(" + matrix->name() + scratchSuffix + ", true);");

      } else {
        o.write("remote_" + matrix->name() + ".localCopy(" + matrix->name() + scratchSuffix + ", false);");
      }
    } else {
      o.write(matrix->typeName(flavor, isConst) + " remote_" + matrix->name() + " = " + i->first + ";");
      if (isConst) {
        o.write(matrix->typeName(flavor, isConst) + " " + matrix->name() + scratchSuffix + "(" + i->first + ");");
      } else {
        o.write(matrix->typeName(flavor, isConst) + " " + matrix->name() + scratchSuffix + ";");
        o.write(matrix->name() + scratchSuffix + ".writeCell(NULL, " + i->first + ".readCell(NULL));");
      }
    }

    if (generateWorkStealingRegion) {
      // generate workstealing region
      std::string target;
      if (generatePartialTrampMetadata) {
        target = "metadata->" + i->first;
      } else {
        target = matrix->typeName(RuleFlavor::WORKSTEALING, isConst) + " " + matrix->name();
      }

      if (matrix->numDimensions() == 0) {
        if (isConst) {
          if (generatePartialTrampMetadata) {
            o.write(target + " = " + matrix->typeName(RuleFlavor::WORKSTEALING, isConst) + "(" + i->first + ");");
          } else {
            o.write(target + "(" + i->first + ");");
          }
        } else {
          o.write(target + " = " + matrix->typeName(RuleFlavor::WORKSTEALING, isConst) + "::allocate();");
          if (generatePartialTrampMetadata) {
            o.write("metadata->" + i->first + ".cell() = " + matrix->name() + scratchSuffix + ".readCell(NULL);");
          } else {
            o.write(matrix->name() + ".cell() = " + matrix->name() + scratchSuffix + ".readCell(NULL);");
          }
        }
      } else {
        o.write(target + " = CONVERT_TO_LOCAL(" + matrix->name() + scratchSuffix + ");");
      }
    }

    args.push_back(matrix->name());

    if (matrix->type() == MatrixDef::T_TO) {
      SimpleRegionPtr toRegion = _toBoundingBox[trans.lookupMatrix(i->first)];
      CoordinateFormulaPtr toLowerBounds = toRegion->getIterationLowerBounds(iterdef.var(), iterdef.begin(), iterdefEndInclusive);
      CoordinateFormulaPtr toUpperBounds = toRegion->getIterationUpperBounds(iterdef.var(), iterdef.begin(), iterdefEndInclusive);
      toLowerBounds->sub(lowerBounds);
      toUpperBounds->sub(lowerBounds);

      if (matrix->numDimensions() > 0) {
        if (!generatePartialTrampMetadata) {
          o.write(matrix->typeName(flavor) + " remote_TO_" + matrix->name() + ";");
          o.write(matrix->typeName(flavor) + " local_TO_" + matrix->name() + ";");
        }
        o.write("{");
        o.incIndent();
        o.write("IndexT _tmp_begin[] = {" + toLowerBounds->toString() + "};");
        o.write("IndexT _tmp_end[] = {" + toUpperBounds->toString() + "};");

        if (generatePartialTrampMetadata) {
          o.write("metadata->remote_TO_" + i->first + " = remote_" + matrix->name() + ".region(_tmp_begin, _tmp_end);");
          o.write("metadata->local_TO_" + i->first + " = " + matrix->name() + scratchSuffix + ".region(_tmp_begin, _tmp_end);");

        } else {
          o.write("remote_TO_" + matrix->name() + " = remote_" + matrix->name() + ".region(_tmp_begin, _tmp_end);");
          o.write("local_TO_" + matrix->name() + " = " + matrix->name() + scratchSuffix + ".region(_tmp_begin, _tmp_end);");
        }
        o.decIndent();
        o.write("}");

        args.push_back("remote_TO_" + matrix->name());
        args.push_back("local_TO_" + matrix->name());

      } else {
        // 0D
        if (generatePartialTrampMetadata) {
          o.write("metadata->remote_TO_" + i->first + " = " + i->first + ";");
        }
      }
    }
  }

  if (generateIterTrampMetadata) {
    std::string metadataclass = itertrampmetadataname(trans)+"_"+flavor.str();
    std::string argsStr = jalib::JPrintable::stringStlList(args.begin(), args.end(), ", ");
    o.write("jalib::JRef<" + metadataclass + "> metadata = new " + metadataclass + "(" + argsStr + ");");

    o.write("{");
    o.incIndent();
    o.write("IndexT _tmp_begin[] = {" + iterdef.begin().toString() + "};");
    o.write("metadata->begin = (IndexT*)malloc(sizeof _tmp_begin);");
    o.write("memcpy(metadata->begin, _tmp_begin, sizeof _tmp_begin);");
    o.write("IndexT _tmp_end[] = {" + iterdef.end().toString() + "};");
    o.write("metadata->end = (IndexT*)malloc(sizeof _tmp_end);");
    o.write("memcpy(metadata->end, _tmp_end, sizeof _tmp_end);");
    o.decIndent();
    o.write("}");
  }
}

void petabricks::UserRule::generateCallCode(const std::string& name,
                                            Transform& trans,
                                            CodeGenerator& o,
                                            const SimpleRegionPtr& region,
                                            RuleFlavor flavor,
                                            bool /*wrap*/,
                                            std::vector<RegionNodeGroup>&,
                                            int,
                                            int,
                                            SpatialCallType spatialCallType){
  SRCPOSSCOPE();
  o.comment("from UserRule::generateCallCode():");
  switch(flavor) {
  case RuleFlavor::SEQUENTIAL:
    o.callSpatial(trampcodename(trans)+TX_STATIC_POSTFIX, region);
    break;
  case RuleFlavor::WORKSTEALING:
  case RuleFlavor::DISTRIBUTED:
    if (spatialCallType == SpatialCallTypes::WORKSTEALING_PARTIAL) {
      std::string methodname = partialtrampcodename(trans)+"_workstealing";
      std::string metadataname = partialtrampmetadataname(trans)+"_workstealing";
      bool shouldGenerateMetadata = (flavor == RuleFlavor::DISTRIBUTED);
      o.mkPartialSpatialTask(name, metadataname, methodname, region, spatialCallType, shouldGenerateMetadata);
    } else {
#ifdef HAVE_OPENCL
      if(wrap)
        o.mkSpatialTask(name, trans.instClassName(), trampcodename(trans)+"_"+flavor.str()+"_wrap", region, spatialCallType);
      else
#endif
        o.mkSpatialTask(name, trans.instClassName(), trampcodename(trans)+"_"+flavor.str(), region, spatialCallType);
    }
    break;
  default:
    UNIMPLEMENTED();
  }
}

int petabricks::UserRule::dimensions() const {
//   return (int)_applicableRegion->dimensions();
  int m=0;
  for(RegionList::const_iterator i=_to.begin(); i!=_to.end(); ++i){
    m=std::max(m, (int)(*i)->dimensions());
  }
  return m;
}

void petabricks::UserRule::addAssumptions() const {
  SRCPOSSCOPE();
  for(int i=0; i<dimensions(); ++i){
    MaximaWrapper::instance().assume(new FormulaGE(getOffsetVar(i), _applicableRegion->minCoord()[i]));
    MaximaWrapper::instance().assume(new FormulaLT(getOffsetVar(i), _applicableRegion->maxCoord()[i]));
  }
  for(RegionList::const_iterator i=_to.begin(); i!=_to.end(); ++i)
    (*i)->addAssumptions();
  for(RegionList::const_iterator i=_from.begin(); i!=_from.end(); ++i)
    (*i)->addAssumptions();
  for(FormulaList::const_iterator i=_conditions.begin(); i!=_conditions.end(); ++i)
    MaximaWrapper::instance().assume(*i);
  for(FormulaList::const_iterator i=_definitions.begin(); i!=_definitions.end(); ++i)
    MaximaWrapper::instance().assume(*i);
}

void petabricks::UserRule::collectDependencies(StaticScheduler& scheduler){
  SRCPOSSCOPE();
  for( MatrixDependencyMap::const_iterator p=_provides.begin()
     ; p!=_provides.end()
     ; ++p)
  {
    ChoiceDepGraphNodeSet pNode = scheduler.lookupNode(p->first, p->second->region());
    for( MatrixDependencyMap::const_iterator d=_depends.begin()
       ; d!=_depends.end()
       ; ++d)
    {
      ChoiceDepGraphNodeSet dNode = scheduler.lookupNode(d->first, d->second->region());
      for(ChoiceDepGraphNodeSet::iterator a=pNode.begin(); a!=pNode.end(); ++a) {
        for(ChoiceDepGraphNodeSet::iterator b=dNode.begin(); b!=dNode.end(); ++b) {
          if( (*a)->dependencyPossible(*b, d->second->direction()) ) {
            (*a)->addDependency(*b, this, d->second->direction());
          }
        }
      }
    }

    //null depedency on all other output regions
    for( MatrixDependencyMap::const_iterator pp=_provides.begin()
      ; pp!=_provides.end()
      ; ++pp)
    {
      if(p!=pp){
        ChoiceDepGraphNodeSet dNode = scheduler.lookupNode(pp->first, pp->second->region());
        for(ChoiceDepGraphNodeSet::iterator a=pNode.begin(); a!=pNode.end(); ++a)
          for(ChoiceDepGraphNodeSet::iterator b=dNode.begin(); b!=dNode.end(); ++b)
            (*a)->addDependency(*b, this, DependencyDirection(std::max(1,dimensions()), DependencyDirection::D_MULTIOUTPUT));
      }
    }
  }
  //TODO collect edge/direction dependencies
}

petabricks::DependencyDirection petabricks::UserRule::getSelfDependency() const {
  SRCPOSSCOPE();
  DependencyDirection rv(dimensions());
  for( MatrixDependencyMap::const_iterator p=_provides.begin()
     ; p!=_provides.end()
     ; ++p)
  {
    MatrixDependencyMap::const_iterator d = _depends.find(p->first);
    if(d!=_depends.end()){
      const DependencyDirection& dir = d->second->direction();
      rv.addDirection(dir);
    }
  }
  return rv;
}

std::string petabricks::UserRule::implcodename(Transform& trans) const {
  return trans.name()+"_rule" + jalib::XToString(_id-trans.ruleIdOffset());
}
std::string petabricks::UserRule::trampcodename(Transform& trans) const {
  std::string s = trans.name()+"_apply_rule" + jalib::XToString(_id-trans.ruleIdOffset());
  for(size_t i=0; i<_duplicateVars.size(); ++i){
    long t = _duplicateVars[i].initial().i();
    if(_duplicateVars[i].min()<0)
      t-=_duplicateVars[i].min().i(); //handle negative case
    s += "_" + _duplicateVars[i].name() + jalib::XToString(t);
  }
  return s;
}

std::string petabricks::UserRule::itertrampcodename(Transform& trans) const {
  return trampcodename(trans) + "_iter";
}

std::string petabricks::UserRule::itertrampmetadataname(Transform& trans) const {
  return trampcodename(trans) + "_iter_metadata";
}

std::string petabricks::UserRule::partialtrampcodename(Transform& trans) const {
  return trampcodename(trans) + "_partial";
}
std::string petabricks::UserRule::partialtrampmetadataname(Transform& trans) const {
  return trampcodename(trans) + "_partial_metadata";
}

size_t petabricks::UserRule::duplicateCount() const {
  int c = 1;
  for(size_t i=0; i<_duplicateVars.size(); ++i)
    c*=_duplicateVars[i].range();
  return c;
}
size_t petabricks::UserRule::setDuplicateNumber(size_t c) {
  SRCPOSSCOPE();
  size_t prev = getDuplicateNumber();
#ifdef DEBUG
  size_t origC=c;
#endif
  for(size_t i=0; i<_duplicateVars.size(); ++i){
    ConfigItem& dv = _duplicateVars[i];
    dv.setInitial( int( dv.min().i() + (c % dv.range())));
    c /= dv.range();
  }
#ifdef DEBUG
  //make sure we did it right
  JASSERT(getDuplicateNumber()==origC)(getDuplicateNumber())(origC);
#endif
  return prev;
}
size_t petabricks::UserRule::getDuplicateNumber() {
  SRCPOSSCOPE();
  int lastRange = 1;
  int c = 0;
  for(ssize_t i=_duplicateVars.size()-1; i>=0; --i){
    ConfigItem& dv = _duplicateVars[i];
    c*=lastRange;
    c+=dv.initial().i()-dv.min().i();
    lastRange = dv.range();
  }
  return c;
}


void petabricks::UserRule::removeDimensionFromRegionList(RegionList& list,
                                                     const MatrixDefPtr matrix,
                                                     const size_t dimension) {
  for(RegionList::iterator i=list.begin(), e=list.end(); i!=e; ++i) {
    RegionPtr region = *i;

    const MatrixDefPtr& fromMatrix = region->matrix();

    if(fromMatrix != matrix) {
      continue;
    }

    region->removeDimension(dimension);
  }
}


void petabricks::UserRule::removeDimensionFromMatrixDependencyMap(MatrixDependencyMap& map,
                                                      const MatrixDefPtr matrix,
                                                      const size_t dimension) {
  MatrixDependencyMap::iterator dependencyIterator = map.find(matrix);
  if(dependencyIterator == map.end()) {
    //No dependencies to remove
    return;
  }

  MatrixDependencyPtr dependency = dependencyIterator->second;

  dependency->removeDimension(dimension);
}


void petabricks::UserRule::removeDimensionFromDefinitions(const size_t dimension) {
  FormulaPtr offsetVar = getOffsetVar(dimension);

  for(FormulaList::iterator i=_definitions.begin(), e=_definitions.end();
      i != e;
      ++i) {
    FormulaPtr definition = *i;
    FormulaPtr definingVar = definition->rhs();
    if(definingVar->toString() != offsetVar->toString()) {
      //It's not the variable we are looking for
      continue;
    }

    //It's the variable we are looking for
    //Let's erase it!
    _definitions.erase(i);
    return;
  }
}


void petabricks::UserRule::removeDimensionFromMatrix(const MatrixDefPtr matrix,
                                                      const size_t dimension) {
  removeDimensionFromRegionList(_to, matrix, dimension);
  removeDimensionFromRegionList(_from, matrix, dimension);

  removeDimensionFromDefinitions(dimension);
}


void petabricks::UserRule::trimDependency(DependencyDirection& dep,
                                          const ChoiceDepGraphNode& /*from*/,
                                          const ChoiceDepGraphNode& /*to*/)
{
  if(dep.isMultioutput() && _to.size() <= 1) {
    dep.removeMultioutput();
  }
}

void petabricks::DataDependencyVectorMap::print(std::ostream& o) const {
  o << "DataDependencyVectorMap: ";
  for(DataDependencyVectorMap::const_iterator i=this->begin(), e=this->end();
      i!=e;
      ++i) {
    MatrixDefPtr matrixDef = i->first;
    const CoordinateFormula& dependencyVector = i->second;
    o << "\n  MatrixDef: " << matrixDef;
    o << "\t\tDependency vector: " << dependencyVector;
  }
}

namespace {
  void fixVersionedRegionsTypeInList(petabricks::RegionList list) {
    for(petabricks::RegionList::iterator i=list.begin(), e=list.end(); i!=e; ++i) {
      petabricks::RegionPtr region = *i;

      region->fixTypeIfVersioned();
    }
  }
}

void petabricks::UserRule::fixVersionedRegionsType() {
  fixVersionedRegionsTypeInList(_to);
  fixVersionedRegionsTypeInList(_from);
}

petabricks::RegionList petabricks::UserRule::getSelfDependentRegions() {
  RegionList list = RegionList();

  for(RegionList::iterator in=_from.begin(), in_end=_from.end();
      in!=in_end;
      ++in) {

    for (RegionList::iterator out=_to.begin(), out_end=_to.end();
         out != out_end;
         ++out)
         {
      if((*in)->matrix()->name() == (*out)->matrix()->name()) {
        list.push_back(*in);
      }
    }
  }
  return list;
}

petabricks::RegionList petabricks::UserRule::getNonSelfDependentRegions() {
  RegionList list = RegionList();

  //Add regions from _from, not in _to
  for(RegionList::iterator in=_from.begin(), in_end=_from.end();
      in!=in_end;
      ++in) {

    for (RegionList::iterator out=_to.begin(), out_end=_to.end();
         out != out_end;
         ++out)
         {
      if((*in)->matrix()->name() != (*out)->matrix()->name()) {
        list.push_back(*in);
      }
    }
  }

  //Add regions from _to, not in _from
  for(RegionList::iterator out=_to.begin(), out_end=_to.end();
      out!=out_end;
      ++out) {

    for (RegionList::iterator in=_from.begin(), in_end=_from.end();
         in != in_end;
         ++in)
         {
      if((*in)->matrix()->name() != (*out)->matrix()->name()) {
        list.push_back(*in);
      }
    }
  }

  return list;
}
