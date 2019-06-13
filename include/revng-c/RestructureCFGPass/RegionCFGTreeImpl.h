#ifndef REVNGC_RESTRUCTURE_CFG_REGIONCFGTREEIMPL_H
#define REVNGC_RESTRUCTURE_CFG_REGIONCFGTREEIMPL_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

// LLVM includes
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/GenericDomTreeConstruction.h"
#include "llvm/Support/raw_os_ostream.h"

// Local libraries includes
#include "revng-c/RestructureCFGPass/ASTTree.h"
#include "revng-c/RestructureCFGPass/BasicBlockNode.h"
#include "revng-c/RestructureCFGPass/MetaRegion.h"
#include "revng-c/RestructureCFGPass/RegionCFGTree.h"
#include "revng-c/RestructureCFGPass/Utils.h"

unsigned const SmallSetSize = 16;

// llvm::SmallPtrSet is a handy way to store set of BasicBlockNode pointers.
template<class NodeT>
using SmallPtrSet = llvm::SmallPtrSet<BasicBlockNode<NodeT> *, SmallSetSize>;

// Helper function that visit an AST tree and creates the sequence nodes
inline ASTNode *createSequence(ASTTree &Tree, ASTNode *RootNode) {
  SequenceNode *RootSequenceNode = Tree.addSequenceNode();
  RootSequenceNode->addNode(RootNode);

  for (ASTNode *Node : RootSequenceNode->nodes()) {
    if (auto *If = llvm::dyn_cast<IfNode>(Node)) {
      if (If->hasThen()) {
        If->setThen(createSequence(Tree, If->getThen()));
      }
      if (If->hasElse()) {
        If->setElse(createSequence(Tree, If->getElse()));
      }
    } else if (auto *Code = llvm::dyn_cast<CodeNode>(Node)) {
      // TODO: confirm that doesn't make sense to process a code node.
    } else if (auto *Scs = llvm::dyn_cast<ScsNode>(Node)) {
      // TODO: confirm that this phase is not needed since the processing is
      //       done inside the processing of each SCS region.
    }
  }

  return RootSequenceNode;
}

// Helper function that simplifies useless dummy nodes
inline void simplifyDummies(ASTNode *RootNode) {

  if (auto *Sequence = llvm::dyn_cast<SequenceNode>(RootNode)) {
    std::vector<ASTNode *> UselessDummies;

    for (ASTNode *Node : Sequence->nodes()) {
      if (Node->isEmpty()) {
        UselessDummies.push_back(Node);
      } else {
        simplifyDummies(Node);
      }
    }

    for (ASTNode *Node : UselessDummies) {
      Sequence->removeNode(Node);
    }

  } else if (auto *If = llvm::dyn_cast<IfNode>(RootNode)) {
    if (If->hasThen()) {
      simplifyDummies(If->getThen());
    }
    if (If->hasElse()) {
      simplifyDummies(If->getElse());
    }
  }
}

// Helper function which simplifies sequence nodes composed by a single AST
// node.
inline ASTNode *simplifyAtomicSequence(ASTNode *RootNode) {
  if (auto *Sequence = llvm::dyn_cast<SequenceNode>(RootNode)) {
    if (Sequence->listSize() == 0) {
      RootNode = nullptr;
    } else if (Sequence->listSize() == 1) {
      RootNode = Sequence->getNodeN(0);
      RootNode = simplifyAtomicSequence(RootNode);
    } else {
      for (ASTNode *Node : Sequence->nodes()) {
        Node = simplifyAtomicSequence(Node);
      }
    }
  } else if (auto *If = llvm::dyn_cast<IfNode>(RootNode)) {
    if (If->hasThen()) {
      If->setThen(simplifyAtomicSequence(If->getThen()));
    }
    if (If->hasElse()) {
      If->setElse(simplifyAtomicSequence(If->getElse()));
    }
  } else if (auto *Scs = llvm::dyn_cast<ScsNode>(RootNode)) {
    // TODO: check if this is not needed as the simplification is done for each
    //       SCS region.
    // After flattening this situation may arise again.
    if (Scs->hasBody()) {
      Scs->setBody(simplifyAtomicSequence(Scs->getBody()));
    }
  }

  return RootNode;
}

template<class NodeT>
inline bool predecessorsVisited(BasicBlockNode<NodeT> *Node,
                                SmallPtrSet<NodeT> &Visited) {

  bool State = true;

  // Cycles through all the predecessor, as soon as we find a predecessor not
  // already visited, set the state to false and break.
  for (BasicBlockNode<NodeT> *Predecessor : Node->predecessors()) {
    if (Visited.count(Predecessor) == 0) {
      State = false;
      break;
    }
  }

  return State;
}

template<class NodeT>
inline bool nodeVisited(BasicBlockNode<NodeT> *Node,
                        SmallPtrSet<NodeT> &Visited) {
  if (Visited.count(Node) != 0) {
    return true;
  } else {
    return false;
  }
}

template<class NodeT>
inline void RegionCFG<NodeT>::setFunctionName(std::string Name) {
  FunctionName = Name;
}

template<class NodeT>
inline void RegionCFG<NodeT>::setRegionName(std::string Name) {
  RegionName = Name;
}

template<class NodeT>
inline std::string RegionCFG<NodeT>::getFunctionName() const {
  return FunctionName;
}

template<class NodeT>
inline std::string RegionCFG<NodeT>::getRegionName() const {
  return RegionName;
}

template<class NodeT>
inline BasicBlockNode<NodeT> *RegionCFG<NodeT>::addNode(NodeT Node) {
  llvm::StringRef Name = Node->getName();
  BlockNodes.emplace_back(std::make_unique<BasicBlockNodeT>(this, Node, Name));
  BasicBlockNodeT *Result = BlockNodes.back().get();
  revng_log(CombLogger,
            "Building " << Name << " at address: " << Result << "\n");
  return Result;
}

template<class NodeT>
inline BasicBlockNode<NodeT> *
RegionCFG<NodeT>::cloneNode(BasicBlockNodeT &OriginalNode) {
  BlockNodes.emplace_back(std::make_unique<BasicBlockNodeT>(OriginalNode,
                                                            this));
  BasicBlockNodeT *New = BlockNodes.back().get();
  New->setName(OriginalNode.getName().str() + " cloned");
  return New;
}

template<class NodeT>
inline void RegionCFG<NodeT>::removeNode(BasicBlockNodeT *Node) {

  revng_log(CombLogger, "Removing node named: " << Node->getNameStr() << "\n");

  for (BasicBlockNodeT *Predecessor : Node->predecessors()) {
    Predecessor->removeSuccessor(Node);
  }

  for (BasicBlockNodeT *Successor : Node->successors()) {
    Successor->removePredecessor(Node);
  }

  for (auto It = BlockNodes.begin(); It != BlockNodes.end(); It++) {
    if ((*It).get() == Node) {
      BlockNodes.erase(It);
      break;
    }
  }
}

template<class NodeT>
using BBNodeT = typename RegionCFG<NodeT>::BasicBlockNodeT;

template<class NodeT>
inline void copyNeighbors(BBNodeT<NodeT> *Dst, BBNodeT<NodeT> *Src) {
  for (BBNodeT<NodeT> *Succ : Src->successors())
    Dst->addSuccessor(Succ);
  for (BBNodeT<NodeT> *Pred : Src->predecessors())
    Dst->addPredecessor(Pred);
}

