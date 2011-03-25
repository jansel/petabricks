/***************************************************************************
 *  Copyright (C) 2008-2009 Massachusetts Institute of Technology          *
 *                                                                         *
 *  This source code is part of the PetaBricks project and currently only  *
 *  available internally within MIT.  This code may not be distributed     *
 *  outside of MIT. At some point in the future we plan to release this    *
 *  code (most likely GPL) to the public.  For more information, contact:  *
 *  Jason Ansel <jansel@csail.mit.edu>                                     *
 *                                                                         *
 *  A full list of authors may be found in the file AUTHORS.               *
 ***************************************************************************/
#ifndef PETABRICKSCHOICEDEPGRAPH_H
#define PETABRICKSCHOICEDEPGRAPH_H


#include "matrixdef.h"
#include "matrixdependency.h"
#include "region.h"
#include "rulechoice.h"

#include "common/jrefcounted.h"
#include "common/srcpos.h"

#include <list>
#include <map>
#include <vector>

namespace petabricks {
class Transform;
class ChoiceGrid;
class CodeGenerator;
typedef jalib::JRef<ChoiceGrid> ChoiceGridPtr;

class ChoiceDepGraphNode;
typedef jalib::JRef<ChoiceDepGraphNode> ChoiceDepGraphNodePtr;
typedef std::vector< ChoiceDepGraphNodePtr > ChoiceDepGraphNodeList; 
typedef std::map<ChoiceDepGraphNode*,ChoiceDepGraphNode*> ChoiceDepGraphNodeRemapping;

class ChoiceDepGraphNodeSet : public std::set<ChoiceDepGraphNode*> {
public:
  void applyRemapping(const petabricks::ChoiceDepGraphNodeRemapping& map);
  bool overlaps(const ChoiceDepGraphNodeSet& that){
    const_iterator i;
    for(i=begin(); i!=end(); ++i){
      if(that.find(*i)!=that.end())
        return true;
    }
    return false;
  }
};

struct DependencyInformation {
  RuleSet rules;
  DependencyDirection direction;

  ///
  /// Add that to this, true if information was updates
  bool merge(const DependencyInformation& that){
    size_t nRules = rules.size();
    DependencyDirection oldDir = direction;
    rules.insert(that.rules.begin(), that.rules.end());
    direction.addDirection(that.direction);
    return rules.size()!=nRules || oldDir!=direction;
  }

  bool contains(const RulePtr& p) const {
    return rules.find(p) != rules.end();
  }

};

class ScheduleDependencies : public std::map<ChoiceDepGraphNode*, DependencyInformation>{
public:
  void merge(const ScheduleDependencies& that){
    for(const_iterator i=that.begin(); i!=that.end(); ++i)
      operator[](i->first).merge(i->second);
  }

  void applyRemapping(const petabricks::ChoiceDepGraphNodeRemapping& map);
};

class ChoiceDepGraphNode : public jalib::JRefCounted,
                     public jalib::JPrintable,
                     public jalib::SrcPosTaggable,
                     public RuleChoiceConsumer {
public:
  ///
  /// Constructor
  ChoiceDepGraphNode();

  ///
  /// Add a dependency edge
  void addDependency(ChoiceDepGraphNode* n, const RulePtr& r, const DependencyDirection& dir){
    JASSERT(_directDependsRemapped.empty());
    _directDependsOriginal[n].rules.insert(r);
    _directDependsOriginal[n].direction.addDirection(dir);
  }

  ///
  /// Print this node name in graphviz/dot format
  void printNode(std::ostream& os) const;

  ///
  /// Print this node's edges in graphviz/dot format
  void printEdges(std::ostream& os) const;

  ///
  /// Name of this node as it appears in graphs
  std::string nodename() const { return "n"+jalib::XToString(_id); }


  ///
  /// Generate code for executing this node
  virtual void generateCode(Transform& trans,
                            CodeGenerator& o,
                            RuleFlavor flavor,
                            const RuleChoiceAssignment& choice) = 0;
  virtual void generateCodeForSlice(Transform& trans,
                                    CodeGenerator& o,
                                    int dimension,
                                    const FormulaPtr& pos,
                                    RuleFlavor flavor,
                                    const RuleChoiceAssignment& choice) = 0;

  virtual const MatrixDefPtr&    matrix() const = 0;
  virtual const SimpleRegionPtr& region() const = 0;
  virtual const RuleSet&  choices() const = 0;

  ScheduleDependencies& directDepends() {
    return _directDependsRemapped.empty() ? _directDependsOriginal : _directDependsRemapped;
  }
  const ScheduleDependencies& directDepends() const {
    return _directDependsRemapped.empty() ? _directDependsOriginal : _directDependsRemapped;
  }

  const ScheduleDependencies& directDependsOriginal() const { return _directDependsOriginal; }
  const ScheduleDependencies& directDependsRemapped() const { return _directDependsRemapped; }
  ScheduleDependencies& indirectDepends() { return _indirectDepends; }
  const ScheduleDependencies& indirectDepends() const { return _indirectDepends; }

  ///
  /// Run one iteration to update indirectDepends, return count of changes
  int updateIndirectDepends();

  ChoiceDepGraphNodeSet getStronglyConnectedComponent();
  
  ChoiceDepGraphNodeSet getMultioutputComponent();

  void applyRemapping(const ChoiceDepGraphNodeRemapping& map);
  void applyChoiceRemapping(const RuleChoiceAssignment& map);

  void resetRemapping();

  bool isInput() const { return _isInput; }
  bool isOutput() const { return _isOutput; }
  void markInput() { _isInput = true; }
  void markOutput() { _isOutput = true; }
  void markLast() { _isLast = true;  }

  std::string getChoicePrefix(Transform& t);

  virtual bool findValidSchedule(const RuleChoiceAssignment&) { return true; }
protected:
  int _id;
  bool _isInput;
  bool _isOutput;
  bool _isLast;
  ScheduleDependencies _directDependsOriginal;
  ScheduleDependencies _directDependsRemapped;
  ScheduleDependencies _indirectDepends;
  int _choiceId;
};

class BasicChoiceDepGraphNode : public ChoiceDepGraphNode {
public:
  BasicChoiceDepGraphNode(const MatrixDefPtr& m, const SimpleRegionPtr& r, const ChoiceGridPtr& choices);