template<class NodeT>
inline void RegionCFG<NodeT>::insertBulkNodes(BasicBlockNodeTSet &Nodes,
                                              BasicBlockNodeT *Head,
                                              BBNodeMap &SubMap) {
  revng_assert(BlockNodes.empty());

  for (BasicBlockNodeT *Node : Nodes) {
    BlockNodes.emplace_back(std::make_unique<BasicBlockNodeT>(*Node, this));
    BasicBlockNodeT *New = BlockNodes.back().get();
    SubMap[Node] = New;
    // The copy constructor used above does not bring along the successors and
    // the predecessors, neither adjusts the parent.
    // The following lines are a hack to fix this problem, but they momentarily
    // build a broken data structure where the predecessors and the successors
    // of the New BasicBlockNodes in *this still refer to the BasicBlockNodes in
    // the Parent CFGRegion of Nodes. This will be fixed later by updatePointers
    copyNeighbors<NodeT>(New, Node);
  }

  revng_assert(Head != nullptr);
  EntryNode = SubMap[Head];
  revng_assert(EntryNode != nullptr);
  // Fix the hack above
  for (BasicBlockNodeTUP &Node : BlockNodes)
    Node->updatePointers(SubMap);
}

template<class NodeT>
using lk_iterator = typename RegionCFG<NodeT>::links_container::iterator;

template<class NodeT>
inline llvm::iterator_range<lk_iterator<NodeT>>
RegionCFG<NodeT>::copyNodesAndEdgesFrom(RegionCFGT *O, BBNodeMap &SubMap) {
  size_t NumCurrNodes = size();

  for (BasicBlockNode<NodeT> *Node : *O) {
    BlockNodes.emplace_back(std::make_unique<BasicBlockNodeT>(*Node, this));
    BasicBlockNode<NodeT> *New = BlockNodes.back().get();
    SubMap[Node] = New;
    copyNeighbors<NodeT>(New, Node);
  }

  internal_iterator BeginInserted = BlockNodes.begin() + NumCurrNodes;
  internal_iterator EndInserted = BlockNodes.end();
  using MovedIteratorRange = llvm::iterator_range<internal_iterator>;
  MovedIteratorRange Result = llvm::make_range(BeginInserted, EndInserted);
  for (std::unique_ptr<BasicBlockNode<NodeT>> &NewNode : Result)
    NewNode->updatePointers(SubMap);
  return Result;
}

template<class NodeT>
inline void RegionCFG<NodeT>::connectBreakNode(std::set<EdgeDescriptor> &Out,
                                               const BBNodeMap &SubMap) {
  for (EdgeDescriptor Edge : Out) {

    // Create a new break for each outgoing edge.
    BasicBlockNode<NodeT> *Break = addBreak();
    if (not Edge.first->isCheck()) {
      addEdge(EdgeDescriptor(SubMap.at(Edge.first), Break));
    } else {
      revng_assert(Edge.second == Edge.first->getTrue()
                   or Edge.second == Edge.first->getFalse());
      if (Edge.second == Edge.first->getTrue())
        SubMap.at(Edge.first)->setTrue(Break);
      else
        SubMap.at(Edge.first)->setFalse(Break);
    }
  }
}

template<class NodeT>
inline void RegionCFG<NodeT>::connectContinueNode() {
  BasicBlockNodeTVect ContinueNodes;

  // We need to pre-save the edges to avoid breaking the predecessor iterator
  for (BasicBlockNode<NodeT> *Source : EntryNode->predecessors()) {
    ContinueNodes.push_back(Source);
  }
  for (BasicBlockNode<NodeT> *Source : ContinueNodes) {

    // Create a new continue node for each retreating edge.
    BasicBlockNode<NodeT> *Continue = addContinue();
    moveEdgeTarget(EdgeDescriptor(Source, EntryNode), Continue);
  }
}

template<class NodeT>
inline std::vector<BasicBlockNode<NodeT> *>
RegionCFG<NodeT>::orderNodes(BasicBlockNodeTVect &L, bool DoReverse) {
  BasicBlockNodeTSet ToOrder;
  ToOrder.insert(L.begin(), L.end());
  llvm::ReversePostOrderTraversal<BasicBlockNode<NodeT> *> RPOT(EntryNode);
  BasicBlockNodeTVect Result;

  if (DoReverse) {
    std::reverse(RPOT.begin(), RPOT.end());
  }

  for (BasicBlockNode<NodeT> *RPOTBB : RPOT) {
    if (ToOrder.count(RPOTBB) != 0) {
      Result.push_back(RPOTBB);
    }
  }

  revng_assert(L.size() == Result.size());

  return Result;
}

template<class NodeT>
template<typename StreamT>
inline void
RegionCFG<NodeT>::streamNode(StreamT &S, const BasicBlockNodeT *BB) const {
  unsigned NodeID = BB->getID();
  S << "\"" << NodeID << "\"";
  S << " ["
    << "label=\"ID: " << NodeID << " Name: " << BB->getNameStr() << "\"";
  if (BB == EntryNode)
    S << ",fillcolor=green,style=filled";
  S << "];\n";
}

/// \brief Dump a GraphViz file on stdout representing this function
template<class NodeT>
template<typename StreamT>
inline void RegionCFG<NodeT>::dumpDot(StreamT &S) const {
  S << "digraph CFGFunction {\n";

  for (const std::unique_ptr<BasicBlockNode<NodeT>> &BB : BlockNodes) {
    streamNode(S, BB.get());
    for (auto &Successor : BB->successors()) {
      unsigned PredID = BB->getID();
      unsigned SuccID = Successor->getID();
      S << "\"" << PredID << "\""
        << " -> \"" << SuccID << "\"";
      if (BB->isCheck() and BB->getFalse() == Successor)
        S << " [color=red];\n";
      else
        S << " [color=green];\n";
    }
  }
  S << "}\n";
}

template<class NodeT>
inline void RegionCFG<NodeT>::dumpDotOnFile(std::string FolderName,
                                            std::string FunctionName,
                                            std::string FileName) const {
  std::ofstream DotFile;
  std::string PathName = FolderName + "/" + FunctionName;
  mkdir(FolderName.c_str(), 0775);
  mkdir(PathName.c_str(), 0775);
  DotFile.open(PathName + "/" + FileName + ".dot");
  if (DotFile.is_open()) {
    dumpDot(DotFile);
    DotFile.close();
  } else {
    revng_abort("Could not open file for dumping dot file.");
  }
}

template<class NodeT>
inline void RegionCFG<NodeT>::dumpDotOnFile(std::string FileName) const {
  std::ofstream DotFile;
  DotFile.open(FileName);
  if (DotFile.is_open()) {
    dumpDot(DotFile);
    DotFile.close();
  } else {
    revng_abort("Could not open file for dumping dot.");
  }
}

template<class NodeT>
inline std::vector<BasicBlockNode<NodeT> *> RegionCFG<NodeT>::purgeDummies() {
  RegionCFG<NodeT> &Graph = *this;
  bool AnotherIteration = true;
  BasicBlockNodeTVect RemovedNodes;

  while (AnotherIteration) {
    AnotherIteration = false;

    for (auto It = Graph.begin(); It != Graph.end(); It++) {
      if (((*It)->isEmpty()) and ((*It)->predecessor_size() == 1)
          and ((*It)->successor_size() == 1)) {

        if (CombLogger.isEnabled()) {
          CombLogger << "Purging dummy node " << (*It)->getNameStr() << "\n";
        }

        BasicBlockNode<NodeT> *Predecessor = (*It)->getPredecessorI(0);
        BasicBlockNode<NodeT> *Successor = (*It)->getSuccessorI(0);

        BasicBlockNode<NodeT> *RemovedNode = *It;

        // Connect directly predecessor and successor, and remove the dummy node
        // under analysis
        RemovedNodes.push_back(RemovedNode);

        // Connect directly predecessor and successor, and remove the dummy node
        // under analysis
        moveEdgeTarget({ Predecessor, *It }, Successor);
        Graph.removeNode(*It);

        AnotherIteration = true;
        break;
      }
    }
  }

  return RemovedNodes;
}

template<class NodeT>
inline void RegionCFG<NodeT>::purgeVirtualSink(BasicBlockNode<NodeT> *Sink) {

  RegionCFG<NodeT> &Graph = *this;

  BasicBlockNodeTVect WorkList;
  BasicBlockNodeTVect PurgeList;

  WorkList.push_back(Sink);

  while (!WorkList.empty()) {
    BasicBlockNode<NodeT> *CurrentNode = WorkList.back();
    WorkList.pop_back();

    if (CurrentNode->isEmpty()) {
      PurgeList.push_back(CurrentNode);

      for (BasicBlockNode<NodeT> *Predecessor : CurrentNode->predecessors()) {
        WorkList.push_back(Predecessor);
      }
    }
  }

  for (BasicBlockNode<NodeT> *Purge : PurgeList) {
    Graph.removeNode(Purge);
  }
}

template<class NodeT>
inline std::vector<BasicBlockNode<NodeT> *>
RegionCFG<NodeT>::getInterestingNodes(BasicBlockNodeT *Cond) {

  RegionCFG<NodeT> &Graph = *this;

  // Retrieve the immediate postdominator.
  llvm::DomTreeNodeBase<BasicBlockNode<NodeT>> *PostBase = PDT[Cond]->getIDom();
  BasicBlockNode<NodeT> *PostDominator = PostBase->getBlock();

  BasicBlockNodeTSet Candidates = findReachableNodes(*Cond, *PostDominator);

  BasicBlockNodeTVect NotDominatedCandidates;
  for (BasicBlockNode<NodeT> *Node : Candidates) {
    if (!DT.dominates(Cond, Node)) {
      NotDominatedCandidates.push_back(Node);
    }
  }

  // TODO: Check that this is the order that we want.
  NotDominatedCandidates = Graph.orderNodes(NotDominatedCandidates, true);

  return NotDominatedCandidates;
}

inline bool isGreater(unsigned Op1, unsigned Op2) {
  unsigned MultiplicativeFactor = 1;
  if (Op1 > (MultiplicativeFactor * Op2)) {
    return true;
  } else {
    return false;
  }
}

template<class NodeT>
inline BasicBlockNode<NodeT>
*RegionCFG<NodeT>::cloneUntilExit(BasicBlockNode<NodeT> *Node,
                                  BasicBlockNode<NodeT> *Sink) {

  // Clone the postdominator node.
  BBNodeMap CloneMap;
  BasicBlockNode<NodeT> *Clone = cloneNode(*Node);

  // Insert the postdominator clone in the map.
  CloneMap[Node] = Clone;

  BasicBlockNodeTVect WorkList;
  WorkList.push_back(Node);

  // Set of nodes which have been already processed.
  BasicBlockNodeTSet AlreadyProcessed;

  while (!WorkList.empty()) {
    BasicBlockNode<NodeT> *CurrentNode = WorkList.back();
    WorkList.pop_back();

    // Ensure that we are not processing the sink node.
    revng_assert(CurrentNode != Sink);

    if (AlreadyProcessed.count(CurrentNode) == 0) {
      AlreadyProcessed.insert(CurrentNode);
    } else {
      continue;
    }

    // Get the clone of the `CurrentNode`.
    BasicBlockNode<NodeT> *CurrentClone = CloneMap.at(CurrentNode);

    bool ConnectSink = false;
    for (BasicBlockNode<NodeT> *Successor : CurrentNode->successors()) {

      // If our successor is the sink, create and edge that directly connects
      // it.
      if (Successor == Sink) {
        ConnectSink = true;
      } else {
        BasicBlockNode<NodeT> *SuccessorClone = nullptr;

        // The clone of the successor node already exists.
        if (CloneMap.count(Successor)) {
          SuccessorClone = CloneMap.at(Successor);
        } else {

          // The clone of the successor does not exist, create it in place.
          SuccessorClone = cloneNode(*Successor);
          CloneMap[Successor] = SuccessorClone;
        }

        // Create the edge to the clone of the successor.
        revng_assert(SuccessorClone != nullptr);
        if (CurrentClone->isCheck()) {
          revng_assert(CurrentNode->isCheck());

          // Check if we need to connect the `then` or `else` branch.
          if (CurrentNode->getTrue() == Successor) {
            CurrentClone->setTrue(SuccessorClone);
          } else if (CurrentNode->getFalse() == Successor) {
            CurrentClone->setFalse(SuccessorClone);
          } else {
            revng_abort("Succesor is not then neither else.");
          }
        } else {
          addEdge(EdgeDescriptor(CurrentClone, SuccessorClone));
        }

        // Add the successor to the worklist.
        WorkList.push_back(Successor);
      }
    }

    if (ConnectSink) {
      addEdge(EdgeDescriptor(CurrentClone, Sink));
    }
  }

  return Clone;
}