  const MatrixDefPtr&    matrix() const { return _matrix; }
  const SimpleRegionPtr& region() const { return _region; }
  const RuleSet&        choices() const { return _choices; }

  void print(std::ostream& o) const {
    o << _matrix->name() << ".region(" << _region << ")";
  }

  void generateCode(Transform& trans, CodeGenerator& o, RuleFlavor flavor,
                            const RuleChoiceAssignment& choice);
  void generateCodeForSlice(Transform& trans, CodeGenerator& o, int dimension, const FormulaPtr& pos, RuleFlavor flavor,
                            const RuleChoiceAssignment& choice);
private:
  MatrixDefPtr      _matrix;
  SimpleRegionPtr   _region;
  RuleSet           _choices;
};

class MetaChoiceDepGraphNode : public ChoiceDepGraphNode {
public:
  MetaChoiceDepGraphNode(const ChoiceDepGraphNodeSet& set);

  const MatrixDefPtr&    matrix()  const { JASSERT(false); return MatrixDefPtr::null();    }
  const SimpleRegionPtr& region()  const { JASSERT(false); return SimpleRegionPtr::null(); }
  const RuleSet& choices() const { static RuleSet empty; return empty; }

  void print(std::ostream& o) const {
    o << classname() << ":";
    for(ChoiceDepGraphNodeSet::const_iterator i=_originalNodes.begin(); i!=_originalNodes.end(); ++i)
      o << "\\n " << **i;
  }
  
  void generateCode(Transform&,
                    CodeGenerator&,
                    RuleFlavor,
                    const RuleChoiceAssignment&){
    UNIMPLEMENTED();
  }
  void generateCodeForSlice(Transform&,
                            CodeGenerator&,
                            int,
                            const FormulaPtr&,
                            RuleFlavor,
                            const RuleChoiceAssignment& ){
    UNIMPLEMENTED();
  }

protected:
  virtual const char* classname() const { return "MetaChoiceDepGraphNode"; }

protected:
  ChoiceDepGraphNodeSet _originalNodes;
};

class MultiOutputChoiceDepGraphNode : public MetaChoiceDepGraphNode {
public:
  MultiOutputChoiceDepGraphNode(const ChoiceDepGraphNodeSet& set)
    : MetaChoiceDepGraphNode(set)
  {}
  bool findValidSchedule(const RuleChoiceAssignment& choice);
  
  void generateCode(Transform& trans, CodeGenerator& o, RuleFlavor flavor,
                            const RuleChoiceAssignment& choice);
protected:
  virtual const char* classname() const { return "MultiOutputChoiceDepGraphNode"; }

};


class SlicedChoiceDepGraphNode : public MetaChoiceDepGraphNode {
public:
  SlicedChoiceDepGraphNode(const ChoiceDepGraphNodeSet& set);
  

  bool findValidSchedule(const RuleChoiceAssignment& choice);

  void generateCode(Transform& trans, CodeGenerator& o, RuleFlavor flavor,
                            const RuleChoiceAssignment& choice);

protected:
  const char* classname() const { return "SlicedChoiceDepGraphNode"; }
private:
  int _dimension;
  bool _forward;
  FormulaPtr _begin;
  FormulaPtr _end;
};


}

#endif