template<class NodeT>
inline void RegionCFG<NodeT>::untangle() {

  revng_assert(isDAG());

  RegionCFG<NodeT> &Graph = *this;

  DT.recalculate(Graph);
  PDT.recalculate(Graph);

  // Collect all the conditional nodes in the graph.
  BasicBlockNodeTVect ConditionalNodes;
  for (auto It = Graph.begin(); It != Graph.end(); It++) {
    if ((*It)->successor_size() == 2) {
      ConditionalNodes.push_back(*It);
    }
  }

  // Map to retrieve the post dominator for each conditional node.
  BBNodeMap PostDominatorMap;

  // Collect entry and exit nodes.
  BasicBlockNode<NodeT> *EntryNode = &Graph.getEntryNode();
  BasicBlockNodeTVect ExitNodes;
  for (auto It = Graph.begin(); It != Graph.end(); It++) {
    if ((*It)->successor_size() == 0) {
      ExitNodes.push_back(*It);
    }
  }

  // Add a new virtual sink node to computer the postdominator.
  BasicBlockNode<NodeT> *Sink = Graph.addArtificialNode();
  for (BasicBlockNode<NodeT> *Exit : ExitNodes) {
    addEdge(EdgeDescriptor(Exit, Sink));
  }

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("untangle",
                        FunctionName,
                        "Region-" + RegionName + "-initial-state");
  }

  DT.recalculate(Graph);
  PDT.recalculate(Graph);

  // Compute the immediate post-dominator for each conditional node.
  for (BasicBlockNode<NodeT> *Conditional : ConditionalNodes) {
    BasicBlockNode<NodeT> *PostDom = PDT[Conditional]->getIDom()->getBlock();
    revng_assert(PostDom != nullptr);
    PostDominatorMap[Conditional] = PostDom;
  }

  // Map which contains the precomputed wheight for each node in the graph. In
  // case of a code node the weight will be equal to the number of instruction
  // in the original basic block; in case of a collapsed node the weight will be
  // the sum of the weights of all the nodes contained in the collapsed graph.
  std::map<BasicBlockNode<NodeT> *, unsigned> WeightMap;
  for (BasicBlockNode<NodeT> *Node : Graph.nodes()) {
    WeightMap[Node] = Node->getWeight();
  }

  // Order the conditional nodes in postorder.
  ConditionalNodes = Graph.orderNodes(ConditionalNodes, false);

  while (!ConditionalNodes.empty()) {
    if (CombLogger.isEnabled()) {
      Graph.dumpDotOnFile("untangle",
                          FunctionName,
                          "Region-" + RegionName + "-debug");
    }
    BasicBlockNode<NodeT> *Conditional = ConditionalNodes.back();
    ConditionalNodes.pop_back();

    // Update the information of the dominator and postdominator trees.
    DT.recalculate(Graph);
    PDT.recalculate(Graph);

    // Get the immediate postdominator.
    BasicBlockNode<NodeT> *PostDominator = PostDominatorMap[Conditional];

    // Ensure that we have both the successors.
    revng_assert(Conditional->successor_size() == 2);

    // Get the first node of the then and else branches respectively.
    // TODO: Check that this is the right way to do this. At this point we
    //       cannot assume that we have the `getThen()` and `getFalse()`
    //       methods.
    BasicBlockNode<NodeT> *ThenChild = Conditional->getSuccessorI(0);
    BasicBlockNode<NodeT> *ElseChild = Conditional->getSuccessorI(1);

    // Collect all the nodes laying between the branches
    BasicBlockNodeTSet ThenNodes = findReachableNodes(*ThenChild,
                                                      *PostDominator);

    BasicBlockNodeTSet ElseNodes = findReachableNodes(*ElseChild,
                                                      *PostDominator);

    // Remove the postdominator from both the sets.
    ThenNodes.erase(PostDominator);
    ElseNodes.erase(PostDominator);

    BasicBlockNodeTVect NotDominatedThenNodes;
    for (BasicBlockNode<NodeT> *Node : ThenNodes) {
      if (!DT.dominates(Conditional, Node)) {
        NotDominatedThenNodes.push_back(Node);
      }
    }

    BasicBlockNodeTVect NotDominatedElseNodes;
    for (BasicBlockNode<NodeT> *Node : ElseNodes) {
      if (!DT.dominates(Conditional, Node)) {
        NotDominatedElseNodes.push_back(Node);
      }
    }

    // Check that we fully dominate at least one of the two branches (this may
    // be a conservative assumption).
    if (NotDominatedThenNodes.size() > 0 and NotDominatedElseNodes.size() > 0) {
      continue;
    }

    // Check that the set of nodes reachable from the `then` and `else` child
    // nodes are disjointed (this may be a conservative assumption).
    BasicBlockNodeTVect Intersection;
    std::set_intersection(ThenNodes.begin(),
                          ThenNodes.end(),
                          ElseNodes.begin(),
                          ElseNodes.end(),
                          std::back_inserter(Intersection));


    if (Intersection.size() > 0) {
      continue;
    }

    // Compute the weight of the `then` and `else` branches.
    unsigned ThenWeight = 0;
    unsigned ElseWeight = 0;

    for (BasicBlockNode<NodeT> *Node : NotDominatedThenNodes) {
      ThenWeight += WeightMap[Node];
    }

    for (BasicBlockNode<NodeT> *Node : NotDominatedElseNodes) {
      ElseWeight += WeightMap[Node];
    }

    // The weight of the nodes placed after the immediate postdominator is the
    // sum of all the weights of the nodes which are reachable starting from the
    // immediate post dominator and the sink node (to which all the exits have
    // been connected).
    unsigned PostDominatorWeight = 0;
    BasicBlockNodeTSet PostDominatorToExit = findReachableNodes(*PostDominator,
                                                                *Sink);

    for (BasicBlockNode<NodeT> *Node : PostDominatorToExit) {
      PostDominatorWeight += WeightMap[Node];
    }

    // Criterion which decides if we can apply the untangle optimization to the
    // conditional under analysis.
    // We define 3 weights:
    // - 1) weight(then) + weight(else)
    // - 2) weight(then) + weight(postdom)
    // - 3) weight(else) + weight(postdom)
    //
    // We need to operate the split if:
    // 2 >> 3
    // 1 >> 3
    // and specifically we need to split the `else` branch.
    //
    // We need to operate the split if:
    // 3 >> 2
    // 1 >> 2
    // and specifically we need to split the `then` branch.
    //
    // We can also define in a dynamic way the >> operator, so we can change the
    // threshold that triggers the split.
    unsigned OneWeight = ThenWeight + ElseWeight;
    unsigned TwoWeight = ThenWeight + PostDominatorWeight;
    unsigned ThreeWeight = ElseWeight + PostDominatorWeight;

    if (isGreater(TwoWeight, ThreeWeight)
        and isGreater(OneWeight, ThreeWeight)
        and PostDominator != Sink) {
      revng_log(CombLogger, FunctionName << ":");
      revng_log(CombLogger, RegionName << ":");
      revng_log(CombLogger,
                "Found untangle candidate then " << Conditional->getNameStr());
      revng_log(CombLogger, "Weight 1:" << OneWeight);
      revng_log(CombLogger, "Weight 2:" << TwoWeight);
      revng_log(CombLogger, "Weight 3:" << ThreeWeight);

      revng_log(CombLogger, "Actually splitting node");
      BasicBlockNode<NodeT> *PostDominatorClone = cloneUntilExit(PostDominator,
                                                                 Sink);
      BasicBlockNodeTVect Predecessors;
      for (BasicBlockNode<NodeT> *Predecessor : PostDominator->predecessors()) {
        Predecessors.push_back(Predecessor);
      }

      for (BasicBlockNode<NodeT> *Predecessor : Predecessors) {

        // We need to move the edge so that it points to the new clone if the
        // `ElseChild` dominates the edge (meaning we are inlining the `else`
        // side) or if the source of the edge is the conditional node itself
        // (meaning that the conditional node is connected to the postdominator
        // itself, so we don't actually have the `ElseChild`)
        if (DT.dominates(ElseChild, Predecessor)
            or Predecessor == Conditional) {
          moveEdgeTarget(EdgeDescriptor(Predecessor, PostDominator),
                         PostDominatorClone);
        }
      }

      // Check that we actually moved some edges.
      revng_assert(PostDominatorClone->predecessor_size() > 0);
    }

    if (isGreater(ThreeWeight, TwoWeight)
        and isGreater(OneWeight, TwoWeight)
        and PostDominator != Sink) {
      revng_log(CombLogger, FunctionName << ":");
      revng_log(CombLogger, RegionName << ":");
      revng_log(CombLogger,
                "Found untangle candidate else " << Conditional->getNameStr());
      revng_log(CombLogger, "Weight 1:" << OneWeight);
      revng_log(CombLogger, "Weight 2:" << TwoWeight);
      revng_log(CombLogger, "Weight 3:" << ThreeWeight);

      revng_log(CombLogger, "Actually splitting node");
      BasicBlockNode<NodeT> *PostDominatorClone = cloneUntilExit(PostDominator,
                                                                 Sink);

      BasicBlockNodeTVect Predecessors;
      for (BasicBlockNode<NodeT> *Predecessor : PostDominator->predecessors()) {
        Predecessors.push_back(Predecessor);
      }

      for (BasicBlockNode<NodeT> *Predecessor : Predecessors) {

        // We need to move the edge so that it points to the new clone if the
        // `ThenChild` dominates the edge (meaning we are inlining the `then`
        // side) or if the source of the edge is the conditional node itself
        // (meaning that the conditional node is connected to the postdominator
        // itself, so we don't actually have the `ThenChild`)
        if (DT.dominates(ThenChild, Predecessor)
            or Predecessor == Conditional) {
          moveEdgeTarget(EdgeDescriptor(Predecessor, PostDominator),
                         PostDominatorClone);
        }
      }

      // Check that we actually moved some edges.
      revng_assert(PostDominatorClone->predecessor_size() > 0);
    }
  }

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("untangle",
                        FunctionName,
                        "Region-" + RegionName + "-after-processing");
  }

  // Remove the sink node.
  purgeVirtualSink(Sink);

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("untangle",
                        FunctionName,
                        "Region-" + RegionName + "-after-sink-removal");
  }
}

template<class NodeT>
inline void RegionCFG<NodeT>::inflate() {

  // Call the untangle preprocessing.
  untangle();

  revng_assert(isDAG());

  // Apply the comb to a RegionCFG object.
  // TODO: handle all the collapsed regions.
  RegionCFG<NodeT> &Graph = *this;

  // Collect entry and exit nodes.
  BasicBlockNode<NodeT> *EntryNode = &Graph.getEntryNode();
  BasicBlockNodeTVect ExitNodes;
  for (auto It = Graph.begin(); It != Graph.end(); It++) {
    if ((*It)->successor_size() == 0) {
      ExitNodes.push_back(*It);
    }
  }

  if (CombLogger.isEnabled()) {
    CombLogger << "The entry node is:\n";
    CombLogger << EntryNode->getNameStr() << "\n";
    CombLogger << "In the graph the exit nodes are:\n";
    for (BasicBlockNode<NodeT> *Node : ExitNodes) {
      CombLogger << Node->getNameStr() << "\n";
    }
  }

  // Helper data structure for exit reachability computation.
  BasicBlockNodeTSet ConditionalBlacklist;
  std::map<BasicBlockNode<NodeT> *, BasicBlockNodeTSet> ReachableExits;

  // Collect nodes reachable from each exit node in the graph.
  for (BasicBlockNode<NodeT> *Exit : ExitNodes) {
    CombLogger << "From exit node: " << Exit->getNameStr() << "\n";
    CombLogger << "We can reach:\n";
    for (BasicBlockNode<NodeT> *Node : llvm::inverse_depth_first(Exit)) {
      CombLogger << Node->getNameStr() << "\n";
      ReachableExits[Node].insert(Exit);
    }
  }

  // Dump graph before virtual sink add.
  if (CombLogger.isEnabled()) {
    CombLogger << "Graph before sink addition is:\n";
    Graph.dumpDotOnFile("inflates",
                        FunctionName,
                        "Region-" + RegionName + "-before-sink");
  }

  // Add a new virtual sink node to which all the exit nodes are connected.
  BasicBlockNode<NodeT> *Sink = Graph.addArtificialNode();
  for (BasicBlockNode<NodeT> *Exit : ExitNodes) {
    addEdge(EdgeDescriptor(Exit, Sink));
  }

  // Dump graph after virtual sink add.
  if (CombLogger.isEnabled()) {
    CombLogger << "Graph after sink addition is:\n";
    Graph.dumpDotOnFile("inflates",
                        FunctionName,
                        "Region-" + RegionName + "-after-sink");
  }

  // Refresh information of dominator tree.
  DT.recalculate(Graph);

  // Collect all the conditional nodes in the graph.
  // This is the working list of conditional nodes on which we will operate and
  // will contain only the filtered conditionals.
  BasicBlockNodeTVect ConditionalNodes;

  // This set contains all the conditional nodes present in the graph
  BasicBlockNodeTSet ConditionalNodesComplete;

  for (auto It = Graph.begin(); It != Graph.end(); It++) {
    revng_assert((*It)->successor_size() < 3);
    if ((*It)->successor_size() == 2) {

      // Check that the intersection of exits nodes reachable from the then and
      // else branches are not disjoint
      BasicBlockNodeTSet ThenExits = ReachableExits[(*It)->getSuccessorI(0)];
      BasicBlockNodeTSet ElseExits = ReachableExits[(*It)->getSuccessorI(1)];
      BasicBlockNodeTVect Intersection;
      std::set_intersection(ThenExits.begin(),
                            ThenExits.end(),
                            ElseExits.begin(),
                            ElseExits.end(),
                            std::back_inserter(Intersection));

      // Check that we do not dominate at maximum on of the two sets of
      // reachable exits.
      bool ThenIsDominated = true;
      bool ElseIsDominated = true;
      for (BasicBlockNode<NodeT> *Exit : ThenExits) {
        if (not DT.dominates(*It, Exit)) {
          ThenIsDominated = false;
        }
      }
      for (BasicBlockNode<NodeT> *Exit : ElseExits) {
        if (not DT.dominates(*It, Exit)) {
          ElseIsDominated = false;
        }
      }

      // This check adds a conditional nodes if the sets of reachable exits are
      // not disjoint or if we do not dominate both the reachable exit sets
      // (note that we may not dominate one of the two reachable sets, meaning
      // the fallthrough branch, but we need to dominate the other in such a way
      // that we can completely absorb it).
      if (Intersection.size() != 0
          or (not(ThenIsDominated or ElseIsDominated))) {
        ConditionalNodes.push_back(*It);
        ConditionalNodesComplete.insert(*It);
      } else {
        CombLogger << "Blacklisted conditional: " << (*It)->getNameStr()
                   << "\n";
      }
    }
  }

  // TODO: reverse this order, with std::vector I can only pop_back.
  ConditionalNodes = Graph.orderNodes(ConditionalNodes, false);

  if (CombLogger.isEnabled()) {
    CombLogger << "Conditional nodes present in the graph are:\n";
    for (BasicBlockNode<NodeT> *Node : ConditionalNodes) {
      CombLogger << Node->getNameStr() << "\n";
    }
  }

  // Map to retrieve the post dominator for each conditional node.
  BBNodeMap PostDominatorMap;

  // Equivalence-class like set to keep track of all the cloned nodes created
  // starting from an original node.
  std::map<BasicBlockNode<NodeT> *, SmallPtrSet<NodeT>> NodesEquivalenceClass;

  // Map to keep track of the cloning relationship.
  BBNodeMap CloneToOriginalMap;

  // Initialize a list containing the reverse post order of the nodes of the
  // graph.
  std::list<BasicBlockNode<NodeT> *> RevPostOrderList;
  llvm::ReversePostOrderTraversal<BasicBlockNode<NodeT> *> RPOT(EntryNode);
  for (BasicBlockNode<NodeT> *RPOTBB : RPOT) {
    RevPostOrderList.push_back(RPOTBB);
    NodesEquivalenceClass[RPOTBB].insert(RPOTBB);
    CloneToOriginalMap[RPOTBB] = RPOTBB;
  }

  // Refresh information of dominator and postdominator trees.
  DT.recalculate(Graph);
  PDT.recalculate(Graph);

  #if 1
  // Compute the immediate post-dominator for each conditional node.
  for (BasicBlockNode<NodeT> *Conditional : ConditionalNodes) {
    BasicBlockNode<NodeT> *PostDom = PDT[Conditional]->getIDom()->getBlock();
    revng_assert(PostDom != nullptr);
    PostDominatorMap[Conditional] = PostDom;
  }
  #endif

  while (!ConditionalNodes.empty()) {

    // List to keep track of the nodes that we still need to analyze.
    SmallPtrSet<NodeT> WorkList;

    // Process each conditional node after ordering it.
    BasicBlockNode<NodeT> *Conditional = ConditionalNodes.back();
    ConditionalNodes.pop_back();
    if (CombLogger.isEnabled()) {
      CombLogger << "Analyzing conditional node " << Conditional->getNameStr()
                 << "\n";
      Graph.dumpDotOnFile("inflates",
                          FunctionName,
                          "Region-" + RegionName + "-conditional-"
                            + Conditional->getNameStr() + "-begin");
    }

    // Enqueue in the worklist the successors of the contional node.
    for (BasicBlockNode<NodeT> *Successor : Conditional->successors()) {
      WorkList.insert(Successor);
    }

    // Keep a set of the visited nodes for the current conditional node.
    SmallPtrSet<NodeT> Visited;
    Visited.insert(Conditional);

    // Get an iterator from the reverse post order list in the position of the
    // conditional node.
    typename std::list<BasicBlockNode<NodeT> *>::iterator ListIt = RevPostOrderList.begin();
    while (&**ListIt != Conditional and ListIt != RevPostOrderList.end()) {
      ListIt++;
    }

    revng_assert(ListIt != RevPostOrderList.end());

    int Iteration = 0;
    while (!WorkList.empty()) {

      // Retrieve a reference to the set of postdominators.
      // TODO:: verify that this is safe in case of insertions.
      BasicBlockNode<NodeT> *PostDom = PostDominatorMap[Conditional];
      SmallPtrSet<NodeT> &PostDomSet = NodesEquivalenceClass[PostDom];

      // Postdom flag, which is useful to understand if the dummies we will
      // insert will need to substitute the current postdominator.
      bool IsPostDom = false;
      ListIt++;

      // Scan the working list and the reverse post order in a parallel manner.
      BasicBlockNode<NodeT> *Candidate = nullptr;
      BasicBlockNode<NodeT> *NextInList = *ListIt;

      // If the next node is in the worklist analyze it.
      if (WorkList.count(NextInList) != 0) {
        Candidate = NextInList;

        // We reached a post dominator node of the region.
        if (PostDomSet.count(Candidate) != 0) {
          if (!predecessorsVisited(Candidate, Visited)) {
            // The post dominator has some edges incoming from node we have not
            // already visited.
            IsPostDom = true;
            Visited.insert(Candidate);
            WorkList.erase(Candidate);
          } else {
            // We can analyze the next conditional node.
            break;
          }

        } else {
          // We have not reached a post dominator.
          if (!predecessorsVisited(Candidate, Visited)) {
            // We have not visited all the node incoming in the node
            Visited.insert(Candidate);
            WorkList.erase(Candidate);
            for (BasicBlockNode<NodeT> *Successor : Candidate->successors()) {
              WorkList.insert(Successor);
            }
          } else {
            Visited.insert(Candidate);
            WorkList.erase(Candidate);
            for (BasicBlockNode<NodeT> *Successor : Candidate->successors()) {
              WorkList.insert(Successor);
            }
            continue;
          }
        }
      } else {

        // Go to the next node in reverse postorder.
        continue;
      }

      revng_assert(Candidate != nullptr);

      if (CombLogger.isEnabled()) {
        CombLogger << "Analyzing candidate nodes\n ";
      }
      if (CombLogger.isEnabled()) {
        CombLogger << "Analyzing candidate " << Candidate->getNameStr() << "\n";
      }

      // Decide wether to insert a dummy or to duplicate.
      if (Candidate->predecessor_size() > 2 and IsPostDom) {

        // Insert a dummy node.
        if (CombLogger.isEnabled()) {
          CombLogger << "Inserting a dummy node for ";
          CombLogger << Candidate->getNameStr() << "\n";
        }

        BasicBlockNode<NodeT> *Dummy = Graph.addArtificialNode();

        // Insert the dummy nodes in the reverse post order list. The insertion
        // order is particularly relevant, since the re-exploration of the dummy
        // which we dominate depends on this.
        RevPostOrderList.insert(ListIt, Dummy);

        // Remove from the visited set the node which triggered the creation of
        // of the dummy nodes.
        Visited.erase(Candidate);
        WorkList.insert(Candidate);

        // Move back the iterator on the reverse post order (we want it to point
        // to the left dummy). We need to move it two position back, since at
        // each iteration the iteration is moved one position forward.
        ListIt--;
        ListIt--;

        // Initialize the equivalence class of the dummy node.
        NodesEquivalenceClass[Dummy] = { Dummy };

        // If the candidate node we are analyzing is a postdominator, substitute
        // the postdominator with the right dummy.
        if (IsPostDom and (!Candidate->isEmpty() or Candidate == Sink)) {
          PostDominatorMap[Conditional] = Dummy;
        }

        // The new dummy node does not lead back to any original node, for this
        // reason we need to insert a new entry in the `CloneToOriginalMap`.
        CloneToOriginalMap[Dummy] = Dummy;

        // Mark the dummy to explore.
        WorkList.insert(Dummy);

        BasicBlockNodeTVect Predecessors;

        CombLogger << "Current predecessors are:\n";
        for (BasicBlockNode<NodeT> *Predecessor : Candidate->predecessors()) {
          CombLogger << Predecessor->getNameStr() << "\n";
          Predecessors.push_back(Predecessor);
        }

        for (BasicBlockNode<NodeT> *Predecessor : Predecessors) {
          if (CombLogger.isEnabled()) {
            CombLogger << "Moving edge from predecessor ";
            CombLogger << Predecessor->getNameStr() << "\n";
          }
          if (nodeVisited(Predecessor, Visited)) {
            moveEdgeTarget(EdgeDescriptor(Predecessor, Candidate),
                           Dummy);
          }
        }

        addEdge(EdgeDescriptor(Dummy, Candidate));

      } else {

        // Duplicate node.
        if (CombLogger.isEnabled()) {
          CombLogger << "Duplicating node for ";
          CombLogger << Candidate->getNameStr() << "\n";
        }

        BasicBlockNode<NodeT> *Duplicated = Graph.cloneNode(*Candidate);
        revng_assert(Duplicated != nullptr);

        // Insert the cloned node in the reverse post order list.
        RevPostOrderList.insert(ListIt, Duplicated);

        // Add the cloned node in the equivalence class of the original node.
        revng_assert(CloneToOriginalMap.count(Candidate) != 0);
        BasicBlockNode<NodeT> *OriginalNode = CloneToOriginalMap[Candidate];
        CloneToOriginalMap[Duplicated] = OriginalNode;
        NodesEquivalenceClass[OriginalNode].insert(Duplicated);

        // If the node we are duplicating is a conditional node, add it to the
        // working list of the conditional nodes.
        if (ConditionalNodesComplete.count(Candidate) != 0) {
          ConditionalNodes.push_back(Duplicated);
          ConditionalNodesComplete.insert(Duplicated);
          PostDominatorMap[Duplicated] = PostDominatorMap[Candidate];
        }

        // Specifically handle the check idx node situation.
        if (Candidate->isCheck()) {
          revng_assert(Candidate->getTrue() != nullptr
                       and Candidate->getFalse() != nullptr);
          BasicBlockNode<NodeT> *TrueSuccessor = Candidate->getTrue();
          BasicBlockNode<NodeT> *FalseSuccessor = Candidate->getFalse();
          Duplicated->setTrue(TrueSuccessor);
          Duplicated->setFalse(FalseSuccessor);

        } else {
          for (BasicBlockNode<NodeT> *Successor : Candidate->successors()) {
            addEdge(EdgeDescriptor(Duplicated, Successor));
          }
        }
        BasicBlockNodeTVect Predecessors;

        for (BasicBlockNode<NodeT> *Predecessor : Candidate->predecessors()) {
          Predecessors.push_back(Predecessor);
        }

        for (BasicBlockNode<NodeT> *Predecessor : Predecessors) {
          if (!nodeVisited(Predecessor, Visited)) {
            moveEdgeTarget(EdgeDescriptor(Predecessor, Candidate), Duplicated);
          }
        }
      }

      if (CombLogger.isEnabled()) {
        Graph.dumpDotOnFile("inflates",
                            FunctionName,
                            "Region-" + RegionName + "-conditional-"
                              + Conditional->getNameStr() + "-"
                              + std::to_string(Iteration) + "-before-purge");
      }

      // Purge extra dummies at each iteration
      BasicBlockNodeTVect RemovedNodes = purgeDummies();
      for (BasicBlockNode<NodeT> *ToRemove : RemovedNodes) {
        Visited.erase(ToRemove);
        WorkList.erase(ToRemove);

        // Update iterator if we are removing it.
        if (ToRemove == *ListIt) {
          ListIt--;
        }
        RevPostOrderList.remove(ToRemove);
      }

      if (CombLogger.isEnabled()) {
        Graph.dumpDotOnFile("inflates",
                            FunctionName,
                            "Region-" + RegionName + "-conditional-"
                              + Conditional->getNameStr() + "-"
                              + std::to_string(Iteration));
      }
      Iteration++;
    }

    revng_log(CombLogger, "Finished looking at: ");
    revng_log(CombLogger, Conditional->getNameStr() << "\n");
  }

  // Purge extra dummy nodes introduced.
  purgeDummies();
  purgeVirtualSink(Sink);

  if (CombLogger.isEnabled()) {
    CombLogger << "Graph after combing is:\n";
    Graph.dumpDotOnFile("inflates",
                        FunctionName,
                        "Region-" + RegionName + "-after-combing");
  }
}

template<class NodeT>
inline void RegionCFG<NodeT>::generateAst() {

  RegionCFG<NodeT> &Graph = *this;

  // Apply combing to the current RegionCFG.

  if (ToInflate) {
    if (CombLogger.isEnabled()) {
      CombLogger << "Inflating region " + RegionName + "\n";
      dumpDotOnFile("dots", FunctionName, "PRECOMB");
    }

    Graph.inflate();
    ToInflate = false;
    if (CombLogger.isEnabled()) {
      dumpDotOnFile("dots", FunctionName, "POSTCOMB");
    }
  }

  // TODO: factorize out the AST generation phase.
  llvm::DominatorTreeBase<BasicBlockNode<NodeT>, false> ASTDT;
  ASTDT.recalculate(Graph);
  ASTDT.updateDFSNumbers();

  CombLogger.emit();

  std::map<int, BasicBlockNode<NodeT> *> DFSNodeMap;

  // Compute the ideal order of visit for creating AST nodes.
  for (BasicBlockNode<NodeT> *Node : Graph.nodes()) {
    DFSNodeMap[ASTDT[Node]->getDFSNumOut()] = Node;
  }

  // Visiting order of the dominator tree.
  if (CombLogger.isEnabled()) {
    for (auto &Pair : DFSNodeMap) {
      CombLogger << Pair.second->getNameStr() << "\n";
    }
  }

  for (auto &Pair : DFSNodeMap) {
    BasicBlockNode<NodeT> *Node = Pair.second;

    // Collect the children nodes in the dominator tree.
    std::vector<llvm::DomTreeNodeBase<BasicBlockNode<NodeT>> *>
      Children = ASTDT[Node]->getChildren();

    std::vector<ASTNode *> ASTChildren;
    BasicBlockNodeTVect BBChildren;
    for (llvm::DomTreeNodeBase<BasicBlockNode<NodeT>> *TreeNode : Children) {
      BasicBlockNode<NodeT> *BlockNode = TreeNode->getBlock();
      ASTNode *ASTPointer = AST.findASTNode(BlockNode);
      ASTChildren.push_back(ASTPointer);
      BBChildren.push_back(BlockNode);
    }

    // Check that the two vector have the same size.
    revng_assert(Children.size() == ASTChildren.size());

    // Handle collapsded node.
    if (Node->isCollapsed()) {
      revng_assert(ASTChildren.size() <= 1);
      if (ASTChildren.size() == 1) {
        RegionCFG<NodeT> *BodyGraph = Node->getCollapsedCFG();
        revng_assert(BodyGraph != nullptr);
        CombLogger << "Inspecting collapsed node: " << Node->getNameStr()
                   << "\n";
        CombLogger.emit();
        BodyGraph->generateAst();
        ASTNode *Body = BodyGraph->getAST().getRoot();
        std::unique_ptr<ASTNode> ASTObject(new ScsNode(Node,
                                                       Body,
                                                       ASTChildren[0]));
        AST.addASTNode(Node, std::move(ASTObject));
      } else {
        RegionCFG<NodeT> *BodyGraph = Node->getCollapsedCFG();
        CombLogger << "Inspecting collapsed node: " << Node->getNameStr()
                   << "\n";
        CombLogger.emit();
        BodyGraph->generateAst();
        ASTNode *Body = BodyGraph->getAST().getRoot();
        std::unique_ptr<ASTNode> ASTObject(new ScsNode(Node, Body));
        AST.addASTNode(Node, std::move(ASTObject));
      }
    } else {
      revng_assert(Children.size() < 4);
      std::unique_ptr<ASTNode> ASTObject;
      if (Children.size() == 3) {
        revng_assert(not Node->isBreak() and not Node->isContinue()
                     and not Node->isSet());

        // If we are creating the AST for the check node, create the adequate
        // AST node preserving the then and else branches, otherwise create a
        // classical node.
        if (Node->isCheck()) {
          if (BBChildren[0] == Node->getTrue()
              and BBChildren[2] == Node->getFalse()) {
            ASTObject.reset(new IfCheckNode(Node,
                                            ASTChildren[0],
                                            ASTChildren[2],
                                            ASTChildren[1]));
          } else if (BBChildren[2] == Node->getTrue()
                     and BBChildren[0] == Node->getFalse()) {
            ASTObject.reset(new IfCheckNode(Node,
                                            ASTChildren[2],
                                            ASTChildren[0],
                                            ASTChildren[1]));
          } else {
            revng_abort("Then and else branches cannot be matched");
          }
        } else {
          // Create the conditional expression associated with the if node.
          using UniqueExpr = std::unique_ptr<ExprNode>;
          auto *OriginalNode = Node->getOriginalNode();
          UniqueExpr CondExpr = std::make_unique<AtomicNode>(OriginalNode);
          ExprNode *CondExprNode = AST.addCondExpr(std::move(CondExpr));
          ASTObject.reset(new IfNode(Node,
                                     CondExprNode,
                                     ASTChildren[0],
                                     ASTChildren[2],
                                     ASTChildren[1]));
        }
      } else if (Children.size() == 2) {
        revng_assert(not Node->isBreak() and not Node->isContinue()
                     and not Node->isSet());

        // If we are creating the AST for the switch tree, create the adequate,
        // AST node, otherwise create a classical node.
        if (Node->isCheck()) {
          if (BBChildren[0] == Node->getTrue()
              and BBChildren[1] == Node->getFalse()) {
            ASTObject.reset(new IfCheckNode(Node,
                                            ASTChildren[0],
                                            ASTChildren[1],
                                            nullptr));
          } else if (BBChildren[1] == Node->getTrue()
                     and BBChildren[0] == Node->getFalse()) {
            ASTObject.reset(new IfCheckNode(Node,
                                            ASTChildren[1],
                                            ASTChildren[0],
                                            nullptr));
          } else {
            revng_abort("Then and else branches cannot be matched");
          }
        } else {
          // Create the conditional expression associated with the if node.
          using UniqueExpr = std::unique_ptr<ExprNode>;
          auto *OriginalNode = Node->getOriginalNode();
          UniqueExpr CondExpr = std::make_unique<AtomicNode>(OriginalNode);
          ExprNode *CondExprNode = AST.addCondExpr(std::move(CondExpr));
          ASTObject.reset(new IfNode(Node,
                                     CondExprNode,
                                     ASTChildren[0],
                                     ASTChildren[1],
                                     nullptr));
        }
      } else if (Children.size() == 1) {
        revng_assert(not Node->isBreak() and not Node->isContinue());
        if (Node->isSet()) {
          ASTObject.reset(new SetNode(Node, ASTChildren[0]));
        } else if (Node->isCheck()) {

          // We may have a check node with a single then/else branch due to
          // condition blacklisting (the other branch is the fallthrough
          // branch).
          if (BBChildren[0] == Node->getTrue()) {
            ASTObject.reset(new IfCheckNode(Node,
                                            ASTChildren[0],
                                            nullptr,
                                            nullptr));
          } else if (BBChildren[0] == Node->getFalse()) {
            ASTObject.reset(new IfCheckNode(Node,
                                            nullptr,
                                            ASTChildren[0],
                                            nullptr));
          }
        } else {
          ASTObject.reset(new CodeNode(Node, ASTChildren[0]));
        }
      } else if (Children.size() == 0) {
        if (Node->isBreak())
          ASTObject.reset(new BreakNode());
        else if (Node->isContinue())
          ASTObject.reset(new ContinueNode());
        else if (Node->isSet())
          ASTObject.reset(new SetNode(Node));
        else if (Node->isEmpty() or Node->isCode())
          ASTObject.reset(new CodeNode(Node, nullptr));
        else
          revng_abort();
      }
      AST.addASTNode(Node, std::move(ASTObject));
    }
  }

  // Set in the ASTTree object the root node.
  BasicBlockNode<NodeT> *Root = ASTDT.getRootNode()->getBlock();
  ASTNode *RootNode = AST.findASTNode(Root);

  // Serialize the graph starting from the root node.
  CombLogger << "Serializing first AST draft:\n";
  AST.setRoot(RootNode);
  if (CombLogger.isEnabled()) {
    AST.dumpOnFile("ast", FunctionName, "First-draft");
  }

  // Create sequence nodes.
  CombLogger << "Performing sequence insertion:\n";
  RootNode = createSequence(AST, RootNode);
  AST.setRoot(RootNode);
  if (CombLogger.isEnabled()) {
    AST.dumpOnFile("ast", FunctionName, "After-sequence");
  }

  // Simplify useless sequence nodes.
  CombLogger << "Performing useless dummies simplification:\n";
  simplifyDummies(RootNode);
  if (CombLogger.isEnabled()) {
    AST.dumpOnFile("ast", FunctionName, "After-dummies-removal");
  }

  // Simplify useless sequence nodes.
  CombLogger << "Performing useless sequence simplification:\n";
  RootNode = simplifyAtomicSequence(RootNode);
  AST.setRoot(RootNode);
  if (CombLogger.isEnabled()) {
    AST.dumpOnFile("ast", FunctionName, "After-sequence-simplification");
  }

  // Remove danling nodes (possibly created by the de-optimization pass, after
  // disconnecting the first CFG node corresponding to the simplified AST node),
  // and superfluos dummy nodes
  removeNotReachables();
  purgeDummies();
}

// Get reference to the AST object which is inside the RegionCFG object
template<class NodeT>
inline ASTTree &RegionCFG<NodeT>::getAST() {
  return AST;
}

template<class NodeT>
inline void RegionCFG<NodeT>::removeNotReachables() {

  // Remove nodes that have no predecessors (nodes that are the result of node
  // cloning and that remains dandling around).
  bool Difference = true;
  while (Difference) {
    Difference = false;
    BasicBlockNode<NodeT> *EntryNode = &getEntryNode();
    for (auto It = begin(); It != end(); It++) {
      if ((EntryNode != *It and (*It)->predecessor_size() == 0)) {

        removeNode(*It);
        Difference = true;
        break;
      }
    }
  }
}

template<class NodeT>
inline void
RegionCFG<NodeT>::removeNotReachables(std::vector<MetaRegion<NodeT> *> &MS) {

  // Remove nodes that have no predecessors (nodes that are the result of node
  // cloning and that remains dandling around).
  bool Difference = true;
  while (Difference) {
    Difference = false;
    BasicBlockNode<NodeT> *EntryNode = &getEntryNode();
    for (auto It = begin(); It != end(); It++) {
      if ((EntryNode != *It and (*It)->predecessor_size() == 0)) {
        for (MetaRegion<NodeT> *M : MS) {
          M->removeNode(*It);
        }
        removeNode(*It);
        Difference = true;
        break;
      }
    }
  }
}

template<class NodeT>
inline bool RegionCFG<NodeT>::isDAG() {
  bool FoundSCC = false;

  for (llvm::scc_iterator<RegionCFG<NodeT> *> I = llvm::scc_begin(this),
                                              IE = llvm::scc_end(this);
       I != IE;
       ++I) {
    const std::vector<BasicBlockNode<NodeT> *> &SCC = *I;
    if (SCC.size() != 1) {
      FoundSCC = true;
    } else {
      BasicBlockNode<NodeT> *Node = SCC[0];
      for (BasicBlockNode<NodeT> *Successor : Node->successors()) {
        if (Successor == Node) {
          FoundSCC = true;
        }
      }
    }
  }

  return not FoundSCC;
}

template<class NodeT>
inline bool
RegionCFG<NodeT>::isTopologicallyEquivalent(RegionCFG &Other) const {

  // The algorithm inspects in a depth first fashion the two graphs, and check
  // that they are topologically equivalent. Take care that this function may
  // return true if there are nodes not reachable from the entry node.

  // Early failure if the number of nodes composing the two CFG is different.
  if (size() != Other.size()) {
    return false;
  }

  // Retrieve the entry nodes of the two `RegionCFG` under analysis.
  BasicBlockNode<NodeT> &Entry = getEntryNode();
  BasicBlockNode<NodeT> &OtherEntry = Other.getEntryNode();

  // Check that the only node without predecessors is the entry node.
  for (const BasicBlockNode<NodeT> *Node : nodes()) {
    if (Node != &Entry and Node->predecessor_size() == 0) {
      return false;
    }
  }

  // Check that the only node without predecessors is the entry node.
  for (const BasicBlockNode<NodeT> *Node : Other.nodes()) {
    if (Node != &OtherEntry and Node->predecessor_size() == 0) {
      return false;
    }
  }

  // Call to a `BasicBlockNode` method which does a deep and recursive
  // comparison of a node and its successors.
  return Entry.isEquivalentTo(&OtherEntry);
}

#endif // REVNGC_RESTRUCTURE_CFG_REGIONCFGTREEIMPL_H